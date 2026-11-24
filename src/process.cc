// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Process-wide state: the error mode, and DPI awareness.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // PROCESSINFOCLASS as winternl.h declares it names only a few members, and
  // this is not among them.
  constexpr ULONG kProcessDefaultHardErrorMode = 12;

  using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

} // namespace

extern "C" {

// The process error mode. kernel32, Vista.
//
// XP can set this and never offered a way to read it back, which is the whole
// of what Vista added. The value is in the process information the kernel
// already keeps, so this asks for it directly.
//
// The one subtlety is that NT and Win32 disagree about the sense of the low
// bit. NT's ProcessDefaultHardErrorMode sets bit 0 when hard errors *are*
// displayed; Win32's SEM_FAILCRITICALERRORS sets the same bit to say they are
// *not*. The two are inverses, so the bit is flipped on the way out. Getting
// that backwards would be invisible in testing and exactly wrong in
// production, which is the kind of detail worth taking from someone who has
// already been through it -- YY-Thunks does the same inversion.
UINT WINAPI ww_GetErrorMode(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetErrorMode)) {
    return system_fn();
  }
  const auto query = WW_TRYFUNC_AS(NtQueryInformationProcessFn, ::winwrap::Module::hNtDll,
                                   "NtQueryInformationProcess");

  if (query) {
    DWORD hard_error_mode = 0;
    NTSTATUS status = query(::GetCurrentProcess(), kProcessDefaultHardErrorMode, &hard_error_mode,
                            sizeof(hard_error_mode), nullptr);
    if (status >= 0) {
      return (hard_error_mode & 1) ? (hard_error_mode & ~1u) : (hard_error_mode | 1u);
    }
  }

  // Fallback for the case that cannot happen, ntdll being absent: SetErrorMode
  // returns the previous mode, so setting and restoring reads it. Deliberately
  // second, because it is not atomic -- another thread that calls SetErrorMode
  // or raises a hard error in between sees the mode momentarily cleared.
  UINT previous = ::SetErrorMode(0);
  ::SetErrorMode(previous);
  return previous;
}

// Declare the process DPI aware. user32, Vista.
//
// Returns success, and that is not a polite fiction: XP has no DPI
// virtualisation to opt out of. From Vista onward a process that has not said
// this gets lied to -- GetSystemMetrics and the rest report scaled values and
// the window is stretched by the compositor -- and SetProcessDPIAware turns
// that off. XP never does any of it. Every process already sees real pixels
// and real metrics, so the state the caller is asking for is the state it is
// already in, and TRUE is the accurate answer rather than a convenient one.
//
// This is the opposite judgement to CreateSymbolicLinkW or the named pipe
// process IDs, where success would have been a lie about something that had
// not happened. The distinction is whether the postcondition holds, not
// whether any work was done.
BOOL WINAPI ww_SetProcessDPIAware(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hUser32, SetProcessDPIAware)) {
    return system_fn();
  }
  return TRUE;
}

// Force a memory barrier on every processor running this process. kernel32,
// Vista.
//
// The point of the function is to let one thread pay for synchronisation so
// that many others need not: readers publish with plain stores, and a writer
// that must see them all calls this instead of every reader taking a fence.
// It works by interrupting each processor, since taking an interrupt drains
// the store buffer.
//
// XP has no call for it, but it has one that interrupts every processor as a
// side effect. Taking write access away from a page the process has mapped
// means no processor may go on using a cached translation that still permits
// writes, so the kernel sends each one running the process an interprocessor
// interrupt to flush it. That is the trick the Concurrency Runtime -- which is where
// this import comes from -- carries as its own fallback, crediting the CLR
// with years of relying on it; YY-Thunks has it too.
//
// The interrupt is only sent if there is a live, writable translation to
// revoke, and the usual form of the trick leaves that to luck: it protects a
// page that is already read-only, and relies on the page being in the working
// set. This one makes it so on every call -- restore write access, write to
// the page, which puts a writable translation in this processor's cache and
// the page in the working set, then take the access away again. The lock is
// what makes that sequence safe: a second thread arriving between the two
// protection changes would otherwise write to a page just made read-only.
//
// A processor not running one of this process's threads is not interrupted
// and need not be. It reached that state through a context switch, and a
// context switch is itself a barrier.
VOID WINAPI ww_FlushProcessWriteBuffers(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, FlushProcessWriteBuffers)) {
    system_fn();
    return;
  }

  static std::atomic<void*> g_page{nullptr};
  static SRWLOCK g_lock = {};

  void* page = g_page.load(std::memory_order_acquire);
  if (page == nullptr) {
    // One page, for the life of the process. If it cannot be had there is no
    // way to report it -- the function returns nothing -- and the next call
    // tries again.
    void* fresh = ::VirtualAlloc(nullptr, 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (fresh == nullptr) {
      return;
    }
    if (g_page.compare_exchange_strong(page, fresh, std::memory_order_acq_rel)) {
      page = fresh;
    } else {
      ::VirtualFree(fresh, 0, MEM_RELEASE); // Another thread got there first.
    }
  }

  ww_AcquireSRWLockExclusive(&g_lock);
  DWORD previous = 0;
  if (::VirtualProtect(page, 1, PAGE_READWRITE, &previous)) {
    ::InterlockedIncrement(static_cast<LONG volatile*>(page));
    ::VirtualProtect(page, 1, PAGE_READONLY, &previous);
  }
  ww_ReleaseSRWLockExclusive(&g_lock);
}

} // extern "C"
