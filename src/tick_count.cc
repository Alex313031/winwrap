// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The clocks: GetTickCount64 and GetSystemTimePreciseAsFileTime.
//
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

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // Last value handed out, as a full 64-bit count. Interlocked so concurrent
  // callers cannot observe it going backwards.
  volatile LONGLONG g_last_tick = 0;

} // namespace

extern "C" ULONGLONG WINAPI ww_GetTickCount64(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetTickCount64)) {
    return system_fn();
  }
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

// The system time at the best resolution the system has. kernel32, Windows 8
// -- so absent from 7 as well as from Vista and XP.
//
// Windows 8 added it to read the clock between ticks: GetSystemTimeAsFileTime
// returns the time as of the last timer interrupt, which moves in steps of up
// to 15.6 ms, and this one interpolates to well under a microsecond. Before
// Windows 8 there is no interpolated clock to read, so the answer here is the
// tick-resolution one. It is the right time, to the precision the system
// keeps it, and it comes from the same clock -- which matters more than the
// digits, because callers compare this against file times and against
// GetSystemTimeAsFileTime, and a value built from a second clock would drift
// against both.
//
// How coarse "tick resolution" is depends on the timer interrupt, and that is
// where the HPET comes in -- less directly than it might seem. The system
// time advances once per clock interrupt, so the step this returns is the
// interrupt period: 15.625 ms by default, 10 ms on some older uniprocessor
// machines.
//
// A High Precision Event Timer does not shorten that by being present. XP
// does not use one at all. From Vista on it can be switched on as the
// platform clock -- `bcdedit /set useplatformclock true` -- and what that
// changes is the timer the system counts with: QueryPerformanceCounter then
// reads the HPET, and a timer period that has been set short is kept much
// closer to what was asked for than the older timers manage. The step of the
// system time is still one interrupt period either way.
//
// What narrows the step is a program asking for it, with timeBeginPeriod(1)
// or the like, which raises the interrupt rate for the whole system. With
// the period at 1 ms this function's answers are 1 ms apart instead of 15.6,
// on any of the three Windows it stands in for, and with the HPET enabled
// that 1 ms is close to exact. So the two together are how to get the most
// out of this fallback: enable the HPET for an accurate period, and request
// a short one. winwrap does not raise the rate on a caller's behalf, since
// that costs every process on the machine power and is not this function's
// to decide.
//
// Staying on the one clock is also what rules out the tempting improvement,
// adding QueryPerformanceCounter's elapsed time to a base reading. It gives
// more digits, but the two clocks run at slightly different rates and are
// corrected differently, so the result walks away from system time and has
// to be snapped back, which shows up as time going backwards. Coarse and
// correct is the better trade.
extern "C" VOID WINAPI ww_GetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, GetSystemTimePreciseAsFileTime)) {
    system_fn(lpSystemTimeAsFileTime);
    return;
  }
  ::GetSystemTimeAsFileTime(lpSystemTimeAsFileTime);
}
