// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GetTickCount64 on top of GetTickCount.
//
// GetTickCount returns 32-bit milliseconds and wraps every ~49.7 days.
// GetTickCount64 does not, and callers rely on that: a comparison across a
// wrap would jump backwards by 49 days, which in Chromium and libuv means
// timers that never fire or fire immediately.
//
// So keep the high half ourselves and extend it whenever the low half is
// observed to go backwards. That is correct as long as this is called at least
// once per wrap period, which any running browser does many times a second.
// A process that sleeps for more than 49.7 days between calls would miss a
// wrap -- the same caveat every implementation of this shim has.

#include "winwrap.h"

namespace {

  // Last value handed out, as a full 64-bit count. Interlocked so concurrent
  // callers cannot observe it going backwards.
  volatile LONGLONG g_last_tick = 0;

} // namespace

extern "C" ULONGLONG WINAPI winwrap_GetTickCount64(void) {
  const ULONGLONG now32 = ::GetTickCount();

  for (;;) {
    const LONGLONG prev    = ::InterlockedCompareExchange64(&g_last_tick, 0, 0);
    const ULONGLONG prev_u = static_cast<ULONGLONG>(prev);

    // Splice the fresh low half onto the high half we have been tracking.
    ULONGLONG candidate = (prev_u & 0xFFFFFFFF00000000ull) | now32;

    // Going backwards means the 32-bit counter wrapped since last time.
    if (candidate < prev_u) {
      candidate += 0x100000000ull;
    }

    if (::InterlockedCompareExchange64(&g_last_tick, static_cast<LONGLONG>(candidate), prev) ==
        prev) {
      return candidate;
    }
    // Lost the race; another thread advanced it. Retry against the new value.
  }
}
