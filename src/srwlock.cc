// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A real reader/writer lock, in one pointer, allocating nothing.
//
// This replaces an earlier version that put a lazily allocated
// CRITICAL_SECTION behind SRWLOCK::Ptr and mapped shared acquisition onto
// exclusive. That was correct in the narrow sense -- code written for a
// reader/writer lock is always correct under a mutex -- but it cost every
// reader its concurrency, and it could deadlock outright: two threads each
// holding shared and waiting on the other is legal under real shared
// semantics and a deadlock under a mutex. It also leaked a CRITICAL_SECTION
// per lock, for process lifetime, because SRWLOCK has no destroy call to free
// one from.
//
// The whole state is the pointer:
//
//   bit 0        writer holds it
//   bit 1        at least one writer is parked
//   bit 2        at least one reader is parked
//   bits 3..     count of threads currently holding it shared
//
// SRWLOCK_INIT and InitializeSRWLock both leave it zero, which reads as
// unheld with nobody waiting, so a lock initialised either way works. An
// uncontended acquire of either kind is one compare-exchange. Waiters live in
// parking_lot.cc, keyed on the lock's address, which is what keeps them out of
// the one pointer there is to work with.
//
// Writers are preferred: a reader will not join an existing read while a
// writer is parked. Without that, a steady stream of readers starves writers
// indefinitely. The Try variants deliberately do not honour it -- a try is
// permitted to barge, and the real SRWLOCK barges too.
//
// Still not recursive, and neither is the real one, so that is a match rather
// than a compromise.

#include "parking_lot.h"
#include "winwrap.h"

namespace {

using winwrap::kWakeAll;
using winwrap::Park;
using winwrap::Queue;
using winwrap::Unpark;

constexpr uintptr_t kWriterHeld = 1;
constexpr uintptr_t kWritersWaiting = 2;
constexpr uintptr_t kReadersWaiting = 4;
constexpr uintptr_t kReaderShift = 3;
constexpr uintptr_t kReaderUnit = static_cast<uintptr_t>(1) << kReaderShift;
constexpr uintptr_t kReaderMask = ~static_cast<uintptr_t>(7);

// Long enough to cover a handoff between two threads on different cores,
// short enough that a lock held across real work reaches the kernel quickly.
constexpr unsigned kSpinLimit = 40;

inline uintptr_t Load(PSRWLOCK lock) {
  return reinterpret_cast<uintptr_t>(
      ::InterlockedCompareExchangePointer(&lock->Ptr, nullptr, nullptr));
}

inline bool Cas(PSRWLOCK lock, uintptr_t expected, uintptr_t desired) {
  return ::InterlockedCompareExchangePointer(
             &lock->Ptr, reinterpret_cast<PVOID>(desired),
             reinterpret_cast<PVOID>(expected)) ==
         reinterpret_cast<PVOID>(expected);
}

void ClearBit(PSRWLOCK lock, uintptr_t bit) {
  for (;;) {
    uintptr_t value = Load(lock);
    if (!(value & bit) || Cas(lock, value, value & ~bit)) {
      return;
    }
  }
}

// A writer sleeps only while the lock is taken and its own bit is still up.
// If either stopped being true between deciding to wait and reaching the
// bucket, there is something to retry and nothing to wait for.
bool ValidateExclusive(void* context) {
  uintptr_t value = Load(static_cast<PSRWLOCK>(context));
  return (value & kWritersWaiting) && (value & (kWriterHeld | kReaderMask));
}

// A reader sleeps only while a writer holds it or is queued ahead.
bool ValidateShared(void* context) {
  uintptr_t value = Load(static_cast<PSRWLOCK>(context));
  return (value & kReadersWaiting) && (value & (kWriterHeld | kWritersWaiting));
}

struct WakeContext {
  PSRWLOCK lock;
  uintptr_t bit;
  size_t woken;
};

// Runs with the bucket locked, so clearing the bit here cannot disagree with
// what is actually queued: a thread that set the bit on its way to parking has
// not reached the bucket yet, and will re-validate once it does.
void OnDequeued(void* context, size_t woken, bool any_left) {
  auto* wake = static_cast<WakeContext*>(context);
  wake->woken = woken;
  if (!any_left) {
    ClearBit(wake->lock, wake->bit);
  }
}

// Hands the lock on. One writer if any is queued, otherwise every reader --
// they can all run at once, which is the point of a shared lock.
void WakeNext(PSRWLOCK lock) {
  uintptr_t value = Load(lock);

  if (value & kWritersWaiting) {
    WakeContext wake = {lock, kWritersWaiting, 0};
    Unpark(lock, Queue::kExclusive, 1, &OnDequeued, &wake);
    if (wake.woken) {
      return;
    }
    // The bit outlived its queue: a waiter gave up in validation after
    // setting it. OnDequeued has cleared it, so fall through rather than
    // leave parked readers asleep with nobody left to wake them.
    value = Load(lock);
  }

  if (value & kReadersWaiting) {
    WakeContext wake = {lock, kReadersWaiting, 0};
    Unpark(lock, Queue::kShared, kWakeAll, &OnDequeued, &wake);
  }
}

}  // namespace

