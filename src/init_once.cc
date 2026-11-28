// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The one-time initialisation family -- InitOnceExecuteOnce,
// InitOnceBeginInitialize and InitOnceComplete -- on top of interlocked
// operations and the parking lot.
//
// INIT_ONCE is one pointer wide (RTL_RUN_ONCE is `struct { PVOID Ptr; }`), and
// INIT_ONCE_STATIC_INIT leaves it zero. The low two bits of Ptr are the state,
// numbered the way ntdll numbers them, and the rest is the context once there
// is one -- which is why a context has to be four-byte aligned
// (INIT_ONCE_CTX_RESERVED_BITS):
//
//   0  not started
//   1  a thread is running the synchronous initialisation
//   2  done; the upper bits are the context
//   3  asynchronous initialisation is under way, by any number of threads
//
// Where this departs from ntdll is in state 1. There the upper bits are a
// list of waiters parked on keyed events. Here they stay zero and a waiter
// sleeps in the parking lot, keyed on the INIT_ONCE's address: keyed events
// are undocumented, and XP's have the hang parking_lot.h describes. So the
// word is never anything but 0, 1, 3 or context|2, and winwrap's three
// functions agree with one another -- which is all that is required, because
// a system old enough to need one of them lacks all three.
//
// Nothing is allocated unless a thread actually has to wait, and then only
// that thread's parking event. That matters because libuv's uv_once() sits
// directly on InitOnceExecuteOnce and runs during libuv startup, before much
// else is usable; the uncontended path is two interlocked operations.
//
// The synchronous and asynchronous modes do not mix, as on Windows: a
// synchronous Begin against an INIT_ONCE in state 3, or the reverse, fails
// with ERROR_INVALID_PARAMETER rather than waiting.

#include "dispatch.h"
#include "parking_lot.h"
#include "winwrap.h"

namespace {

  using winwrap::Park;
  using winwrap::Queue;
  using winwrap::Unpark;

  enum : ULONG_PTR {
    kFresh        = 0,
    kSyncRunning  = 1,
    kDone         = 2,
    kAsyncRunning = 3,
    kStateMask    = 3,
  };

  inline PVOID AsPtr(ULONG_PTR value) {
    return reinterpret_cast<PVOID>(value);
  }

  inline PVOID* Slot(PINIT_ONCE once) {
    return &once->Ptr;
  }

  inline ULONG_PTR Load(PVOID* slot) {
    return reinterpret_cast<ULONG_PTR>(::InterlockedCompareExchangePointer(slot, nullptr, nullptr));
  }

  // Swaps `expected` for `desired` and says whether it took.
  inline bool Swap(PVOID* slot, ULONG_PTR desired, ULONG_PTR expected) {
    return ::InterlockedCompareExchangePointer(slot, AsPtr(desired), AsPtr(expected)) ==
           AsPtr(expected);
  }

  // Park's validate hook: sleep only if the synchronous initialiser is still
  // at it. Runs under the bucket lock that Complete's Unpark has to take, so
  // a completion cannot slip between this check and the sleep.
  bool StillRunning(void* context) {
    return (Load(static_cast<PVOID*>(context)) & kStateMask) == kSyncRunning;
  }

  // RtlRunOnceBeginInitialize, returning a Win32 error rather than an
  // NTSTATUS. On ERROR_SUCCESS `*pending` says whether the caller now has
  // initialisation to do (STATUS_PENDING) or it was already done.
  DWORD Begin(PINIT_ONCE once, DWORD flags, bool* pending, PVOID* context) {
    constexpr DWORD kKnown = INIT_ONCE_CHECK_ONLY | INIT_ONCE_ASYNC;
    if (!once || (flags & ~kKnown) || flags == kKnown) {
      return ERROR_INVALID_PARAMETER;
    }
    const bool async = (flags & INIT_ONCE_ASYNC) != 0;
    PVOID* slot      = Slot(once);

    for (;;) {
      const ULONG_PTR value = Load(slot);
      const ULONG_PTR state = value & kStateMask;

      if (state == kDone) {
        if (context) {
          *context = AsPtr(value & ~static_cast<ULONG_PTR>(kStateMask));
        }
        *pending = false;
        return ERROR_SUCCESS;
      }
      if (flags & INIT_ONCE_CHECK_ONLY) {
        return ERROR_GEN_FAILURE; // STATUS_UNSUCCESSFUL: not done yet.
      }

      switch (state) {
        case kFresh:
          if (Swap(slot, async ? kAsyncRunning : kSyncRunning, kFresh)) {
            *pending = true;
            return ERROR_SUCCESS;
          }
          break; // Lost the race to claim it; look again.

        case kSyncRunning:
          if (async) {
            return ERROR_INVALID_PARAMETER;
          }
          // Wait for the initialiser. Whatever wakes us -- success, or a
          // failure that put the state back to fresh -- the loop re-reads.
          Park(once, Queue::kExclusive, StillRunning, /*before_sleep=*/nullptr, slot, INFINITE);
          break;

        default: // kAsyncRunning
          if (!async) {
            return ERROR_INVALID_PARAMETER;
          }
          // Asynchronous initialisers do not exclude one another: each does
          // the work and InitOnceComplete picks the winner.
          *pending = true;
          return ERROR_SUCCESS;
      }
    }
  }

