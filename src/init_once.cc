// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// InitOnceExecuteOnce on top of interlocked operations.
//
// INIT_ONCE is one pointer wide (RTL_RUN_ONCE is `struct { PVOID Ptr; }`), and
// INIT_ONCE_STATIC_INIT leaves it zero, so Ptr doubles as the state word:
//
//   0  not started
//   1  a thread is running the callback
//   2  done, callback succeeded
//
// A losing thread spins with SwitchToThread rather than blocking on an event.
// That is cruder than the real implementation, which parks waiters, but
// callbacks behind InitOnce are short by construction and this keeps the shim
// free of any allocation -- which matters because libuv's uv_once() sits
// directly on this, and uv_once runs during libuv startup before much else is
// usable.
//
// Limitation worth knowing: the context value is not preserved. Real
// InitOnceExecuteOnce packs a pointer-aligned context into the same word and
// hands it back to later callers. Here, callers after the first get nullptr.
// libuv passes no context, which is the caller this exists for; anything that
// does need the context should not use this shim without extending it to the
// tagged-pointer scheme.

#include "winwrap.h"

namespace {

enum : ULONG_PTR {
  kUninitialised = 0,
  kRunning = 1,
  kDone = 2,
};

inline PVOID AsPtr(ULONG_PTR value) {
  return reinterpret_cast<PVOID>(value);
}

inline ULONG_PTR Load(PVOID volatile* slot) {
  return reinterpret_cast<ULONG_PTR>(::InterlockedCompareExchangePointer(
      const_cast<PVOID*>(slot), nullptr, nullptr));
}

}  // namespace

extern "C" BOOL WINAPI winwrap_InitOnceExecuteOnce(PINIT_ONCE InitOnce,
                                                   PINIT_ONCE_FN InitFn,
                                                   PVOID Parameter,
                                                   LPVOID* Context) {
  if (!InitOnce || !InitFn) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  PVOID volatile* slot = reinterpret_cast<PVOID volatile*>(&InitOnce->Ptr);

  for (;;) {
    switch (Load(slot)) {
      case kDone:
        if (Context) {
          *Context = nullptr;  // See the limitation note above.
        }
        return TRUE;

      case kUninitialised: {
        // Claim the right to run the callback.
        if (::InterlockedCompareExchangePointer(
                const_cast<PVOID*>(slot), AsPtr(kRunning),
                AsPtr(kUninitialised)) != AsPtr(kUninitialised)) {
          continue;  // Someone else claimed it; re-read the state.
        }

        PVOID callback_context = nullptr;
        const BOOL ok = InitFn(InitOnce, Parameter, &callback_context);

        // On failure reset to uninitialised, which is what the real API does:
        // a failed attempt leaves the next caller free to try again.
        ::InterlockedExchangePointer(const_cast<PVOID*>(slot),
                                     AsPtr(ok ? kDone : kUninitialised));

        if (ok && Context) {
          *Context = callback_context;
        }
        return ok;
      }

      default:
        // kRunning: another thread is in the callback. Yield and re-check.
        if (!::SwitchToThread()) {
          ::Sleep(1);
        }
        break;
    }
  }
}
