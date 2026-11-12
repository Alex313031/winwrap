// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Fiber helpers. kernel32, Vista.
//
// XP has fibers; it does not have these two conveniences on top of them.

#include "winwrap.h"

#if !defined(FIBER_FLAG_FLOAT_SWITCH)
#define FIBER_FLAG_FLOAT_SWITCH 0x1
#endif

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
LPVOID WINAPI winwrap_ConvertThreadToFiberEx(LPVOID lpParameter,
                                             DWORD dwFlags) {
  UNREFERENCED_PARAMETER(dwFlags);
  return ::ConvertThreadToFiber(lpParameter);
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
BOOL WINAPI winwrap_IsThreadAFiber(void) {
  PVOID fiber_data = ::GetCurrentFiber();
  if (fiber_data == nullptr ||
      fiber_data == reinterpret_cast<PVOID>(static_cast<uintptr_t>(0x1E00))) {
    return FALSE;
  }
  return TRUE;
}

}  // extern "C"
