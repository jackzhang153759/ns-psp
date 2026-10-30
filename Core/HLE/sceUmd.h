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

#pragma once

#include "Common/CommonTypes.h"
#include "Common/File/Path.h"
class PointerWrap;

enum pspUmdState { 
	PSP_UMD_INIT        = 0x00,
	PSP_UMD_NOT_PRESENT = 0x01,
	PSP_UMD_PRESENT     = 0x02,
	PSP_UMD_CHANGED     = 0x04,
	PSP_UMD_NOT_READY   = 0x08,
	PSP_UMD_READY       = 0x10, 
	PSP_UMD_READABLE    = 0x20,
};

enum pspUmdType {
	PSP_UMD_TYPE_GAME  = 0x10,
	PSP_UMD_TYPE_VIDEO = 0x20,
	PSP_UMD_TYPE_AUDIO = 0x40,
};

void __UmdInit();
void __UmdDoState(PointerWrap &p);

void __UmdReplace(const Path &filepath);
void __UmdEject();
bool __UmdIsInserted();
void __UmdNoDiscAtBoot();
// A drive that can be empty: the XMB's, booted without a game. Insert fails if a disc is in.
bool __UmdInsert(const Path &filepath, std::string *error);
// The host path of the disc image in the drive, or empty if there's none.
Path __UmdGetPath();
bool getUMDReplacePermit();

// True when cbId is the drive callback the shell registered with sceUmdRegisterUMDCallBack().
// Used only by the diagnostic in __KernelNotifyCallback(), so that an insert notification is
// visible in the log without printing every unrelated callback in the system.
bool __UmdIsDriveCallback(int cbId);

// What the drive holds, as PSP_UMD_TYPE_* bits (game, video, audio - possibly combined), or 0 for
// an empty drive. Exposed so that other diagnostics - notably sceIoDevctl() 0x01F20001, the block
// device version of sceUmdGetDiscInfo()'s type field - report the same thing.
u32 __UmdDiscType();

// The host-side path of the image currently sitting in the emulated drive. Empty when the drive
// is empty. A title booted from the shell's UMD slot needs this so the host can point
// PSP_CoreParameter().fileToStart at the disc image instead of at the shell's vshmain.prx (that
// field is what GameInfoCache and the pause menu use to describe "the program being run").
const Path &GetInsertedUmdPath();

// Forgets the drive contents for the host side. Called when the whole emulated machine goes away
// (CPU_Shutdown), since the mounts are dropped there without the individual eject path running.
void __UmdClearHostState();

void Register_sceUmdUser();
