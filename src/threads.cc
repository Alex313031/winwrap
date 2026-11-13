// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Thread functions. Fibers live in fibers.cc; everything else thread-shaped
// belongs here.

#include <intrin.h> // __cpuid

#include "dispatch.h"
#include "winwrap.h"

extern "C" {

// Which processor the calling thread is running on. kernel32, Server 2003.
//
// XP has no call for this, but the processor will say so itself: CPUID leaf 1
// returns the initial APIC ID in the top eight bits of EBX, and firmware
// assigns those as 0..N-1 across logical processors. That is the technique
// everyone used before the API existed.
//
// Two honest limitations, neither of which XP can be configured into:
//
//  - The ID is eight bits, so it cannot describe more than 256 logical
//    processors. XP's own ceiling is 32, and 64 on x64, well inside that.
//  - Firmware is not obliged to number them densely or in order. In practice
//    the systems XP runs on do, which is why the technique was relied upon.
//
// The answer is a hint whatever produces it. A thread can be migrated to
// another processor between this returning and the caller reading it, which is
// equally true of the real function -- the result picks a shard or a
// per-processor cache, and is never load-bearing for correctness.
//
// Uses the __cpuid intrinsic rather than inline assembly, which keeps one
// implementation for both architectures. YY-Thunks uses inline asm here and so
// is x86-only; it can afford that, because its x64 target is Server 2003 and
// already has the real function.
DWORD WINAPI winwrap_GetCurrentProcessorNumber(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WINWRAP_SYSTEM(::winwrap::Module::hKernel32, GetCurrentProcessorNumber)) {
    return system_fn();
  }
  int registers[4] = {};
  __cpuid(registers, 1);
  // registers[1] is EBX; bits 31..24 hold the initial APIC ID.
  return (static_cast<DWORD>(registers[1]) >> 24) & 0xFF;
}

} // extern "C"
