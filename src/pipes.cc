// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Named pipes. kernel32, Vista.
//
// Who is at the other end of a pipe. Vista added this by teaching npfs to
// record the connecting process and exposing it through a new control code;
// XP's npfs does not record it, so on XP there is usually no answer to give.
//
// "Usually" rather than "never", because this does not assume which Windows it
// is running on. Three steps, in order:
//
//   1. If kernel32 exports the real function, call it. See the note on that
//      below -- it is a deliberate exception to how the rest of winwrap works.
//   2. Otherwise ask npfs directly, through the control code the real function
//      uses. A kernel that implements it answers correctly whatever its
//      version; XP's does not, and says so.
//   3. Otherwise fail with ERROR_NOT_SUPPORTED.
//
// === What is deliberately not done ===
//
// There is an inference available that looks like a free answer and is a trap.
// GetNamedPipeInfo reports whether a handle is the server or the client end,
// so a process holding the client end could report itself as the client. It
// would often be right and sometimes be badly wrong: a handle can be inherited
// or duplicated, and then the process holding it is not the process that
// connected. Chromium and Node pass pipe handles to child processes routinely.
//
// Being sometimes wrong matters more here than for most functions, because of
// what the answer is used for. Mojo calls GetNamedPipeServerProcessId on a
// handle it holds as the *client*, opens that process, and refuses to proceed
// unless the server's integrity level is at least its own
// (mojo/public/cpp/platform/named_platform_channel_win.cc). Answering with the
// caller's own process ID would make it open itself, compare its integrity
// level against itself, and pass -- turning an anti-spoofing check into a
// formality. A plausible wrong answer is worse than no answer.
//
// Both callers in Electron handle failure correctly: that one returns false,
// and ipcz_driver/invitation.cc abandons the connection when it cannot
// identify the peer.
//
// The session-ID and client-computer-name siblings are absent for the same
// reason and are not imported by anything here.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // CTL_CODE(FILE_DEVICE_NAMED_PIPE, 14, METHOD_BUFFERED, FILE_ANY_ACCESS).
  constexpr ULONG kFsctlPipeQueryClientProcess = 0x110038;

  struct PipeClientProcessBuffer {
    PVOID ClientSession;
    PVOID ClientProcess;
  };

  using NtFsControlFileFn = NTSTATUS(
      NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);
  using PipeProcessIdFn = BOOL(WINAPI*)(HANDLE, PULONG);

  // Calls the real kernel32 export when the running system has one.
  //
  // This is the only place in winwrap that defers to the system rather than
  // replacing it, and it is deliberate. Everywhere else, winwrap either
  // implements the function properly or the substitution is required for
  // consistency -- the SRWLOCK family has to be all ours or all the system's,
  // for instance. Here there is nothing to implement and nothing to keep
  // consistent: a winwrap-linked binary on Vista would otherwise lose a function
  // that works, purely because it also wants to run on XP. Whether this should
  // become winwrap's general behaviour is an open question, recorded in
  // electron-legacy's TODO; for now it is one exception with a reason.
  PipeProcessIdFn RealExport(const char* name) {
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    return kernel32 ? reinterpret_cast<PipeProcessIdFn>(::GetProcAddress(kernel32, name)) : nullptr;
  }

} // namespace

extern "C" {

BOOL WINAPI winwrap_GetNamedPipeClientProcessId(HANDLE Pipe, PULONG ClientProcessId) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WINWRAP_SYSTEM(::winwrap::Module::hKernel32, GetNamedPipeClientProcessId)) {
    return system_fn(Pipe, ClientProcessId);
  }
  if (!ClientProcessId) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  static const PipeProcessIdFn real = RealExport("GetNamedPipeClientProcessId");
  if (real) {
    return real(Pipe, ClientProcessId);
  }

  static const auto control = []() -> NtFsControlFileFn {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll ? reinterpret_cast<NtFsControlFileFn>(::GetProcAddress(ntdll, "NtFsControlFile"))
                 : nullptr;
  }();
  if (control) {
    PipeClientProcessBuffer buffer = {};
    IO_STATUS_BLOCK iosb           = {};
    NTSTATUS status = control(Pipe, nullptr, nullptr, nullptr, &iosb, kFsctlPipeQueryClientProcess,
                              nullptr, 0, &buffer, sizeof(buffer));
    if (status >= 0) {
      *ClientProcessId = static_cast<ULONG>(reinterpret_cast<ULONG_PTR>(buffer.ClientProcess));
      return TRUE;
    }
    // Anything else means npfs does not implement the control code, which is
    // the XP case, and is reported below rather than as a device error.
  }

  ::SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}

BOOL WINAPI winwrap_GetNamedPipeServerProcessId(HANDLE Pipe, PULONG ServerProcessId) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WINWRAP_SYSTEM(::winwrap::Module::hKernel32, GetNamedPipeServerProcessId)) {
    return system_fn(Pipe, ServerProcessId);
  }
  if (!ServerProcessId) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  static const PipeProcessIdFn real = RealExport("GetNamedPipeServerProcessId");
  if (real) {
    return real(Pipe, ServerProcessId);
  }

  // No equivalent of the client control code exists for the server end: npfs
  // on XP records neither party, and knowing the client would not identify the
  // server. Nothing to try.
  ::SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}

} // extern "C"
