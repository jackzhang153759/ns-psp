// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <vector>

#include "Common/System/System.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeMap.h"
#include "Core/Loaders.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/PSPLoaders.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/CoreTiming.h"
#include "Core/Reporting.h"
#include "Core/MIPS/MIPS.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceUmd.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/KernelWaitHelpers.h"
#include "Core/RetroAchievements.h"

#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/FileSystems/VirtualDiscFileSystem.h"

static constexpr u64 MICRO_DELAY_ACTIVATE = 4000;
// Does not include PSP_UMD_CHANGED.
static constexpr uint32_t UMD_STAT_ALLOW_WAIT = PSP_UMD_NOT_PRESENT | PSP_UMD_PRESENT | PSP_UMD_NOT_READY | PSP_UMD_READY | PSP_UMD_READABLE;

static bool umdActivated = true;
static u32 umdStatus = 0;
static u32 umdErrorStat = 0;
static int driveCBId = 0;
static int umdStatTimeoutEvent = -1;
static int umdStatChangeEvent = -1;
static int umdInsertChangeEvent = -1;
// Separate from umdStatChangeEvent on purpose: that one models sceUmdActivate/Deactivate and only
// sets umdActivated + wakes waiters, it does not notify the shell. This one is the second half of
// the insert notification (see __UmdInsertReady).
static int umdInsertReadyEvent = -1;
static std::vector<SceUID> umdWaitingThreads;
static std::map<SceUID, u64> umdPausedWaits;

bool g_UMDReplacePermit = false;
bool UMDInserted = true;
// The image a disc swap or insert put in the drive. Empty if it's still the one we booted with.
static Path g_umdPath;

// Path of the image in the drive, for the host side (see GetInsertedUmdPath()). Deliberately not
// reset by __UmdInit(): the HLE modules are torn down on every LoadExec, but the disc stays in
// the drive across a LoadExec - the game that boots from it needs to find this path afterwards.
static Path umdImagePath;

const Path &GetInsertedUmdPath() {
	return umdImagePath;
}

// What kind of disc is in the drive, as reported by sceUmdGetDiscInfo(). The shell decides from
// this single field which partition to read: a game disc is expected to carry
// disc0:/PSP_GAME/PARAM.SFO, a UMD Video disc carries disc0:/UMD_VIDEO/ instead. Always answering
// GAME makes the XMB open the game partition of a disc that has none, which surfaces as "the disc
// could not be read". Set once per insert by __UmdReplace() from the disc's actual contents.
static u32 umdDiscType = PSP_UMD_TYPE_GAME;

static const char *UmdDiscTypeName(u32 type) {
	switch (type) {
	case PSP_UMD_TYPE_VIDEO: return "video";
	case PSP_UMD_TYPE_AUDIO: return "audio";
	case PSP_UMD_TYPE_GAME: return "game";
	default: return "unknown";
	}
}

// Single entry point for the type so that every reset point logs it the same way and none of them
// can silently forget to clear it.
static void __UmdSetDiscType(u32 type) {
	umdDiscType = type;
	printf("[umd] disc type = 0x%02x (%s)\n", (unsigned)type, UmdDiscTypeName(type));
}

// Drive-state tracing (bounded, observational). A shell that waits for a state bit we never report
// blocks until its own timer runs out - a multi-second stall followed by "the disc could not be
// read" - and nothing else in the log can show that, because no filesystem call is ever made.
// The bits we currently ever report come from __KernelUmdGetState().
static constexpr int UMD_STATE_LOG_BUDGET = 24;
static int g_umdStateLogCount = 0;

static bool UmdStateLog() {
	if (g_umdStateLogCount < UMD_STATE_LOG_BUDGET) {
		g_umdStateLogCount++;
		return true;
	}
	if (g_umdStateLogCount == UMD_STATE_LOG_BUDGET) {
		g_umdStateLogCount++;
		printf("[umdio] umd state log budget exhausted (%d lines)\n", UMD_STATE_LOG_BUDGET);
	}
	return false;
}

// sceUmdGetDriveStat is polled, so it gets its own tiny budget: sharing one with the wait calls
// would let a chatty shell starve the very lines that matter.
static constexpr int UMD_GETSTAT_LOG_BUDGET = 6;
static int g_umdGetStatLogCount = 0;

static bool UmdGetStatLog() {
	if (g_umdGetStatLogCount < UMD_GETSTAT_LOG_BUDGET) {
		g_umdGetStatLogCount++;
		return true;
	}
	return false;
}

struct PspUmdInfo {
	u32_le size;
	u32_le type;
};

static void __UmdStatTimeout(u64 userdata, int cyclesLate);
static void __UmdStatChange(u64 userdata, int cyclesLate);
static void __UmdInsertChange(u64 userdata, int cyclesLate);
static void __UmdInsertReady(u64 userdata, int cyclesLate);
static void __UmdBeginCallback(SceUID threadID, SceUID prevCallbackId);
static void __UmdEndCallback(SceUID threadID, SceUID prevCallbackId);

void __UmdInit()
{
	umdStatTimeoutEvent = CoreTiming::RegisterEvent("UmdTimeout", __UmdStatTimeout);
	umdStatChangeEvent = CoreTiming::RegisterEvent("UmdChange", __UmdStatChange);
	umdInsertChangeEvent = CoreTiming::RegisterEvent("UmdInsertChange", __UmdInsertChange);
	umdInsertReadyEvent = CoreTiming::RegisterEvent("UmdInsertReady", __UmdInsertReady);
	umdActivated = true;
	umdStatus = 0;
	umdErrorStat = 0;
	driveCBId = 0;
	umdWaitingThreads.clear();
	umdPausedWaits.clear();
	g_UMDReplacePermit = false;
	g_umdPath.clear();
	// The XMB ejects the disc when it boots without one, and the next game boot mustn't inherit
	// the empty drive.
	UMDInserted = true;

	// The disc type survives a LoadExec for the same reason umdImagePath does: the disc stays in
	// the drive, and the shell that is being re-entered (or the title booting from it) has to see
	// the type that was determined when it went in. Only an empty drive resets it.
	if (umdImagePath.empty()) {
		__UmdSetDiscType(PSP_UMD_TYPE_GAME);
	}

	__KernelRegisterWaitTypeFuncs(WAITTYPE_UMD, __UmdBeginCallback, __UmdEndCallback);
}

