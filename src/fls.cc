// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Fiber-local storage. kernel32, Windows Server 2003 and Vista.
//
// Built on thread-local storage, which XP has. The two differ in exactly two
// ways that matter:
//
//  - FLS is per *fiber*; TLS is per thread. This implementation is per thread.
//    A program that converts threads to fibers and expects each fiber to see
//    its own FLS values will not get that here. Nothing in Chromium or the CRT
//    does, and faking it would mean intercepting the whole fiber API.
//  - FlsAlloc takes a callback invoked for every non-null value when a thread
//    exits and when the index is freed. TLS has no such thing, so this file
//    provides it, and it is the reason this is more than four forwarding
//    calls. The MSVC CRT relies on it to release per-thread state, so dropping
//    it would leak on every thread exit -- which in a Chromium process is a
//    great many.
//
// The callback is driven by a PE TLS callback in .CRT$XLB. The loader walks
// that array on DLL_THREAD_DETACH, in an executable as well as a DLL, and it
// works on XP. Registering it costs a TLS directory in the image and an empty
// call per thread exit when no index is in use.
//
// FLS_MAXIMUM_AVAILABLE is 128, which is what Vista allowed and what this
// matches. Later Windows raised it to 4080; a program that needs more than 128
// is not a program that was going to run on XP.

#include "winwrap.h"

namespace {

// Per Windows, and per the FLS_MAXIMUM_AVAILABLE the SDK defines.
constexpr DWORD kSlotCount = 128;

constexpr DWORD kSpinCount = 2000;

struct Slot {
  bool in_use;
  PFLS_CALLBACK_FUNCTION callback;
};

// One block per thread, linked into a process-wide list so FlsFree can reach
// every thread's value for the index being freed -- which is the whole reason
// the list exists. Values are only ever read and written by the owning thread,
// except during FlsFree.
struct ThreadBlock {
  ThreadBlock* next;
  ThreadBlock* prev;
  PVOID values[kSlotCount];
};

Slot g_slots[kSlotCount];
ThreadBlock g_blocks = {&g_blocks, &g_blocks, {}};  // Sentinel head.
DWORD g_tls_index = TLS_OUT_OF_INDEXES;

// Guards g_slots, the g_blocks list, and g_tls_index. Not the values: those
// belong to one thread each.
PVOID g_lock_ptr = nullptr;

CRITICAL_SECTION* EnsureLock() {
  // Same lazy install srwlock.cc uses, for the same reason: there is no
  // initialisation hook to put a DeleteCriticalSection against, so this is
  // allocated once and kept for process lifetime.
  if (PVOID existing =
          ::InterlockedCompareExchangePointer(&g_lock_ptr, nullptr, nullptr)) {
    return static_cast<CRITICAL_SECTION*>(existing);
  }

  auto* fresh = static_cast<CRITICAL_SECTION*>(
      ::HeapAlloc(::GetProcessHeap(), 0, sizeof(CRITICAL_SECTION)));
  if (!fresh || !::InitializeCriticalSectionAndSpinCount(fresh, kSpinCount)) {
    if (fresh) {
      ::HeapFree(::GetProcessHeap(), 0, fresh);
    }
    ::RaiseException(STATUS_NO_MEMORY, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return nullptr;
  }

  PVOID won = ::InterlockedCompareExchangePointer(&g_lock_ptr, fresh, nullptr);
  if (won == nullptr) {
    return fresh;
  }
  ::DeleteCriticalSection(fresh);
  ::HeapFree(::GetProcessHeap(), 0, fresh);
  return static_cast<CRITICAL_SECTION*>(won);
}

// Caller holds the lock.
bool EnsureTlsIndexLocked() {
  if (g_tls_index == TLS_OUT_OF_INDEXES) {
    g_tls_index = ::TlsAlloc();
  }
  return g_tls_index != TLS_OUT_OF_INDEXES;
}

ThreadBlock* CurrentBlock() {
  if (g_tls_index == TLS_OUT_OF_INDEXES) {
    return nullptr;
  }
  return static_cast<ThreadBlock*>(::TlsGetValue(g_tls_index));
}

// Allocates this thread's block and links it into the list. Returns null only
// on allocation failure, which the caller reports as the API demands.
ThreadBlock* EnsureCurrentBlock() {
  if (ThreadBlock* existing = CurrentBlock()) {
    return existing;
  }

  auto* block = static_cast<ThreadBlock*>(::HeapAlloc(
      ::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadBlock)));
  if (!block) {
    return nullptr;
  }

  CRITICAL_SECTION* lock = EnsureLock();
  ::EnterCriticalSection(lock);
  if (!EnsureTlsIndexLocked()) {
    ::LeaveCriticalSection(lock);
    ::HeapFree(::GetProcessHeap(), 0, block);
    return nullptr;
  }
  block->next = g_blocks.next;
  block->prev = &g_blocks;
  g_blocks.next->prev = block;
  g_blocks.next = block;
  ::LeaveCriticalSection(lock);

  ::TlsSetValue(g_tls_index, block);
  return block;
}

void NTAPI OnThreadEvent(PVOID /*module*/, DWORD reason, PVOID /*reserved*/) {
  if (reason != DLL_THREAD_DETACH) {
    return;
  }

  ThreadBlock* block = CurrentBlock();
  if (!block) {
    return;
  }

  // Unlink first, and take a copy of the callbacks while holding the lock, so
  // the callbacks themselves run with nothing held. Once unlinked, no other
  // thread can see this block, so its values need no further protection.
  PFLS_CALLBACK_FUNCTION callbacks[kSlotCount];
  CRITICAL_SECTION* lock = EnsureLock();
  ::EnterCriticalSection(lock);
  block->prev->next = block->next;
  block->next->prev = block->prev;
  for (DWORD i = 0; i < kSlotCount; ++i) {
    callbacks[i] = g_slots[i].in_use ? g_slots[i].callback : nullptr;
  }
  ::LeaveCriticalSection(lock);

  ::TlsSetValue(g_tls_index, nullptr);

  for (DWORD i = 0; i < kSlotCount; ++i) {
    if (callbacks[i] && block->values[i]) {
      callbacks[i](block->values[i]);
    }
  }
  ::HeapFree(::GetProcessHeap(), 0, block);
}

}  // namespace

