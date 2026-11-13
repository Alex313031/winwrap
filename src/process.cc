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

  using SetProcessDPIAwareFn = BOOL(WINAPI*)(void);

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
  static const auto query = []() -> NtQueryInformationProcessFn {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll ? reinterpret_cast<NtQueryInformationProcessFn>(
                       ::GetProcAddress(ntdll, "NtQueryInformationProcess"))
                 : nullptr;
  }();

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
  // Where the system has its own, use it: on Vista and later this genuinely
  // changes process behaviour and must not be skipped.
  static const auto real = []() -> SetProcessDPIAwareFn {
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<SetProcessDPIAwareFn>(
                        ::GetProcAddress(user32, "SetProcessDPIAware"))
                  : nullptr;
  }();
  if (real) {
    return real();
  }
  return TRUE;
}

} // extern "C"
