// Copyright (C) 2023 M4xw

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include "ppsspp_config.h"
#if PPSSPP_PLATFORM(SWITCH)

#include <stdio.h>
#include <malloc.h> // memalign
#include <switch.h>

#include "Common/MemArena.h"

static uintptr_t memoryBase = 0;
static uintptr_t memoryCodeBase = 0;
static uintptr_t memorySrcBase = 0;
static VirtmemReservation *memoryReservation = nullptr;
static VirtmemReservation *memoryCodeReservation = nullptr;
static bool memoryCodeMapped = false;

static constexpr size_t PSP_ADDRESS_SPACE_SIZE = 0x10000000;
static constexpr size_t PSP_BACKING_STORE_SIZE = PSP_ADDRESS_SPACE_SIZE;

static void *ReserveVirtmem(size_t size, bool code, VirtmemReservation **reservation) {
	virtmemLock();
	void *address = code ? virtmemFindCodeMemory(size, 0x1000) : virtmemFindAslr(size, 0x1000);
	*reservation = address ? virtmemAddReservation(address, size) : nullptr;
	virtmemUnlock();
	return *reservation ? address : nullptr;
}

size_t MemArena::roundup(size_t x) {
	return x;
}

bool MemArena::NeedsProbing() {
	return false;
}

bool MemArena::GrabMemSpace(size_t size) {
	return true;
}

void MemArena::ReleaseSpace() {
	if (memoryCodeMapped) {
		const Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, PSP_BACKING_STORE_SIZE);
		// Logged on success too: whether this call works is exactly what decides if the process is
		// left with a stale code region (which is what makes the loader abort with
		// KernelError_InvalidMemoryState once the app exits).
		printf("[mem] unmap code memory code=%p src=%p size=0x%zx rc=0x%x\n",
			(void *)memoryCodeBase, (void *)memorySrcBase, PSP_BACKING_STORE_SIZE, rc);
		if (R_FAILED(rc)) {
			printf("Failed to release view space...\n");
		}
	}
	memoryCodeMapped = false;

	free((void *)memorySrcBase);
	memorySrcBase = 0;
	virtmemLock();
	if (memoryReservation) {
		virtmemRemoveReservation(memoryReservation);
	}
	if (memoryCodeReservation) {
		virtmemRemoveReservation(memoryCodeReservation);
	}
	virtmemUnlock();
	memoryReservation = nullptr;
	memoryCodeReservation = nullptr;
	memoryBase = 0;
	memoryCodeBase = 0;
}

void *MemArena::CreateView(s64 offset, size_t size, void *base) {
	if (offset < 0 || (size_t)offset > PSP_BACKING_STORE_SIZE || size > PSP_BACKING_STORE_SIZE - (size_t)offset) {
		return nullptr;
	}
	Result rc = svcMapProcessMemory(base, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size);
	if (R_FAILED(rc)) {
		printf("Fatal error creating the view... base: %p offset: %p size: %p src: %p err: %d "
			   "baseRange=%p..%p codeRange=%p..%p backRange=%p..%p\n",
			   (void *)base, (void *)offset, (void *)size, (void *)(memoryCodeBase + offset), rc,
			   (void *)memoryBase, (void *)(memoryBase + PSP_ADDRESS_SPACE_SIZE),
			   (void *)memoryCodeBase, (void *)(memoryCodeBase + PSP_BACKING_STORE_SIZE),
			   (void *)memorySrcBase, (void *)(memorySrcBase + PSP_BACKING_STORE_SIZE));
		return nullptr;
	}

	return base;
}

void MemArena::ReleaseView(s64 offset, void *view, size_t size) {
	if (R_FAILED(svcUnmapProcessMemory(view, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size))) {
		printf("Failed to unmap view...\n");
	}
}

u8 *MemArena::Find4GBBase() {
	memorySrcBase = (uintptr_t)memalign(0x1000, PSP_BACKING_STORE_SIZE);

	if (!memoryBase) {
		memoryBase = (uintptr_t)ReserveVirtmem(PSP_ADDRESS_SPACE_SIZE, false, &memoryReservation);
	}

	if (!memoryCodeBase) {
		memoryCodeBase = (uintptr_t)ReserveVirtmem(PSP_BACKING_STORE_SIZE, true, &memoryCodeReservation);
	}

	if (!memorySrcBase || !memoryBase || !memoryCodeBase) {
		printf("[mem] reserve failed src=%p base=%p code=%p\n", (void *)memorySrcBase, (void *)memoryBase, (void *)memoryCodeBase);
		ReleaseSpace();
		return nullptr;
	}

	// Every value here matters when diagnosing a failing view: offset+size hardcoded in CreateView()
	// are relative to memoryCodeBase, and the three addresses below are what the kernel checks.
	printf("[mem] code range=%p..%p src=%p..%p base=%p size=0x%zx\n",
		(void *)memoryCodeBase, (void *)(memoryCodeBase + PSP_BACKING_STORE_SIZE),
		(void *)memorySrcBase, (void *)(memorySrcBase + PSP_BACKING_STORE_SIZE),
		(void *)memoryBase, PSP_BACKING_STORE_SIZE);

	const Result mapRc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, PSP_BACKING_STORE_SIZE);
	printf("[mem] svcMapProcessCodeMemory rc=0x%x (code=%p src=%p size=0x%zx)\n",
		mapRc, (void *)memoryCodeBase, (void *)memorySrcBase, PSP_BACKING_STORE_SIZE);
	if (R_FAILED(mapRc)) {
		printf("Failed to map memory...\n");
		ReleaseSpace();
		return nullptr;
	}
	memoryCodeMapped = true;
	const Result permRc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), memoryCodeBase, PSP_BACKING_STORE_SIZE, Perm_Rx);
	printf("[mem] svcSetProcessMemoryPermission(Perm_Rx) rc=0x%x\n", permRc);
	if (R_FAILED(permRc)) {
		printf("Failed to set perms...\n");
		ReleaseSpace();
		return nullptr;
	}

	return (u8 *)memoryBase;
}

#endif // PPSSPP_PLATFORM(SWITCH)
