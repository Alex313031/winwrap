// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Condition variables. kernel32, Vista.
//
// A CONDITION_VARIABLE is one pointer, and this implementation never writes to
// it: its *address* is the key the parking lot queues waiters under, so the
// word stays whatever CONDITION_VARIABLE_INIT left it. That makes a variable
// initialised by the static initialiser and one passed to
// InitializeConditionVariable indistinguishable, which is the behaviour to
// match.
//
// The hard part of a condition variable is not the waiting, it is releasing
// the caller's lock without losing a wake-up. Between a waiter dropping the
// mutex and actually going to sleep there is a window, and a Wake that lands
// inside it must not vanish. The parking lot closes that window with
// before_sleep: this thread is queued first, the lock is released second, and
// only then does it block. A Wake arriving anywhere after the queueing finds
// this waiter and signals its event, and because that event is auto-reset, a
// signal that arrives before the wait simply makes the wait return at once.
//
// A Wake that happens before the waiter queues itself is not a lost wake-up,
// it is a wake-up that preceded the wait -- which is why every correct caller
// re-tests its predicate in a loop. Windows says the same thing by permitting
// spurious wakes.
//
// Both Sleep functions re-acquire the caller's lock on every path out,
// including the timeout, because the caller is entitled to hold it on return
// whatever the result was.

#include "lock_family.h"
#include "parking_lot.h"
#include "winwrap.h"

namespace {

  using winwrap::kWakeAll;
  using winwrap::Park;
  using winwrap::ParkResult;
  using winwrap::Queue;
  using winwrap::Unpark;

  // A condition variable has one kind of waiter, so they all share a queue.
  constexpr Queue kWaiters = Queue::kExclusive;

  struct CriticalSectionWait {
    PCRITICAL_SECTION section;
  };

  void ReleaseCriticalSection(void* context) {
    ::LeaveCriticalSection(static_cast<CriticalSectionWait*>(context)->section);
  }

  struct SRWLockWait {
    PSRWLOCK lock;
    bool shared;
  };

  void ReleaseSRWLock(void* context) {
    auto* wait = static_cast<SRWLockWait*>(context);
    // winwrap's own, deliberately. The lock was taken through this library's
    // implementation, whose state layout is its own; the system's release would
    // be reading a different structure, where it exists at all.
    if (wait->shared) {
      ww_ReleaseSRWLockShared(wait->lock);
    } else {
      ww_ReleaseSRWLockExclusive(wait->lock);
    }
  }

} // namespace

extern "C" {

VOID WINAPI ww_InitializeConditionVariable(PCONDITION_VARIABLE ConditionVariable) {
  if (const ::winwrap::LockFamily* system = ::winwrap::SystemLockFamily()) {
    system->InitializeConditionVariable(ConditionVariable);
    return;
  }
  // Match CONDITION_VARIABLE_INIT exactly. Nothing reads this word; it is
  // zeroed only so that a caller who inspects it, or who copies a freshly
  // initialised variable, sees what Windows would have left.
  ConditionVariable->Ptr = nullptr;
}

BOOL WINAPI ww_SleepConditionVariableCS(PCONDITION_VARIABLE ConditionVariable,
                                        PCRITICAL_SECTION CriticalSection,
                                        DWORD dwMilliseconds) {
  if (const ::winwrap::LockFamily* system = ::winwrap::SystemLockFamily()) {
    return system->SleepConditionVariableCS(ConditionVariable, CriticalSection, dwMilliseconds);
  }
  CriticalSectionWait wait = {CriticalSection};
  ParkResult result        = Park(ConditionVariable, kWaiters, /*validate=*/nullptr,
                                  &ReleaseCriticalSection, &wait, dwMilliseconds);

  ::EnterCriticalSection(CriticalSection);

  if (result == ParkResult::kTimedOut) {
    ::SetLastError(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

BOOL WINAPI ww_SleepConditionVariableSRW(PCONDITION_VARIABLE ConditionVariable,
                                         PSRWLOCK SRWLock,
                                         DWORD dwMilliseconds,
                                         ULONG Flags) {
  if (const ::winwrap::LockFamily* system = ::winwrap::SystemLockFamily()) {
    return system->SleepConditionVariableSRW(ConditionVariable, SRWLock, dwMilliseconds, Flags);
  }
  SRWLockWait wait  = {SRWLock, (Flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0};
  ParkResult result = Park(ConditionVariable, kWaiters, /*validate=*/nullptr, &ReleaseSRWLock,
                           &wait, dwMilliseconds);

  if (wait.shared) {
    ww_AcquireSRWLockShared(SRWLock);
  } else {
    ww_AcquireSRWLockExclusive(SRWLock);
  }

  if (result == ParkResult::kTimedOut) {
    ::SetLastError(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

VOID WINAPI ww_WakeConditionVariable(PCONDITION_VARIABLE ConditionVariable) {
  if (const ::winwrap::LockFamily* system = ::winwrap::SystemLockFamily()) {
    system->WakeConditionVariable(ConditionVariable);
    return;
  }
  // Waking nobody is not a failure. "If no threads are waiting, the function
  // does nothing" -- there is no signal to store for a future waiter, which is
  // exactly why callers re-test a predicate rather than trusting the wake.
  Unpark(ConditionVariable, kWaiters, 1, /*on_dequeued=*/nullptr,
         /*context=*/nullptr);
}

VOID WINAPI ww_WakeAllConditionVariable(PCONDITION_VARIABLE ConditionVariable) {
  if (const ::winwrap::LockFamily* system = ::winwrap::SystemLockFamily()) {
    system->WakeAllConditionVariable(ConditionVariable);
    return;
  }
  Unpark(ConditionVariable, kWaiters, kWakeAll, /*on_dequeued=*/nullptr,
         /*context=*/nullptr);
}

} // extern "C"
