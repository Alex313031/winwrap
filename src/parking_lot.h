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
// allocated per lock. The pattern is the usual one: a futex, a keyed event, or
// WebKit's ParkingLot.
//
// Windows grew its own version of this in Win8, WaitOnAddress, and NT has had
// keyed events since XP. Neither is usable here. WaitOnAddress is far newer
// than the targets; keyed events are undocumented ntdll calls, and the XP
// implementation has a hang on the second wait that YY-Thunks carries an
// explicit workaround for. A thread blocks on its own auto-reset event
// instead -- one handle per thread that ever sleeps, created on first use and
// closed by a TLS callback at thread exit.
//
// Not a general-purpose API. It exists for srwlock.cc, and condition
// variables will want it next.

#include "winwrap.h"

namespace winwrap {

// Which queue on a given address a waiter belongs to. An SRWLOCK parks readers
// and writers on the same address and has to wake them by class.
enum class Queue { kExclusive, kShared };

// Sleeps until unparked, unless `validate` says the reason to sleep is gone.
//
// `validate` runs while the bucket is locked, which is the whole trick: an
// unparker must take the same bucket lock before it can observe or clear the
// "someone is waiting" state an object publishes. Re-checking under that lock
// closes the window where a waiter decides to sleep just as a releaser decides
// nobody is waiting.
//
// Returns true if it parked and was woken, false if `validate` declined.
bool Park(const void* address, Queue queue, bool (*validate)(void* context),
          void* context);

// Wakes waiters on `address`.
//
// `kExclusive` wakes at most one -- handing a lock to several writers at once
// only produces a stampede. `kShared` wakes all of them, since they can all
// proceed together, which is the entire point of a shared lock.
//
// `on_dequeued` runs with the bucket still locked, taking the number woken and
// whether any waiter of that class remains. Clearing an object's "waiters"
// bit from there is what keeps the bit honest: it changes under the same lock
// that governs queue membership, so it cannot contradict the queue.
void Unpark(const void* address, Queue queue,
            void (*on_dequeued)(void* context, size_t woken, bool any_left),
            void* context);

}  // namespace winwrap

#endif  // WINWRAP_PARKING_LOT_H_
