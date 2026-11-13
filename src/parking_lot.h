// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINWRAP_PARKING_LOT_H_
#define WINWRAP_PARKING_LOT_H_

// Internal: a place for threads to sleep, keyed by address.
//
// This is what lets a synchronisation object be nothing but a word of state.
// The waiters do not live in the object -- they live here, in a global table
// hashed on the object's address -- so an SRWLOCK stays one pointer wide, an
// uncontended acquire stays a single compare-exchange, and nothing is ever
// allocated per lock. A CONDITION_VARIABLE goes further and keeps no state at
// all: its address is the only thing anyone needs. The pattern is the usual
// one: a futex, a keyed event, or WebKit's ParkingLot.
//
// Windows grew its own version of this in Win8, WaitOnAddress, and NT has had
// keyed events since XP. Neither is usable here. WaitOnAddress is far newer
// than the targets; keyed events are undocumented ntdll calls, and the XP
// implementation has a hang on the second wait that YY-Thunks carries an
// explicit workaround for. A thread blocks on its own auto-reset event
// instead -- one handle per thread that ever sleeps, created on first use and
// closed by a TLS callback at thread exit.
//
// Not a general-purpose API. It exists for srwlock.cc and
// condition_variable.cc.

#include "winwrap.h"

namespace winwrap {

  // Which queue on a given address a waiter belongs to. An SRWLOCK parks readers
  // and writers on one address and has to wake them by class. A condition
  // variable has only one kind of waiter and uses kExclusive for all of them.
  enum class Queue { kExclusive, kShared };

  enum class ParkResult {
    kWoken,    // Unparked by someone.
    kTimedOut, // The timeout expired first.
    kDeclined, // `validate` said the reason to sleep had already gone.
  };

  // Wake every waiter of the requested class, for Unpark's `max_to_wake`.
  constexpr size_t kWakeAll = static_cast<size_t>(-1);

  // Sleeps until unparked or `timeout_ms` elapses.
  //
  // `validate` runs while the bucket is locked, which is the whole trick: an
  // unparker must take the same bucket lock before it can observe or clear the
  // "someone is waiting" state an object publishes. Re-checking under that lock
  // closes the window where a waiter decides to sleep just as a releaser decides
  // nobody is waiting. Pass null to always sleep.
  //
  // `before_sleep` runs after this thread is queued but before it blocks, with
  // no lock of ours held. That ordering is what makes a condition variable
  // possible: releasing the caller's mutex there cannot lose a wake-up, because
  // a waiter that is already queued will be found, and its event is auto-reset,
  // so a signal arriving before the wait merely makes the wait return at once.
  // Pass null if there is nothing to do.
  ParkResult Park(const void* address,
                  Queue queue,
                  bool (*validate)(void* context),
                  void (*before_sleep)(void* context),
                  void* context,
                  DWORD timeout_ms);

  // Wakes up to `max_to_wake` waiters of `queue` on `address`.
  //
  // `on_dequeued` runs with the bucket still locked, taking the number woken and
  // whether any waiter of that class remains. Clearing an object's "waiters" bit
  // from there is what keeps the bit honest: it changes under the same lock that
  // governs queue membership, so it cannot contradict the queue. Pass null if
  // there is no such bit.
  void Unpark(const void* address,
              Queue queue,
              size_t max_to_wake,
              void (*on_dequeued)(void* context, size_t woken, bool any_left),
              void* context);

} // namespace winwrap

#endif // WINWRAP_PARKING_LOT_H_