void __UmdDoState(PointerWrap &p)
{
	auto s = p.Section("sceUmd", 1, 3);
	if (!s)
		return;

	u8 activatedByte = umdActivated ? 1 : 0;
	Do(p, activatedByte);
	umdActivated = activatedByte != 0;
	Do(p, umdStatus);
	Do(p, umdErrorStat);
	Do(p, driveCBId);
	Do(p, umdStatTimeoutEvent);
	CoreTiming::RestoreRegisterEvent(umdStatTimeoutEvent, "UmdTimeout", __UmdStatTimeout);
	Do(p, umdStatChangeEvent);
	CoreTiming::RestoreRegisterEvent(umdStatChangeEvent, "UmdChange", __UmdStatChange);
	Do(p, umdWaitingThreads);
	Do(p, umdPausedWaits);

	if (s > 1) {
		Do(p, g_UMDReplacePermit);
		if (g_UMDReplacePermit && p.mode == p.MODE_READ) {
			System_Notify(SystemNotification::UI);
		}
	} else if (p.mode == p.MODE_READ) {
		g_UMDReplacePermit = false;
	}
	if (s > 2) {
		Do(p, umdInsertChangeEvent);
		Do(p, UMDInserted);
	} else {
		umdInsertChangeEvent = -1;
		UMDInserted = true;
	}
	CoreTiming::RestoreRegisterEvent(umdInsertChangeEvent, "UmdInsertChange", __UmdInsertChange);
}

static u8 __KernelUmdGetState() {
	if (!UMDInserted) {
		return PSP_UMD_NOT_PRESENT;
	}

	// Most games seem to expect the disc to be ready early on, active or not.
	// It seems like the PSP sets this state when the disc is "ready".
	u8 state = PSP_UMD_PRESENT | PSP_UMD_READY;
	if (umdActivated) {
		state |= PSP_UMD_READABLE;
	}
	return state;
}

static void UmdWakeThreads() {
	// Wake anyone waiting on this.
	for (size_t i = 0; i < umdWaitingThreads.size(); ++i) {
		const SceUID threadID = umdWaitingThreads[i];

		u32 error;
		u32 stat = __KernelGetWaitValue(threadID, error);
		bool keep = false;
		if (HLEKernel::VerifyWait(threadID, WAITTYPE_UMD, 1)) {
			// Only if they are still waiting do we keep them in the list.
			keep = (stat & __KernelUmdGetState()) == 0;
			if (!keep) {
				__KernelResumeThreadFromWait(threadID, 0);
			}
		}

		if (!keep) {
			umdWaitingThreads.erase(umdWaitingThreads.begin() + i--);
		}
	}
}

static void __UmdStatChange(u64 userdata, int cyclesLate) {
	umdActivated = userdata != 0;
	// Rare (only sceUmdActivate/Deactivate), so it is worth an unconditional line.
	printf("[umd] stat change -> activated=%d state=0x%02x\n", umdActivated ? 1 : 0,
		(unsigned)__KernelUmdGetState());

	UmdWakeThreads();
}

// Callback notifications don't queue: a second one before the callback runs replaces the first's
// argument. So a disc that goes in before the shell has run the callback for the one that came out
// (both done from the pause menu) would look to it like nothing changed.
static bool DriveCallbackPending() {
	u32 error;
	const PSPCallback *cb = driveCBId != 0 ? kernelObjects.Get<PSPCallback>(driveCBId, error) : nullptr;
	return cb && cb->nc.notifyCount != 0;
}

// userdata 1: an insert waiting for the drive callback to see the drive empty, which then gets
// the callback itself. 0: the second half of a disc swap.
static void __UmdInsertChange(u64 userdata, int cyclesLate) {
	if (userdata == 1 && DriveCallbackPending()) {
		CoreTiming::ScheduleEvent(usToCycles(100 * 1000), umdInsertChangeEvent, 1);
		return;
	}

	UMDInserted = true;

	UmdWakeThreads();
	if (userdata == 1 && driveCBId != 0) {
		__KernelNotifyCallback(driveCBId, PSP_UMD_PRESENT | PSP_UMD_READABLE | PSP_UMD_CHANGED);
	}
}

// Second half of the insert notification: the disc has spun up, so it is readable from here on.
//
// Real hardware notifies twice - first "the medium changed" (PRESENT|CHANGED, not readable yet),
// then, once the drive reports ready, another callback with PRESENT|READY|READABLE|CHANGED. We used
// to send only the first half with READABLE set, which leaves a shell that waits for the ready
// callback waiting forever: it can see the disc, reads UMD_DATA.BIN, and then has nothing to react
// to. Note this deliberately does not go through __UmdStatChange, which models
// sceUmdActivate/Deactivate and never notifies the shell at all.
static void __UmdInsertReady(u64 userdata, int cyclesLate) {
	if (!UMDInserted) {
		// Ejected during the spin-up: nothing to report.
		printf("[umd] notify(ready) skipped, disc was ejected\n");
		return;
	}
	// Deliberately does NOT force umdActivated: on hardware inserting a disc does not activate the
	// drive, sceUmdActivate does (and the shell already calls it - see the log). Forcing it here
	// would also resurrect a drive a game had spun down on purpose. The shell's own Activate is
	// what puts READABLE into the state below.
	const u32 state = __KernelUmdGetState();
	// CHANGED is what the medium-change callback carries on hardware and stays set until the shell
	// has consumed it.
	const u32 readyArg = state | PSP_UMD_CHANGED;
	printf("[umd] notify(ready) arg=0x%02x state=0x%02x activated=%d\n", readyArg, (unsigned)state,
		umdActivated ? 1 : 0);
	if (driveCBId != 0) {
		__KernelNotifyCallback(driveCBId, readyArg);
	}
	UmdWakeThreads();
}

static void __KernelUmdActivate()
{
	u32 notifyArg = PSP_UMD_PRESENT | PSP_UMD_READABLE;
	// PSP_UMD_READY will be returned when sceKernelGetCompiledSdkVersion() != 0
	if (sceKernelGetCompiledSdkVersion() != 0) {
		notifyArg |= PSP_UMD_READY;
	}
	// An empty drive (the XMB's) says so: the shell otherwise asks for the disc and reports it unreadable.
	if (!UMDInserted) {
		notifyArg = PSP_UMD_NOT_PRESENT;
	}
	if (driveCBId != 0)
		__KernelNotifyCallback(driveCBId, notifyArg);

	// Don't activate immediately, take time to "spin up."
	CoreTiming::RemoveEvent(umdStatChangeEvent);
	CoreTiming::ScheduleEvent(usToCycles(MICRO_DELAY_ACTIVATE), umdStatChangeEvent, 1);
}

static void __KernelUmdDeactivate()
{
	u32 notifyArg = UMDInserted ? PSP_UMD_PRESENT | PSP_UMD_READY : PSP_UMD_NOT_PRESENT;
	if (driveCBId != 0)
		__KernelNotifyCallback(driveCBId, notifyArg);

	CoreTiming::RemoveEvent(umdStatChangeEvent);
	__UmdStatChange(0, 0);
}

