// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The SRWLOCK and CONDITION_VARIABLE functions, resolved as one unit.
//
// Every other shim here can decide for itself whether to use the system's
// version, because the arguments mean the same thing to both. These twelve
// cannot. An SRWLOCK is one opaque pointer-sized word, and winwrap's
// implementation gives it a meaning of its own -- writer-held in bit 0,
// waiters in bits 1 and 2, a reader count above that -- which has nothing to
// do with whatever the kernel keeps there. Half a lock from each
// implementation is not a degraded lock, it is memory corruption: the
// system's release would walk winwrap's bits as a wait list, and winwrap's
// acquire would hand out a lock the kernel thinks someone else holds.
//
// CONDITION_VARIABLE joins them because SleepConditionVariableSRW takes an
// SRWLOCK and releases it, so it has to agree about what that word means.
//
// Which makes the whole group all-or-nothing, and the oldest Windows that can
// supply all of it decides where the line falls. That is Windows 7, not
// Vista: Vista exports AcquireSRWLock* and ReleaseSRWLock* but not
// TryAcquireSRWLock*, and there is no lower-level way around it either --
// Vista's ntdll exports RtlAcquireSRWLockExclusive, RtlAcquireSRWLockShared,
// RtlInitializeSRWLock, RtlReleaseSRWLockExclusive, RtlReleaseSRWLockShared
// and RtlUpdateClonedSRWLock, and no try-variant at all (checked against a
// Vista SP2 ntdll, both architectures).
//
// So on Vista these locks stay winwrap's, and from Windows 7 they are the
// system's. Vista pays for that with a parking-lot lock instead of a kernel
// one, which is the correct trade: a slower lock is a cost, a mixed lock is a
// crash.

#ifndef WINWRAP_LOCK_FAMILY_H_
#define WINWRAP_LOCK_FAMILY_H_

#include "framework.h"
#include "winwrap.h"

namespace winwrap {

  // The system's twelve, or nothing. Members are named after the functions so a
  // call site reads the same as the call it replaces.
  // Members are typed from winwrap's own shims: these sources compile against
  // XP headers, where none of the twelve are declared at all. See
  // WINWRAP_SYSTEM in dispatch.h.
  struct LockFamily {
    decltype(&winwrap_InitializeSRWLock) InitializeSRWLock;
    decltype(&winwrap_AcquireSRWLockExclusive) AcquireSRWLockExclusive;
    decltype(&winwrap_AcquireSRWLockShared) AcquireSRWLockShared;
    decltype(&winwrap_ReleaseSRWLockExclusive) ReleaseSRWLockExclusive;
    decltype(&winwrap_ReleaseSRWLockShared) ReleaseSRWLockShared;
    decltype(&winwrap_TryAcquireSRWLockExclusive) TryAcquireSRWLockExclusive;
    decltype(&winwrap_TryAcquireSRWLockShared) TryAcquireSRWLockShared;
    decltype(&winwrap_InitializeConditionVariable) InitializeConditionVariable;
    decltype(&winwrap_SleepConditionVariableCS) SleepConditionVariableCS;
    decltype(&winwrap_SleepConditionVariableSRW) SleepConditionVariableSRW;
    decltype(&winwrap_WakeConditionVariable) WakeConditionVariable;
    decltype(&winwrap_WakeAllConditionVariable) WakeAllConditionVariable;
  };

  // Returns the system's set, or nullptr if this Windows is missing any part of
  // it -- in which case every one of the twelve must use winwrap's own code.
  //
  // Resolved once. After that this is a relaxed load and a compare, so the
  // Windows 7 and later path through a lock shim is that plus an indirect call.
  const LockFamily* SystemLockFamily() noexcept;

} // namespace winwrap

#endif // WINWRAP_LOCK_FAMILY_H_
