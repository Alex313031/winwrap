// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Fibers.
//
// XP has fibers. What it does not have is ConvertThreadToFiberEx, nor a
// CreateFiberEx that accepts FIBER_FLAG_FLOAT_SWITCH, nor IsThreadAFiber --
// and the three are used together, so they have to agree about that flag.
//
// They did not, and it cost a silent startup failure. Electron's 32-bit
// wWinMain converts the main thread to a fiber and creates a 4 MiB one to run
// on, because the executable reserves only 512 KiB for the main thread and
// Chromium's initialisation needs more. Both calls pass
// FIBER_FLAG_FLOAT_SWITCH. winwrap supplied ConvertThreadToFiberEx and quietly
// ignored the flag, while XP's own CreateFiberEx refused it outright with
// ERROR_INVALID_PARAMETER -- so the conversion succeeded, the big fiber was
// never created, and startup ran on in 512 KiB until it overflowed. No window,
// no log, no crash dialog.
//
// So CreateFiberEx is here too, even though XP exports one, because the
// version XP exports does not do what a caller asking for that flag needs.

#include "winwrap.h"

#if !defined(FIBER_FLAG_FLOAT_SWITCH)
 #define FIBER_FLAG_FLOAT_SWITCH 0x1
#endif

namespace {

  // The real kernel32 export, where there is one. Deferring rather than
  // replacing is deliberate for these: on a system that implements the flag
  // properly there is no reason to give up float switching just because the
  // binary also runs on XP. It is the same reasoning as pipes.cc, and the two
  // together are becoming winwrap's rule for functions that exist but behave
  // differently, as opposed to functions that are simply absent.
  FARPROC Kernel32Proc(const char* name) {
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    return kernel32 ? ::GetProcAddress(kernel32, name) : nullptr;
  }

} // namespace

extern "C" {

// ConvertThreadToFiber with a flags argument. The only flag is
// FIBER_FLAG_FLOAT_SWITCH, asking that x87/MMX state be switched along with
// the fiber, and XP's fibers simply do not do that -- it is the capability
// Vista added, not a parameter XP's version is missing.
//
// So the flag is accepted and ignored rather than refused. Refusing would be
// the more literal reading, but it makes callers worse off: OpenSSL's async
// engine, the only one here, responds to a NULL return by calling
// GetCurrentFiber() on a thread that was never converted and carrying on as
// though it had been (crypto/async/arch/async_win.c). Converting without float
// switching is a real fiber; the alternative is a fake one.
//
// A caller that genuinely depends on float switching cannot be satisfied on
// XP by any means, so there is no answer here that is both honest and useful.
LPVOID WINAPI ww_ConvertThreadToFiberEx(LPVOID lpParameter, DWORD dwFlags) {
  using ConvertFunction = LPVOID(WINAPI*)(LPVOID, DWORD);
  static const auto real =
      reinterpret_cast<ConvertFunction>(Kernel32Proc("ConvertThreadToFiberEx"));
  if (real) {
    return real(lpParameter, dwFlags);
  }

  // XP: the flag is unavailable, so drop it rather than refuse the call.
  return ::ConvertThreadToFiber(lpParameter);
}

// CreateFiberEx, with FIBER_FLAG_FLOAT_SWITCH made optional.
//
// XP exports this and accepts every argument except that flag, which it
// rejects with ERROR_INVALID_PARAMETER -- turning a request for a larger stack
// into no fiber at all. The flag is an optimisation to the fiber, not a
// property the caller can observe directly, so retrying without it gives the
// caller the thing it actually asked for.
//
// Only retried on ERROR_INVALID_PARAMETER, and only when the flag was set. A
// failure for any other reason -- out of memory, a bad start address -- is the
// caller's to see, and retrying it would just fail twice.
LPVOID WINAPI ww_CreateFiberEx(SIZE_T dwStackCommitSize,
                               SIZE_T dwStackReserveSize,
                               DWORD dwFlags,
                               LPFIBER_START_ROUTINE lpStartAddress,
                               LPVOID lpParameter) {
  using CreateFunction   = LPVOID(WINAPI*)(SIZE_T, SIZE_T, DWORD, LPFIBER_START_ROUTINE, LPVOID);
  static const auto real = reinterpret_cast<CreateFunction>(Kernel32Proc("CreateFiberEx"));

  if (!real) {
    // Not expected -- XP has it -- but CreateFiber takes the reserve size, so
    // the stack the caller asked for still survives.
    return ::CreateFiber(dwStackReserveSize, lpStartAddress, lpParameter);
  }

  LPVOID fiber = real(dwStackCommitSize, dwStackReserveSize, dwFlags, lpStartAddress, lpParameter);
  if (fiber != nullptr) {
    return fiber;
  }

  if ((dwFlags & FIBER_FLAG_FLOAT_SWITCH) != 0 && ::GetLastError() == ERROR_INVALID_PARAMETER) {
    return real(dwStackCommitSize, dwStackReserveSize, dwFlags & ~FIBER_FLAG_FLOAT_SWITCH,
                lpStartAddress, lpParameter);
  }
  return fiber;
}

// Whether this thread is running as a fiber.
//
// The test is on the TEB field GetCurrentFiber() reads, which is a union: for
// a fiber it is the fiber's data pointer, and for an ordinary thread it is the
// NT "Version" value, 0x1E00. Neither NULL nor that sentinel means a fiber.
//
// A heuristic rather than a derivation, but it is the same one Chromium itself
// falls back to where this function is absent -- chrome/app/chrome_exe_main_win.cc
// does exactly this comparison after GetProcAddress fails -- so a build using
// winwrap and one not using it agree on the answer.
BOOL WINAPI ww_IsThreadAFiber(void) {
  PVOID fiber_data = ::GetCurrentFiber();
  if (fiber_data == nullptr ||
      fiber_data == reinterpret_cast<PVOID>(static_cast<uintptr_t>(0x1E00))) {
    return FALSE;
  }
  return TRUE;
}

} // extern "C"