static void __UmdBeginCallback(SceUID threadID, SceUID prevCallbackId)
{
	SceUID pauseKey = prevCallbackId == 0 ? threadID : prevCallbackId;

	if (HLEKernel::VerifyWait(threadID, WAITTYPE_UMD, 1))
	{
		// Shouldn't happen: each nesting level pauses under its own key, and on hardware a callback can
		// nest only one level (a CB wait that would go deeper never returns.)
		if (umdPausedWaits.find(pauseKey) != umdPausedWaits.end())
			return;

		_dbg_assert_msg_(umdStatTimeoutEvent != -1, "Must have a umd timer");
		s64 cyclesLeft = CoreTiming::UnscheduleEvent(umdStatTimeoutEvent, threadID);
		if (cyclesLeft != 0)
			umdPausedWaits[pauseKey] = CoreTiming::GetTicks(currentMIPS) + cyclesLeft;
		else
			umdPausedWaits[pauseKey] = 0;

		HLEKernel::RemoveWaitingThread(umdWaitingThreads, threadID);

		DEBUG_LOG(Log::sceIo, "sceUmdWaitDriveStatCB: Suspending lock wait for callback");
	}
	else
		WARN_LOG_REPORT(Log::sceIo, "sceUmdWaitDriveStatCB: beginning callback with bad wait id?");
}

