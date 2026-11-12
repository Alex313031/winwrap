// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// SRWLOCK on top of CRITICAL_SECTION.
//
// SRWLOCK is one pointer wide (RTL_SRWLOCK is `struct { PVOID Ptr; }`) and a
// CRITICAL_SECTION does not fit in it, so Ptr holds a pointer to a lazily
// allocated one. SRWLOCK_INIT and InitializeSRWLock both leave Ptr null, which
// is the "not yet allocated" state, so a lock initialised either way works.
//
// Two deliberate semantic differences, both safe but worth knowing:
//
//  - Shared acquisition is exclusive. A CRITICAL_SECTION has no shared mode,
//    so readers serialise against each other. That costs concurrency and never
//    correctness: code written for a reader/writer lock is always correct under
//    a mutex.
//  - CRITICAL_SECTION is recursive, SRWLOCK is not. Taking the same lock twice
//    on one thread deadlocks on a real SRWLOCK and succeeds here. That turns a
//    deadlock into forward progress, so it hides bugs rather than creating
//    them, but it means this is not a conformance-grade replacement.
//
// The allocation is never freed. That is not a leak that can be fixed: the
// SRWLOCK API has no destroy call, so there is no hook to free it from. One
// CRITICAL_SECTION per lock, for process lifetime.
//
// Chromium's own base takes this same trade in
// patches/chromium/base/0315-migrate-from-critical-section-to-srwlock.patch,
// which reverts upstream's move to SRWLOCK and gets LockImpl back onto
// critical sections.

#include "winwrap.h"

namespace {

// Spin a little before sleeping, which is what the real SRWLOCK does and what
// makes uncontended acquisition cheap.
constexpr DWORD kSpinCount = 2000;

CRITICAL_SECTION* EnsureSection(PSRWLOCK lock) {
  PVOID volatile* slot = reinterpret_cast<PVOID volatile*>(&lock->Ptr);

  // Fast path: already installed. A plain read would be enough on x86/x64 for
  // a pointer-sized slot, but this keeps the atomicity explicit.
  if (PVOID existing = ::InterlockedCompareExchangePointer(
          const_cast<PVOID*>(slot), nullptr, nullptr)) {
    return static_cast<CRITICAL_SECTION*>(existing);
  }

  auto* fresh = static_cast<CRITICAL_SECTION*>(
      ::HeapAlloc(::GetProcessHeap(), 0, sizeof(CRITICAL_SECTION)));
  if (!fresh) {
    // Nothing useful to return and no way to report failure through a void
    // API. Failing loudly beats handing back a lock that does not lock.
    ::RaiseException(STATUS_NO_MEMORY, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return nullptr;
  }

  // The AndSpinCount form returns a BOOL instead of raising on low memory, and
  // is available on XP.
  if (!::InitializeCriticalSectionAndSpinCount(fresh, kSpinCount)) {
    ::HeapFree(::GetProcessHeap(), 0, fresh);
    ::RaiseException(STATUS_NO_MEMORY, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return nullptr;
  }

  PVOID won = ::InterlockedCompareExchangePointer(const_cast<PVOID*>(slot),
                                                  fresh, nullptr);
  if (won == nullptr) {
    return fresh;  // We installed ours.
  }

  // Another thread got there first; discard ours and use theirs.
  ::DeleteCriticalSection(fresh);
  ::HeapFree(::GetProcessHeap(), 0, fresh);
  return static_cast<CRITICAL_SECTION*>(won);
}

}  // namespace

extern "C" {

VOID WINAPI winwrap_InitializeSRWLock(PSRWLOCK SRWLock) {
  // Match SRWLOCK_INIT rather than allocating here, so a lock that was
  // zero-initialised instead of initialised through this call behaves the
  // same. The CRITICAL_SECTION appears on first acquisition.
  SRWLock->Ptr = nullptr;
}

VOID WINAPI winwrap_AcquireSRWLockExclusive(PSRWLOCK SRWLock) {
  ::EnterCriticalSection(EnsureSection(SRWLock));
}

VOID WINAPI winwrap_ReleaseSRWLockExclusive(PSRWLOCK SRWLock) {
  ::LeaveCriticalSection(EnsureSection(SRWLock));
}

// Shared maps onto exclusive; see the note at the top of the file.
VOID WINAPI winwrap_AcquireSRWLockShared(PSRWLOCK SRWLock) {
  ::EnterCriticalSection(EnsureSection(SRWLock));
}

VOID WINAPI winwrap_ReleaseSRWLockShared(PSRWLOCK SRWLock) {
  ::LeaveCriticalSection(EnsureSection(SRWLock));
}

BOOLEAN WINAPI winwrap_TryAcquireSRWLockExclusive(PSRWLOCK SRWLock) {
  return ::TryEnterCriticalSection(EnsureSection(SRWLock)) ? TRUE : FALSE;
}

BOOLEAN WINAPI winwrap_TryAcquireSRWLockShared(PSRWLOCK SRWLock) {
  return ::TryEnterCriticalSection(EnsureSection(SRWLock)) ? TRUE : FALSE;
}

}  // extern "C"