// The loader calls everything in .CRT$XLB on thread attach and detach. The
// /INCLUDE directives are both required: __tls_used is what makes the linker
// emit a TLS directory at all, and without a reference to the callback itself
// it is dropped as unused, since nothing in the program names it.
#pragma section(".CRT$XLB", long, read)

extern "C" __declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK
    winwrap_fls_tls_callback = OnThreadEvent;

#if defined(_M_IX86)
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_winwrap_fls_tls_callback")
#else
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:winwrap_fls_tls_callback")
#endif

extern "C" {

DWORD WINAPI winwrap_FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback) {
  CRITICAL_SECTION* lock = EnsureLock();
  ::EnterCriticalSection(lock);

  if (!EnsureTlsIndexLocked()) {
    ::LeaveCriticalSection(lock);
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FLS_OUT_OF_INDEXES;
  }

  for (DWORD i = 0; i < kSlotCount; ++i) {
    if (g_slots[i].in_use) {
      continue;
    }
    g_slots[i].in_use = true;
    g_slots[i].callback = lpCallback;

    // A slot can be reused after FlsFree, and a thread that was alive across
    // the free may still hold a stale value in it. Windows hands out a slot
    // whose value reads as NULL on every fiber, so clear it everywhere.
    for (ThreadBlock* b = g_blocks.next; b != &g_blocks; b = b->next) {
      b->values[i] = nullptr;
    }
    ::LeaveCriticalSection(lock);
    return i;
  }

  ::LeaveCriticalSection(lock);
  ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  return FLS_OUT_OF_INDEXES;
}

PVOID WINAPI winwrap_FlsGetValue(DWORD dwFlsIndex) {
  if (dwFlsIndex >= kSlotCount) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }

  // No lock: a thread only ever reads its own values. Reading an index another
  // thread is concurrently freeing is a use-after-free in the caller, which is
  // equally true of the real FLS.
  ThreadBlock* block = CurrentBlock();
  PVOID value = block ? block->values[dwFlsIndex] : nullptr;
  ::SetLastError(ERROR_SUCCESS);
  return value;
}

BOOL WINAPI winwrap_FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData) {
  if (dwFlsIndex >= kSlotCount) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  ThreadBlock* block = EnsureCurrentBlock();
  if (!block) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }

  block->values[dwFlsIndex] = lpFlsData;
  return TRUE;
}

BOOL WINAPI winwrap_FlsFree(DWORD dwFlsIndex) {
  if (dwFlsIndex >= kSlotCount) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  CRITICAL_SECTION* lock = EnsureLock();
  ::EnterCriticalSection(lock);

  if (!g_slots[dwFlsIndex].in_use) {
    ::LeaveCriticalSection(lock);
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  PFLS_CALLBACK_FUNCTION callback = g_slots[dwFlsIndex].callback;
  g_slots[dwFlsIndex].in_use = false;
  g_slots[dwFlsIndex].callback = nullptr;

  // Windows runs the callback for every fiber that has a value in this index,
  // so run it for every thread. This happens with the lock held, which means a
  // callback must not call back into this API -- it would deadlock, as
  // CRITICAL_SECTION recursion would not save a second thread's block from
  // being walked while it is changing. The CRT's callback only frees memory.
  // Doing better would mean snapshotting an unbounded number of values first,
  // and the realistic caller frees an index at shutdown, from one thread.
  for (ThreadBlock* b = g_blocks.next; b != &g_blocks; b = b->next) {
    PVOID value = b->values[dwFlsIndex];
    b->values[dwFlsIndex] = nullptr;
    if (callback && value) {
      callback(value);
    }
  }

  ::LeaveCriticalSection(lock);
  return TRUE;
}

}  // extern "C"