static void __UmdEndCallback(SceUID threadID, SceUID prevCallbackId)
{
	SceUID pauseKey = prevCallbackId == 0 ? threadID : prevCallbackId;

	u32 error;
	u32 stat = __KernelGetWaitValue(threadID, error);
	if (umdPausedWaits.find(pauseKey) == umdPausedWaits.end())
	{
		WARN_LOG_REPORT(Log::sceIo, "__UmdEndCallback(): UMD paused wait missing");

		__KernelResumeThreadFromWait(threadID, 0);
		return;
	}

	u64 waitDeadline = umdPausedWaits[pauseKey];
	umdPausedWaits.erase(pauseKey);

	// TODO: Don't wake up if __KernelCurHasReadyCallbacks()?

	if ((stat & __KernelUmdGetState()) != 0)
	{
		__KernelResumeThreadFromWait(threadID, 0);
		return;
	}

	s64 cyclesLeft = waitDeadline - CoreTiming::GetTicks(currentMIPS);
	if (cyclesLeft < 0 && waitDeadline != 0)
		__KernelResumeThreadFromWait(threadID, SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	else
	{
		_dbg_assert_msg_(umdStatTimeoutEvent != -1, "Must have a umd timer");
		// A deadline of 0 means the wait has no timeout.
		if (waitDeadline != 0)
			CoreTiming::ScheduleEvent(cyclesLeft, umdStatTimeoutEvent, __KernelGetCurThread());

		umdWaitingThreads.push_back(threadID);

		DEBUG_LOG(Log::sceIo, "sceUmdWaitDriveStatCB: Resuming lock wait for callback");
	}
}

// UMD HLE call tracing (bounded). The drive's own state machine is the other place a multi-second
// stall can come from - a wait that is never satisfied burns its caller's timeout - so each of the
// entry points the shell uses is logged once, in order, with its result.
static constexpr int UMD_HLE_LOG_BUDGET = 32;
static int g_umdHleLogCount = 0;

static bool UmdHleLog() {
	if (g_umdHleLogCount < UMD_HLE_LOG_BUDGET) {
		g_umdHleLogCount++;
		return true;
	}
	if (g_umdHleLogCount == UMD_HLE_LOG_BUDGET) {
		g_umdHleLogCount++;
		printf("[umd] HLE log budget exhausted (%d lines)\n", UMD_HLE_LOG_BUDGET);
	}
	return false;
}

// True when cbId is the drive callback the shell registered. Used by the diagnostic in
// __KernelNotifyCallback() to tell an insert notification from the dozens of other callbacks.

// What the drive holds, as PSP_UMD_TYPE_* bits - and 0 for an empty drive, which is what the
// firmware's own UMD driver answers when there is no disc. Cached rather than re-read on every
// call: this is polled, and DetectUmdDiscType() walks the mounted disc to do its job.
u32 __UmdDiscType() {
	return UMDInserted ? umdDiscType : 0;
}

bool __UmdIsDriveCallback(int cbId) {
	return cbId != 0 && driveCBId != 0 && cbId == (int)driveCBId;
}

static int sceUmdCheckMedium() {
	int retVal = 0;
	if (UMDInserted) {
		retVal = 1; //non-zero: disc in drive
	}
	if (UmdHleLog()) {
		printf("[umd] HLE sceUmdCheckMedium() -> %d (0x%08x)\n", retVal, (unsigned)retVal);
	}
	return hleLogDebug(Log::sceKernel, retVal);
}
	
// A user mode caller can't pass a kernel address. (mediaman.prx checks these with k1.)
static bool IsUserAddress(u32 addr) {
	return (addr & 0x80000000) == 0;
}

static u32 sceUmdGetDiscInfo(u32 infoAddr) {
	DEBUG_LOG(Log::sceIo, "sceUmdGetDiscInfo(%08x)", infoAddr);

	if (Memory::IsValidRange(infoAddr, 8) && IsUserAddress(infoAddr) && IsUserAddress(infoAddr + 4)) {
		auto info = PSPPointer<PspUmdInfo>::Create(infoAddr);
		// Diagnostic only, no behaviour change: this is the one value the shell uses to tell a
		// game disc from a UMD Video disc, so the return address shows who asks - it is what
		// tells us whether the XMB's video side really queries this. The call is polled, hence
		// only the first few are logged. Logged before the size check on purpose: if the caller
		// passes a size we reject, that rejection is exactly what we need to see, and it would
		// otherwise only show up as a generic HLE error.
		static int discInfoLogCount = 0;
		const bool logThisCall = discInfoLogCount < 12;
		if (logThisCall) {
			discInfoLogCount++;
		}

		if (info->size != 8) {
			if (logThisCall) {
				printf("[umd] sceUmdGetDiscInfo from ra=0x%08x -> size=%d, rejected (expected 8)\n",
					currentMIPS->r[MIPS_REG_RA], (int)info->size);
			}
			return hleLogError(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT);
		}

		// Games see a game disc, whatever else is on it: a mixed disc (video plus a bundled
		// firmware update) must still boot its game side.
		const u32 type = __UmdDiscType();
		info->type = (type & PSP_UMD_TYPE_GAME) || type == 0 ? PSP_UMD_TYPE_GAME : type;
		if (logThisCall) {
			printf("[umd] sceUmdGetDiscInfo from ra=0x%08x -> type=0x%02x (%s) size=%d\n",
				currentMIPS->r[MIPS_REG_RA], (unsigned)info->type, UmdDiscTypeName(type), (int)info->size);
		}
		if (UmdHleLog()) {
			printf("[umd] HLE sceUmdGetDiscInfo(type=0x%02x (%s)) -> 0 (0x00000000)\n",
				(unsigned)type, UmdDiscTypeName(type));
		}
		return hleLogDebug(Log::sceIo, 0);
	} else {
		return hleLogError(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT);
	}
}

static int sceUmdActivate(u32 mode, u32 namePtr) {
	if (mode < 1 || mode > 2)
		return hleLogWarning(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT);
	// The firmware compares the name before checking the pointer, so a bad one crashes there.
	const char *name = namePtr != 0 && Memory::IsValidAddress(namePtr) ? Memory::GetCharPointer(namePtr) : nullptr;
	if (!name || strncmp(name, "disc0:", 7) != 0 || !IsUserAddress(namePtr))
		return hleLogWarning(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, "bad name");

	__KernelUmdActivate();

	if (UmdHleLog()) {
		printf("[umd] HLE sceUmdActivate(mode=%u, '%s') -> 0 (0x00000000)\n", mode, name ? name : "");
	}
	if (mode != 1) {
		return hleLogError(Log::sceIo, 0, "UNTESTED");
	}
	return hleLogDebug(Log::sceIo, 0);
}

static int sceUmdDeactivate(u32 mode, u32 namePtr)
{
	// Why 18?  No idea.
	if (mode > 18)
		return hleLogError(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT);
	// Unlike sceUmdActivate(), the name isn't compared, and only mode 2 requires one.
	if ((mode == 2 && namePtr == 0) || !IsUserAddress(namePtr))
		return hleLogError(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, "bad name");
	// Only for the log below: upstream doesn't read the name here, our diagnostic does.
	const char *name = namePtr != 0 && Memory::IsValidAddress(namePtr) ? Memory::GetCharPointer(namePtr) : nullptr;

	__KernelUmdDeactivate();

	if (UmdHleLog()) {
		printf("[umd] HLE sceUmdDeactivate(mode=%u, '%s') -> 0 (0x00000000)\n", mode, name ? name : "");
	}
	if (mode == 1) {
		return hleLogDebug(Log::sceIo, 0);
	} else {
		return hleLogError(Log::sceIo, 0, "UNTESTED mode != 1");
	}
}

static u32 sceUmdRegisterUMDCallBack(u32 cbId)
{
	int retVal = 0;

	if (!kernelObjects.Is<PSPCallback>(cbId)) {
		retVal = SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT;
	} else {
		// There's only ever one.
		driveCBId = cbId;
	}
	// Whether an insert notification can reach the shell at all depends on this having happened.
	if (UmdHleLog()) {
		printf("[umd] HLE sceUmdRegisterUMDCallBack(cbId=%u) -> %d (0x%08x) [driveCBId=%d]\n",
			cbId, retVal, (unsigned)retVal, driveCBId);
	}
	return hleLogDebug(Log::sceIo, retVal);
}

static int sceUmdUnRegisterUMDCallBack(int cbId)
{
	if (cbId != driveCBId) {
		return hleLogError(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT);
	} else {
		int retVal;
		if (sceKernelGetCompiledSdkVersion() > 0x3000000) {
			retVal = 0;
		} else {
			retVal = cbId;
		}
		driveCBId = 0;
		return hleLogDebug(Log::sceIo, retVal);
	}
}

static u32 sceUmdGetDriveStat() {
	if (!UMDInserted) {
		if (UmdGetStatLog()) {
			printf("[umdio] getDriveStat -> 0x%02x (no disc)\n", (unsigned)PSP_UMD_NOT_PRESENT);
		}
		return hleLogWarning(Log::sceIo, PSP_UMD_NOT_PRESENT, "sceUmdGetDriveStat: UMD is taking out for switch UMD");
	}
	//u32 retVal = PSP_UMD_INITED | PSP_UMD_READY | PSP_UMD_PRESENT;
	u32 retVal = __KernelUmdGetState();
	if (UmdGetStatLog()) {
		printf("[umdio] getDriveStat -> 0x%02x (activated=%d)\n", (unsigned)retVal, umdActivated ? 1 : 0);
	}
	// This one can be very spammy.
	return hleLogVerbose(Log::sceIo, retVal, "0x%02x=sceUmdGetDriveStat()", retVal);
}

static void __UmdStatTimeout(u64 userdata, int cyclesLate)
{
	SceUID threadID = (SceUID)userdata;

	u32 error;
	SceUID waitID = __KernelGetWaitID(threadID, WAITTYPE_UMD, error);
	// Assuming it's still waiting.
	if (waitID == 1) {
		if (UmdStateLog()) {
			printf("[umdio] umd stat wait TIMED OUT (thread %d) - a caller was waiting for a state bit we never reported\n", (int)threadID);
		}
		__KernelResumeThreadFromWait(threadID, SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	}

	HLEKernel::RemoveWaitingThread(umdWaitingThreads, threadID);
}

// The firmware waits on an event flag holding the drive state (mediaman.prx), and passes no
// timeout at all for 0, so that waits forever.
static void __UmdWaitStat(u32 timeout, bool callbacks)
{
	if (timeout == 0)
		return;

	// Measured on hardware. Oddly, the CB version doesn't have the shortest step.
	if (timeout <= 1 && !callbacks)
		timeout = 25;
	else if (timeout <= 209)
		timeout = 240;

	CoreTiming::ScheduleEvent(usToCycles((int) timeout), umdStatTimeoutEvent, __KernelGetCurThread());
}

/** 
* Wait for a drive to reach a certain state
*
* @param stat - The drive stat to wait for.
* @return < 0 on error
*
*/
static int sceUmdWaitDriveStat(u32 stat) {
	if ((stat & UMD_STAT_ALLOW_WAIT) == 0) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, "bad status");
	}
	if (!__KernelIsDispatchEnabled()) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	}
	if (__IsInInterrupt()) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_ILLEGAL_CONTEXT, "inside interrupt");
	}

	hleEatCycles(520);
	const u32 curState = __KernelUmdGetState();
	const bool willWait = (stat & curState) == 0;
	if (UmdStateLog()) {
		printf("[umdio] waitDriveStat req=0x%02x cur=0x%02x -> %s\n", (unsigned)stat, (unsigned)curState,
			willWait ? "WAIT (no timeout - blocks until the state changes)" : "satisfied");
	}
	if (willWait) {
		DEBUG_LOG(Log::sceIo, "sceUmdWaitDriveStat(stat = %08x): waiting", stat);
		umdWaitingThreads.push_back(__KernelGetCurThread());
		__KernelWaitCurThread(WAITTYPE_UMD, 1, stat, 0, 0, "umd stat waited");
		return hleLogDebug(Log::sceIo, 0);
	}

	return hleLogDebug(Log::sceIo, 0);
}

