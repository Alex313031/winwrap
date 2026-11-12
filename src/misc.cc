// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Functions with no better home.

// winwrap.h first: it brings in windows.h, which winternl.h needs to have been
// included before it. See files.cc for the failure mode when it is not.
#include "winwrap.h"

#include <winternl.h>

namespace {

// PROCESSINFOCLASS as winternl.h declares it names only a few members, and
// this is not among them.
constexpr ULONG kProcessDefaultHardErrorMode = 12;

using NtQueryInformationProcessFn =
    NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

}  // namespace

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
UINT WINAPI winwrap_GetErrorMode(void) {
  static const auto query = []() -> NtQueryInformationProcessFn {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll ? reinterpret_cast<NtQueryInformationProcessFn>(
                       ::GetProcAddress(ntdll, "NtQueryInformationProcess"))
                 : nullptr;
  }();

  if (query) {
    DWORD hard_error_mode = 0;
    NTSTATUS status =
        query(::GetCurrentProcess(), kProcessDefaultHardErrorMode,
              &hard_error_mode, sizeof(hard_error_mode), nullptr);
    if (status >= 0) {
      return (hard_error_mode & 1) ? (hard_error_mode & ~1u)
                                   : (hard_error_mode | 1u);
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

// Register for automatic restart after a crash or an update. kernel32, Vista.
//
// Part of Application Recovery and Restart, which XP does not have in any
// form: there is no Restart Manager, and Windows Error Reporting on XP has no
// mechanism for relaunching an application it terminated. Nothing to call and
// nothing to approximate.
//
// E_NOTIMPL rather than a Win32-derived HRESULT, because that is the plainest
// statement of the situation and callers test only for failure. The sole
// caller in Chromium resolves this through GetProcAddress and simply does not
// register when it is absent (chrome/browser/chrome_browser_main_win.cc).
HRESULT WINAPI winwrap_RegisterApplicationRestart(PCWSTR pwzCommandline,
                                                  DWORD dwFlags) {
  UNREFERENCED_PARAMETER(pwzCommandline);
  UNREFERENCED_PARAMETER(dwFlags);
  return E_NOTIMPL;
}

// The other half of the pair, and the same answer.
//
// S_OK would be defensible -- nothing can be registered, so the state this
// asks for already holds, and returning success would spare a caller a
// pointless error path. It is rejected because the two halves should not
// disagree about whether the facility exists: a caller told E_NOTIMPL by
// RegisterApplicationRestart learns the same thing here, instead of being told
// that an unregistration it never made has been undone.
//
// Nothing in Electron imports this. It is here because a library that
// implements one half of a pair and not the other is a worse library, and this
// half costs a line.
HRESULT WINAPI winwrap_UnregisterApplicationRestart(void) {
  return E_NOTIMPL;
}

}  // extern "C"