  // RtlRunOnceComplete, likewise returning a Win32 error.
  DWORD Complete(PINIT_ONCE once, DWORD flags, PVOID context) {
    constexpr DWORD kKnown = INIT_ONCE_ASYNC | INIT_ONCE_INIT_FAILED;
    if (!once || (flags & ~kKnown) || flags == kKnown) {
      return ERROR_INVALID_PARAMETER;
    }
    const bool failed       = (flags & INIT_ONCE_INIT_FAILED) != 0;
    const ULONG_PTR payload = reinterpret_cast<ULONG_PTR>(context);
    // A failure carries no context, and a context must leave the state bits
    // free.
    if (payload && (failed || (payload & kStateMask))) {
      return ERROR_INVALID_PARAMETER;
    }
    const ULONG_PTR next = failed ? kFresh : (payload | kDone);

    PVOID* slot           = Slot(once);
    const ULONG_PTR value = Load(slot);

    switch (value & kStateMask) {
      case kSyncRunning:
        if (flags & INIT_ONCE_ASYNC) {
          return ERROR_INVALID_PARAMETER;
        }
        // Only the thread that claimed state 1 moves it on, and waiters never
        // write the word, so there is nothing to race with here.
        ::InterlockedExchangePointer(slot, AsPtr(next));
        Unpark(once, Queue::kExclusive, winwrap::kWakeAll, /*on_dequeued=*/nullptr, nullptr);
        return ERROR_SUCCESS;

      case kAsyncRunning:
        if (!(flags & INIT_ONCE_ASYNC)) {
          return ERROR_INVALID_PARAMETER;
        }
        // Several threads may be here at once. The first swap wins and the
        // rest are told so, to go and fetch the winner's context with
        // INIT_ONCE_CHECK_ONLY. Nobody waits in this mode, so nobody is woken.
        return Swap(slot, next, value) ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;

      default:
        // Fresh or already done: there is no initialisation to complete. An
        // asynchronous loser that arrives after the winner lands here.
        return ERROR_GEN_FAILURE;
    }
  }

} // namespace

extern "C" BOOL WINAPI ww_InitOnceBeginInitialize(LPINIT_ONCE lpInitOnce,
                                                  DWORD dwFlags,
                                                  PBOOL fPending,
                                                  LPVOID* lpContext) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, InitOnceBeginInitialize)) {
    return system_fn(lpInitOnce, dwFlags, fPending, lpContext);
  }
  if (!fPending) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  bool pending      = false;
  const DWORD error = Begin(lpInitOnce, dwFlags, &pending, lpContext);
  if (error != ERROR_SUCCESS) {
    ::SetLastError(error);
    return FALSE;
  }
  *fPending = pending ? TRUE : FALSE;
  return TRUE;
}

extern "C" BOOL WINAPI ww_InitOnceComplete(LPINIT_ONCE lpInitOnce,
                                           DWORD dwFlags,
                                           LPVOID lpContext) {
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, InitOnceComplete)) {
    return system_fn(lpInitOnce, dwFlags, lpContext);
  }

  const DWORD error = Complete(lpInitOnce, dwFlags, lpContext);
  if (error != ERROR_SUCCESS) {
    ::SetLastError(error);
    return FALSE;
  }
  return TRUE;
}

extern "C" BOOL WINAPI ww_InitOnceExecuteOnce(PINIT_ONCE InitOnce,
                                              PINIT_ONCE_FN InitFn,
                                              PVOID Parameter,
                                              LPVOID* Context) {
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, InitOnceExecuteOnce)) {
    return system_fn(InitOnce, InitFn, Parameter, Context);
  }
  if (!InitOnce || !InitFn) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  // Begin fills `Context` itself when the work was already done, including
  // by a thread this one had to wait for.
  bool pending = false;
  DWORD error  = Begin(InitOnce, 0, &pending, Context);
  if (error != ERROR_SUCCESS) {
    ::SetLastError(error);
    return FALSE;
  }
  if (!pending) {
    return TRUE;
  }

  PVOID callback_context = nullptr;
  if (InitFn(InitOnce, Parameter, &callback_context)) {
    error = Complete(InitOnce, 0, callback_context);
    if (error == ERROR_SUCCESS) {
      if (Context) {
        *Context = callback_context;
      }
      return TRUE;
    }
    // The callback produced a context that cannot be stored, one with the
    // low bits set. Fall through and treat that as a failed attempt, so the
    // INIT_ONCE is released rather than left in state 1 with every later
    // caller waiting on it for ever.
    ::SetLastError(error);
  }

  // A failed attempt puts the state back to fresh, leaving the next caller
  // free to try again, and wakes anyone who was waiting so one of them can.
  // The callback's last error is the caller's only clue to what went wrong,
  // so it is carried across the wake-up.
  const DWORD callback_error = ::GetLastError();
  Complete(InitOnce, INIT_ONCE_INIT_FAILED, nullptr);
  ::SetLastError(callback_error);
  return FALSE;
}