static int sceUmdWaitDriveStatWithTimer(u32 stat, u32 timeout) {
	if ((stat & UMD_STAT_ALLOW_WAIT) == 0) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, "bad status");
	}
	if (!__KernelIsDispatchEnabled()) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	}
	if (__IsInInterrupt()) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_ILLEGAL_CONTEXT, "inside interrupt");
	}

	hleEatCycles(520);
	const u32 curState = __KernelUmdGetState();
	const bool willWait = (stat & curState) == 0;
	if (UmdStateLog()) {
		printf("[umdio] waitDriveStatWithTimer req=0x%02x cur=0x%02x timeout=%uus -> %s\n", (unsigned)stat,
			(unsigned)curState, (unsigned)timeout,
			willWait ? "WAIT (guest will burn this timeout)" : "satisfied");
	}
	if (willWait) {
		__UmdWaitStat(timeout, false);
		umdWaitingThreads.push_back(__KernelGetCurThread());
		__KernelWaitCurThread(WAITTYPE_UMD, 1, stat, 0, false, "umd stat waited with timer");
		return hleLogDebug(Log::sceIo, 0, "waiting");
	} else {
		hleReSchedule("umd stat checked");
	}

	return hleLogDebug(Log::sceIo, 0);
}

static int sceUmdWaitDriveStatCB(u32 stat, u32 timeout) {
	if ((stat & UMD_STAT_ALLOW_WAIT) == 0) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_ERRNO_INVALID_ARGUMENT, "bad status");
	}
	if (!__KernelIsDispatchEnabled()) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	}
	if (__IsInInterrupt()) {
		return hleLogDebug(Log::sceIo, SCE_KERNEL_ERROR_ILLEGAL_CONTEXT, "inside interrupt");
	}

	hleEatCycles(520);
	hleCheckCurrentCallbacks();
	if ((stat & __KernelUmdGetState()) == 0) {
		__UmdWaitStat(timeout, true);
		umdWaitingThreads.push_back(__KernelGetCurThread());
		__KernelWaitCurThread(WAITTYPE_UMD, 1, stat, 0, true, "umd stat waited");
		return hleLogDebug(Log::sceIo, 0, "waiting");
	} else {
		hleReSchedule("umd stat waited");
	}

	return hleLogDebug(Log::sceIo, 0);
}

static u32 sceUmdCancelWaitDriveStat() {
	for (size_t i = 0; i < umdWaitingThreads.size(); ++i) {
		const SceUID threadID = umdWaitingThreads[i];
		CoreTiming::UnscheduleEvent(umdStatTimeoutEvent, threadID);
		HLEKernel::ResumeFromWait(threadID, WAITTYPE_UMD, 1, (int)SCE_KERNEL_ERROR_WAIT_CANCEL);
	}
	umdWaitingThreads.clear();

	return hleLogDebug(Log::sceIo, 0);
}

static u32 sceUmdGetErrorStat() {
	return hleLogDebug(Log::sceIo, umdErrorStat);
}

// Diagnosis for a disc that has no disc0:/PSP_GAME/PARAM.SFO - a UMD Video disc, UMD Music, or a
// homebrew/hybrid image. Strictly read-only: nothing here touches emulator state. It exists
// because the questions that matter cannot be answered on the host - does the image really carry
// the UMD_VIDEO partition the shell's own player opens, can PPSSPP's ISO filesystem read those
// files at all, and does the image still identify as video rather than as a game.
static void LogNonGameDiscContents(const Path &filepath, FileLoader *fileLoader) {
	printf("[umd] non-game disc: image='%s'\n", filepath.c_str());

	// 1. Root directory: proves the ISO filesystem is mounted and shows which partitions exist.
	bool rootExists = false;
	const std::vector<PSPFileInfo> rootListing = pspFileSystem.GetDirListing("disc0:/", &rootExists);
	const size_t maxLogged = 20;
	printf("[umd] root: dir_exists=%d entries=%d\n", rootExists ? 1 : 0, (int)rootListing.size());
	for (size_t i = 0; i < rootListing.size() && i < maxLogged; ++i) {
		printf("[umd]   root[%d]='%s'%s\n", (int)i, rootListing[i].name.c_str(),
			rootListing[i].type == FILETYPE_DIRECTORY ? " (dir)" : "");
	}
	if (rootListing.size() > maxLogged) {
		printf("[umd]   ... %d more entries not logged\n", (int)(rootListing.size() - maxLogged));
	}

	// 2. The paths the shell's own video player opens (PLAYLIST.UMD lists the streams, the
	// streams themselves live under STREAM/, RESOURCE/ holds the menus/artwork).
	static const char *const videoPaths[] = {
		"disc0:/UMD_VIDEO/PARAM.SFO",
		"disc0:/UMD_VIDEO/PLAYLIST.UMD",
		"disc0:/UMD_VIDEO/STREAM",
		"disc0:/UMD_VIDEO/RESOURCE",
	};
	for (const char *path : videoPaths) {
		const PSPFileInfo info = pspFileSystem.GetFileInfo(path);
		printf("[umd]   %s exists=%d dir=%d size=%lld\n", path, info.exists ? 1 : 0,
			info.type == FILETYPE_DIRECTORY ? 1 : 0, (long long)info.size);
	}

	// 2b. UMD_DATA.BIN - the 48 bytes at the disc root that carry the disc's own metadata (region,
	// parental level, and the flags the shell reads before it decides how to present the disc).
	// Dumped because the guest reads it right before stalling, and a UMD Video disc's copy differs
	// from a game disc's, so this tells a well-formed video disc apart from an odd rip without
	// guessing.
	{
		std::vector<u8> umdData;
		const int umdDataRead = pspFileSystem.ReadEntireFile("disc0:/UMD_DATA.BIN", umdData);
		if (umdDataRead == 0 && !umdData.empty()) {
			const size_t dumpSize = umdData.size() < 32 ? umdData.size() : 32;
			printf("[umd] UMD_DATA.BIN: size=%d\n", (int)umdData.size());
			for (size_t offset = 0; offset < dumpSize; offset += 16) {
				printf("[umd]   %04x:", (unsigned)offset);
				for (size_t i = 0; i < 16 && offset + i < dumpSize; ++i) {
					printf(" %02x", umdData[offset + i]);
				}
				printf("  |");
				for (size_t i = 0; i < 16 && offset + i < dumpSize; ++i) {
					const u8 c = umdData[offset + i];
					printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
				}
				printf("|\n");
			}
		} else {
			printf("[umd] UMD_DATA.BIN: unreadable (read=%d)\n", umdDataRead);
		}
	}

	// 3. The disc's own PARAM.SFO (CATEGORY = UG on UMD Video). Parsed into a local ParamSFOData
	// on purpose: g_paramSFO is the *game* identity, and a video disc must not be allowed to
	// leak a fake disc ID into per-game settings or the shader cache.
	std::vector<u8> sfoData;
	// NB: ReadEntireFile returns 0 on success (an error code otherwise), not the byte count.
	const int sfoReadResult = pspFileSystem.ReadEntireFile("disc0:/UMD_VIDEO/PARAM.SFO", sfoData);
	ParamSFOData videoSfo;
	if (sfoReadResult == 0 && videoSfo.ReadSFO(sfoData)) {
		printf("[umd] video SFO: size=%d keys=%d TITLE='%s' CATEGORY='%s' DISC_ID='%s'\n",
			(int)sfoData.size(), (int)videoSfo.GetKeys().size(),
			videoSfo.GetValueString("TITLE").c_str(),
			videoSfo.GetValueString("CATEGORY").c_str(),
			videoSfo.GetValueString("DISC_ID").c_str());
		if (videoSfo.HasKey("TITLE_0")) {
			printf("[umd] video SFO: TITLE_0='%s'\n", videoSfo.GetValueString("TITLE_0").c_str());
		}
		// The full key list matters: real UMD Video discs carry extra entries (VMODE etc.) that
		// tell the shell which player path to take.
		const std::vector<std::string> keys = videoSfo.GetKeys();
		printf("[umd] video SFO: keys:");
		for (const std::string &key : keys) {
			printf(" %s", key.c_str());
		}
		printf("\n");
	} else {
		printf("[umd] video SFO: missing or unreadable (read=%d size=%d)\n", sfoReadResult, (int)sfoData.size());
	}

	// 4. Re-identify the image. It has to come out as UMD_VIDEO here, otherwise the mount above
	// was done with the wrong expectations about what is on the disc.
	if (fileLoader) {
		std::string identifyError;
		const IdentifiedFileType type = Identify_File(fileLoader, &identifyError);
		printf("[umd] identify: %s%s%s\n", IdentifiedFileTypeToString(type),
			identifyError.empty() ? "" : " - ", identifyError.c_str());
	}
}