extern "C" {

VOID WINAPI winwrap_InitializeSRWLock(PSRWLOCK SRWLock) {
  SRWLock->Ptr = nullptr;
}

VOID WINAPI winwrap_AcquireSRWLockExclusive(PSRWLOCK SRWLock) {
  if (Cas(SRWLock, 0, kWriterHeld)) {
    return;
  }

  for (unsigned spins = 0;;) {
    uintptr_t value = Load(SRWLock);

    if ((value & (kWriterHeld | kReaderMask)) == 0) {
      if (Cas(SRWLock, value, value | kWriterHeld)) {
        return;
      }
      continue;
    }

    // Only spin while nobody is parked. Once there is a queue, spinning just
    // takes cycles from the thread holding the lock.
    if (spins < kSpinLimit &&
        (value & (kWritersWaiting | kReadersWaiting)) == 0) {
      ++spins;
      YieldProcessor();
      continue;
    }

    if (!(value & kWritersWaiting) &&
        !Cas(SRWLock, value, value | kWritersWaiting)) {
      continue;
    }

    Park(SRWLock, Queue::kExclusive, &ValidateExclusive, nullptr, SRWLock,
         INFINITE);
    spins = 0;
  }
}

VOID WINAPI winwrap_ReleaseSRWLockExclusive(PSRWLOCK SRWLock) {
  for (;;) {
    uintptr_t value = Load(SRWLock);
    uintptr_t next = value & ~kWriterHeld;
    if (!Cas(SRWLock, value, next)) {
      continue;
    }
    if (next & (kWritersWaiting | kReadersWaiting)) {
      WakeNext(SRWLock);
    }
    return;
  }
}

VOID WINAPI winwrap_AcquireSRWLockShared(PSRWLOCK SRWLock) {
  for (unsigned spins = 0;;) {
    uintptr_t value = Load(SRWLock);

    if ((value & (kWriterHeld | kWritersWaiting)) == 0) {
      if (Cas(SRWLock, value, value + kReaderUnit)) {
        return;
      }
      continue;
    }

    if (spins < kSpinLimit && !(value & kReadersWaiting)) {
      ++spins;
      YieldProcessor();
      continue;
    }

    if (!(value & kReadersWaiting) &&
        !Cas(SRWLock, value, value | kReadersWaiting)) {
      continue;
    }

    Park(SRWLock, Queue::kShared, &ValidateShared, nullptr, SRWLock, INFINITE);
    spins = 0;
  }
}

VOID WINAPI winwrap_ReleaseSRWLockShared(PSRWLOCK SRWLock) {
  for (;;) {
    uintptr_t value = Load(SRWLock);
    uintptr_t next = value - kReaderUnit;
    if (!Cas(SRWLock, value, next)) {
      continue;
    }
    // Only the reader that turns the count to zero owes anyone a wake.
    if ((next & kReaderMask) == 0 &&
        (next & (kWritersWaiting | kReadersWaiting))) {
      WakeNext(SRWLock);
    }
    return;
  }
}

BOOLEAN WINAPI winwrap_TryAcquireSRWLockExclusive(PSRWLOCK SRWLock) {
  uintptr_t value = Load(SRWLock);
  if (value & (kWriterHeld | kReaderMask)) {
    return FALSE;
  }
  return Cas(SRWLock, value, value | kWriterHeld) ? TRUE : FALSE;
}

BOOLEAN WINAPI winwrap_TryAcquireSRWLockShared(PSRWLOCK SRWLock) {
  for (;;) {
    uintptr_t value = Load(SRWLock);
    if (value & kWriterHeld) {
      return FALSE;
    }
    if (Cas(SRWLock, value, value + kReaderUnit)) {
      return TRUE;
    }
  }
}

}  // extern "C"