void __UmdReplace(const Path &filepath) {
	std::string error = "";
	FileLoader *fileLoader;
	if (!UmdReplace(filepath, &fileLoader, error)) {
		ERROR_LOG(Log::sceIo, "UMD Replace failed: %s", error.c_str());
		return;
	}

	// Remember what's in the drive: a LoadExec out of the shell's UMD slot later has to tell the
	// host which disc image the game came from.
	umdImagePath = filepath;

	Achievements::ChangeUMD(filepath, fileLoader);
	g_umdPath = filepath;

	// Read the disc's PARAM.SFO so the rest of the emulator knows which game this is.
	// A title booted from the shell's UMD slot otherwise has no disc ID at all, which
	// leaves it without a shader cache (every shader compiles on the fly) and without
	// its per-game settings.
	//
	// Note: do NOT pull in the shader cache here. At this point disc0: may still report
	// the previous disc (observed: it reported another title's ID), so we'd load the wrong
	// file and block for seconds. The shader cache is loaded at LoadExec instead, where the
	// ID is verified to be this disc's.
	if (LoadParamSFOFromDisc()) {
		printf("[cache] UmdReplace: disc ID is '%s'\n", g_paramSFO.GetDiscID().c_str());
	} else {
		// A UMD Video (or UMD Music) disc has no disc0:/PSP_GAME/PARAM.SFO, so the load above
		// fails and g_paramSFO stays empty. That is exactly what we want: the video disc's own
		// SFO lives in disc0:/UMD_VIDEO/PARAM.SFO and has CATEGORY = UG - it is not a game
		// identity, so putting it into g_paramSFO would leak a fake disc ID into per-game
		// settings and the shader cache. Log it instead; this is the host-side proof that the
		// image really is mounted and carries the video partition the shell's player will read.
		// Everything else the insert path needs (umdImagePath, UMDInserted, the drive callback)
		// happens below regardless of this failing.
		const PSPFileInfo videoSfo = pspFileSystem.GetFileInfo("disc0:/UMD_VIDEO/PARAM.SFO");
		printf("[umd] video disc: UMD_VIDEO/PARAM.SFO=%d image='%s'\n", videoSfo.exists ? 1 : 0, filepath.c_str());
		// Read-only forensics on the disc contents, so one hardware log tells us where the
		// shell gets stuck: mounting, recognition (XMB listing the disc), or playback.
		LogNonGameDiscContents(filepath, fileLoader);
	}

	// What is in the drive now, read off the disc itself rather than inferred from how it got
	// here: UMD_DATA.BIN's fourth field lists the content (G/V/A - a video demo disc says GVA),
	// with the partitions as the fallback for images that have no UMD_DATA.BIN. A video disc
	// that also bundles a firmware update carries a PSP_GAME, so a directory-only guess calls
	// it a game and the VSH then refuses it (#22737); DetectUmdDiscType() drops the game bit
	// when that PSP_GAME has no EBOOT.BIN.
	const u32 detected = DetectUmdDiscType(&pspFileSystem, "disc0:");
	// A loose game directory or an image without the usual layout still boots as a game.
	__UmdSetDiscType(detected != 0 ? detected : PSP_UMD_TYPE_GAME);

	UMDInserted = false;
	// Wake any threads waiting for the disc to be removed.
	UmdWakeThreads();

	// A pending insert notification from __UmdInsert() (which schedules with userdata 1 and can
	// still be queued while the callback is pending) must not fire on top of this swap - the shell
	// would get a second, stale change notification for a disc that has already been replaced.
	// Same cleanup __UmdEject() does, for the same reason.
	CoreTiming::UnscheduleEvent(umdInsertChangeEvent, 1);
	CoreTiming::ScheduleEvent(usToCycles(200*1000), umdInsertChangeEvent, 0); // Wait sceUmdCheckMedium call
	// The disc is physically in the drive already, so make it visible right away. The
	// scheduled event above exists to emulate the PSP's medium check, but while the
	// emulator is paused (XMB pause menu) it never runs and the shell would keep
	// reporting an empty drive until the user resumes.
	UMDInserted = true;
	UmdWakeThreads();
	// First half of the insert notification, matching hardware: the medium changed, but the drive
	// has not spun up yet, so this one does NOT carry READABLE. The second half (with
	// PRESENT|READY|READABLE) follows from __UmdInsertReady once MICRO_DELAY_ACTIVATE has passed -
	// a shell that waits for a ready callback has nothing to react to otherwise.
	const u32 notifyArg = PSP_UMD_PRESENT | PSP_UMD_CHANGED;
	// If nothing is registered here the shell never hears that the disc arrived, and whatever it
	// is waiting for never happens - worth stating explicitly rather than inferring it from the
	// absence of a callback line in the log.
	printf("[umd] notify(insert) arg=0x%02x driveCBId=%d (0 = no callback registered, notification DROPPED)\n",
		notifyArg, driveCBId);
	if (driveCBId != 0)
		__KernelNotifyCallback(driveCBId, notifyArg);

	// Spin-up. Uses its own event id: umdStatChangeEvent belongs to sceUmdActivate/Deactivate and
	// umdInsertChangeEvent still has the 200ms pending-insert above, so cancelling either of those
	// here would break them.
	CoreTiming::UnscheduleEvent(umdInsertReadyEvent, 0);
	CoreTiming::ScheduleEvent(usToCycles(MICRO_DELAY_ACTIVATE), umdInsertReadyEvent, 0);
	printf("[umd] insert ready scheduled in %lluus\n", (unsigned long long)MICRO_DELAY_ACTIVATE);

	// List the next 200 HLE calls: whatever the shell does with this disc happens in that window,
	// and a guest spinning in short calls is invisible to the slow-call tracer.
	HleTraceNextCalls(200);
}

bool __UmdInsert(const Path &filepath, std::string *error) {
	if (__UmdIsInserted()) {
		*error = "a disc is already in the drive";
		return false;
	}
	FileLoader *fileLoader;
	if (!UmdMount(filepath, &fileLoader, *error)) {
		return false;
	}
	Achievements::ChangeUMD(filepath, fileLoader);
	g_umdPath = filepath;
	// Same host-side bookkeeping as __UmdReplace(): a title launched from the shell's UMD slot
	// later needs the image path (see GetInsertedUmdPath/AdoptDiscTitleForHost). Upstream's insert
	// path only kept g_umdPath, which the disc-title adoption doesn't read.
	umdImagePath = filepath;
	// And what is on the disc, read off the disc itself the same way __UmdReplace() does - without
	// this a UMD Video put into an empty drive would still describe itself as a game disc.
	const u32 detected = DetectUmdDiscType(&pspFileSystem, "disc0:");
	__UmdSetDiscType(detected != 0 ? detected : PSP_UMD_TYPE_GAME);
	// List the next 200 HLE calls: whatever the shell does with this disc happens in that window.
	HleTraceNextCalls(200);

	// The upstream half of the two-stage insert: this path schedules with userdata 1 (and polls
	// DriveCallbackPending() below), while __UmdReplace() uses userdata 0. The two share one event
	// id, so anything that cancels a pending insert has to cancel both userdatas - see __UmdEject()
	// and __UmdReplace(). Our own second notification is a separate event, umdInsertReadyEvent.
	CoreTiming::UnscheduleEvent(umdInsertChangeEvent, 1);
	if (DriveCallbackPending()) {
		CoreTiming::ScheduleEvent(usToCycles(100 * 1000), umdInsertChangeEvent, 1);
		return true;
	}

	UMDInserted = true;
	UmdWakeThreads();
	if (driveCBId != 0)
		__KernelNotifyCallback(driveCBId, PSP_UMD_PRESENT | PSP_UMD_READABLE | PSP_UMD_CHANGED);
	// The drive has only just taken the disc, so it isn't spun up yet: the second notification
	// (with the drive's own state once it is) follows from __UmdInsertReady, exactly as it does
	// for a swap. A shell that waits for the ready callback has nothing to react to otherwise.
	CoreTiming::UnscheduleEvent(umdInsertReadyEvent, 0);
	CoreTiming::ScheduleEvent(usToCycles(MICRO_DELAY_ACTIVATE), umdInsertReadyEvent, 0);
	return true;
}

Path __UmdGetPath() {
	if (!UMDInserted) {
		return Path();
	}
	if (!g_umdPath.empty()) {
		return g_umdPath;
	}
	const CoreParameter &coreParam = PSP_CoreParameter();
	if (!coreParam.mountIso.empty()) {
		return coreParam.mountIso;
	}
	switch (coreParam.fileType) {
	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
	case IdentifiedFileType::PSP_UMD_VIDEO_ISO:
	case IdentifiedFileType::PSP_DISC_DIRECTORY:
		return coreParam.fileToStart;
	default:
		// A homebrew or PS1 EBOOT, with no disc behind disc0:.
		return Path();
	}
}

bool getUMDReplacePermit() {
	return g_UMDReplacePermit;
}

void __UmdEject() {
	// A disc still waiting to go in comes back out without the shell ever hearing of it.
	// __UmdReplace schedules the pending insert with userdata 0, and __UmdInsert (the empty-drive
	// path) with 1, so match both here - otherwise the queued insert fires anyway and the disc
	// reappears after ejecting.
	const bool insertPending = CoreTiming::UnscheduleEvent(umdInsertChangeEvent, 0) != 0;
	CoreTiming::UnscheduleEvent(umdInsertChangeEvent, 1);
	// A disc that comes out during spin-up must not report itself ready afterwards.
	CoreTiming::UnscheduleEvent(umdInsertReadyEvent, 0);
	if (!UMDInserted && !insertPending) {
		return;
	}
	pspFileSystem.Unmount("umd0:");
	pspFileSystem.Unmount("umd1:");
	pspFileSystem.Unmount("umd:");
	pspFileSystem.Unmount("disc0:");
	umdImagePath.clear();
	// The host-facing path the insert paths filled in, so __UmdGetPath() has nothing left to report.
	g_umdPath.clear();
	// No disc, so nothing to describe - back to the default the shell sees on a bare console.
	__UmdSetDiscType(PSP_UMD_TYPE_GAME);
	if (!UMDInserted) {
		return;
	}

	UMDInserted = false;
	UmdWakeThreads();
	if (driveCBId != 0)
		__KernelNotifyCallback(driveCBId, PSP_UMD_NOT_PRESENT | PSP_UMD_CHANGED);
}

bool __UmdIsInserted() {
	// Counting a disc that's on its way in, which can't finish while emulation is paused.
	//
	// The "on its way in" half is load-bearing, not defensive: __UmdInsert() returns before the
	// disc is announced (UMDInserted is still false) when a drive callback is pending, and it
	// reschedules with userdata 1 until it can fire. Between those two points the drive is empty
	// to UMDInserted but does have a mounted disc - and both the pause menu and
	// UmdReplaceScreen::UseDisc() ask this to decide insert vs. replace. Dropping the pending
	// check makes a disc inserted into an empty drive look like an empty drive, so the next
	// insert goes down UmdReplace()'s path and fails its "has no disc" guard.
	//
	// It costs a false "inserted" only in the one case where it matters: __UmdEject() cancels
	// both userdatas, so a pending insert never survives an eject.
	return UMDInserted || CoreTiming::IsScheduled(umdInsertChangeEvent);
}

// The PSP boots the XMB with an empty drive, but UMDInserted defaults to true because
// games are normally started from a disc image. Clear it when the VSH is loaded so the
// pause menu offers "Insert UMD" first, like a real console with no disc.
void __UmdNoDiscAtBoot() {
	// A disc the user put in the drive stays there when the shell is re-entered: coming back
	// from a game is another vshmain LoadExec, but disc0: is still mounted, so clearing the
	// slot here would make the pause menu offer "Insert UMD" for a disc that is in the drive -
	// and re-launching the title from its icon would then fall back to a disc0:/ boot path
	// because the image path is gone. Only the first shell boot, with an empty drive, clears.
	if (!umdImagePath.empty()) {
		return;
	}
	// A disc the loader put in the drive counts as well: booting a UMD Video image mounts it as
	// mountIso and then swaps in vshmain.prx (Core/System.cpp's PSP_InitStart), which reaches the
	// shell without any insert going through here, so umdImagePath is still empty. Saying "no disc"
	// anyway makes the XMB open with an empty drive: it ejects, and the video menu is the one place
	// that has to show the disc. The mount is the evidence, not our bookkeeping.
	if (pspFileSystem.GetSystem("disc0:")) {
		return;
	}
	UMDInserted = false;
	// The drive is empty, so there is no image for the host to describe either. (Not reset in
	// __UmdInit: a LoadExec into a game must still see the disc it is about to boot from.)
	umdImagePath.clear();
	// Same reasoning as the image path above: an empty drive has no type to report. A disc that
	// is still in the drive returns above and keeps the type it was given when it went in.
	__UmdSetDiscType(PSP_UMD_TYPE_GAME);
	if (umdInsertChangeEvent != -1) {
		CoreTiming::UnscheduleEvent(umdInsertChangeEvent, 0);
	}
}

// Called from CPU_Shutdown. The mounts are dropped by pspFileSystem.Shutdown(), so all that's
// left is to forget that this session ever had a disc: umdImagePath is what tells
// __UmdNoDiscAtBoot() whether the drive is really occupied, and a stale path from a previous
// game would make the next shell boot believe a disc that isn't mounted is still in the drive.
// UMDInserted goes back to its module-level default so a direct game boot still sees a disc.
void __UmdClearHostState() {
	umdImagePath.clear();
	UMDInserted = true;
	// The next shell boot starts from scratch, so it must not inherit the previous session's disc
	// type either - otherwise a video disc from the last session would describe a possibly empty
	// drive, or a game disc inserted next.
	__UmdSetDiscType(PSP_UMD_TYPE_GAME);
}

static u32 sceUmdReplaceProhibit() {
	if (UmdHleLog()) {
		printf("[umd] HLE sceUmdReplaceProhibit() -> 0 (0x00000000) [was permitted=%d]\n", g_UMDReplacePermit ? 1 : 0);
	}
	if (g_UMDReplacePermit) {
		g_UMDReplacePermit = false;
		System_Notify(SystemNotification::SWITCH_UMD_UPDATED);
		return hleLogDebug(Log::sceIo, 0, "switched state to prohibited");
	} else {
		return hleLogDebug(Log::sceIo, 0);
	}
}

static u32 sceUmdReplacePermit() {
	if (UmdHleLog()) {
		printf("[umd] HLE sceUmdReplacePermit() -> 0 (0x00000000) [was permitted=%d]\n", g_UMDReplacePermit ? 1 : 0);
	}
	if (!g_UMDReplacePermit) {
		//INFO_LOG(Log::sceIo, "sceUmdReplacePermit() - permitted");
		g_UMDReplacePermit = true;
		System_Notify(SystemNotification::SWITCH_UMD_UPDATED);
		return hleLogDebug(Log::sceIo, 0, "switched state to permitted");
	} else {
		return hleLogDebug(Log::sceIo, 0);
	}
}

const HLEFunction sceUmdUser[] = 
{
	{0XC6183D47, &WrapI_UU<sceUmdActivate>,               "sceUmdActivate",               'i', "is"},
	{0X6B4A146C, &WrapU_V<sceUmdGetDriveStat>,            "sceUmdGetDriveStat",           'x', ""  },
	{0X46EBB729, &WrapI_V<sceUmdCheckMedium>,             "sceUmdCheckMedium",            'i', ""  },
	{0XE83742BA, &WrapI_UU<sceUmdDeactivate>,             "sceUmdDeactivate",             'i', "xs"},
	{0X8EF08FCE, &WrapI_U<sceUmdWaitDriveStat>,           "sceUmdWaitDriveStat",          'i', "x" },
	{0X56202973, &WrapI_UU<sceUmdWaitDriveStatWithTimer>, "sceUmdWaitDriveStatWithTimer", 'i', "xx"},
	{0X4A9E5E29, &WrapI_UU<sceUmdWaitDriveStatCB>,        "sceUmdWaitDriveStatCB",        'i', "xx"},
	{0X6AF9B50A, &WrapU_V<sceUmdCancelWaitDriveStat>,     "sceUmdCancelWaitDriveStat",    'x', ""  },
	{0X20628E6F, &WrapU_V<sceUmdGetErrorStat>,            "sceUmdGetErrorStat",           'x', ""  },
	{0X340B7686, &WrapU_U<sceUmdGetDiscInfo>,             "sceUmdGetDiscInfo",            'x', "x" },
	{0XAEE7404D, &WrapU_U<sceUmdRegisterUMDCallBack>,     "sceUmdRegisterUMDCallBack",    'x', "x" },
	{0XBD2BDE07, &WrapI_I<sceUmdUnRegisterUMDCallBack>,   "sceUmdUnRegisterUMDCallBack",  'i', "i" },
	{0X87533940, &WrapU_V<sceUmdReplaceProhibit>,         "sceUmdReplaceProhibit",        'x', ""  },
	{0XCBE9F02A, &WrapU_V<sceUmdReplacePermit>,           "sceUmdReplacePermit",          'x', ""  },
	{0X14C6C45C, nullptr,                                 "sceUmdUnuseUMDInMsUsbWlan",    '?', ""  },
	{0XB103FA38, nullptr,                                 "sceUmdUseUMDInMsUsbWlan",      '?', ""  },
	{0X1A2485D2, nullptr,                                 "sceUmdUseUMDDetectIntr",       '?', ""  },
	{0X3BA4EC53, nullptr,                                 "sceUmdUnuseUMDDetectIntr",     '?', ""  },
};

void Register_sceUmdUser()
{
	RegisterHLEModule("sceUmdUser", ARRAY_SIZE(sceUmdUser), sceUmdUser);
}
