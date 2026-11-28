// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// System Information functions.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // Number of set bits in an affinity mask.
  //
  // Kernighan's loop rather than a popcount intrinsic: POPCNT is SSE4.2, and a
  // machine old enough to need this shim will meet it as an invalid opcode
  // rather than a slow path.
  DWORD CountSetBits(DWORD_PTR mask) {
    DWORD count = 0;
    for (; mask != 0; mask &= mask - 1) {
      ++count;
    }
    return count;
  }

} // namespace

extern "C" {

// GetLogicalProcessorInformation: Windows XP SP3 and Server 2003 SP1.
//
// The import is the thing that matters here. A binary referencing this fails
// to load on XP SP2 before a line of its own code runs, whether or not it ever
// calls it -- and the MSVC CRT references it while guarding the call behind a
// version check, so the usual case is a program that imports it and never
// calls it at all.
//
// What gets built when the system has no implementation is an approximation,
// and it is worth being exact about which part is approximate. The processor
// *count* is right: it comes from GetSystemInfo's active processor mask, which
// is the same number every other API reports. The *topology* is not. Telling a
// core from a hyperthread needs CPUID, so every active processor is reported
// as its own core with no SMT sibling, and no cache entries are reported.
//
// That is the right trade for the callers this exists to serve. "How many
// processors are there" gets a correct answer; "how many physical cores, and
// which caches do they share" gets a conservative one instead of a failure. A
// caller doing cache-line tuning sees no RelationCache entries and falls back
// to its own defaults, which is what a failure would have made it do anyway.
//
// The alternative is what YY-Thunks does: read CPUID leaves 0, 1, 0x0B and
// 0x1F, recover the APIC IDs and merge logical processors into real cores.
// That is several hundred lines of inline assembly, x86 only, and worth having
// if something here ever needs the topology rather than the count. Not before.
//
// Note this only ever runs on 32-bit. The oldest x64 Windows is XP x64, which
// is Server 2003 SP1 underneath and has this natively -- so on x64 the lookup
// below always succeeds and none of the rest executes.
BOOL WINAPI ww_GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer,
                                              PDWORD ReturnedLength) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, GetLogicalProcessorInformation)) {
    return system_fn(Buffer, ReturnedLength);
  }

  if (ReturnedLength == nullptr) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  SYSTEM_INFO info = {};
  ::GetSystemInfo(&info);

  DWORD_PTR active_mask = info.dwActiveProcessorMask;
  DWORD processors      = CountSetBits(active_mask);
  if (processors == 0) {
    // An empty mask, which should not happen on a system that is running
    // code. Report one processor rather than an empty buffer, so the required
    // length is never zero -- a caller measuring with the two-step call would
    // otherwise read zero as "nothing to return".
    active_mask = 1;
    processors  = 1;
  }

  // One entry per processor, plus one NUMA node and one package covering all
  // of them. Anything old enough to lack this function is a single socket with
  // no NUMA, so both of those describe the whole machine in one entry.
  const DWORD entries  = processors + 2;
  const DWORD required = entries * static_cast<DWORD>(sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));

  if (*ReturnedLength < required) {
    // The documented two-step protocol: report how much is needed and fail.
    // A caller passing zero to measure lands here, which is why the buffer is
    // neither touched nor checked until after this.
    *ReturnedLength = required;
    ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }

  if (Buffer == nullptr) {
    // Large enough by its own account, but nowhere to put it.
    ::SetLastError(ERROR_NOACCESS);
    return FALSE;
  }

  ::ZeroMemory(Buffer, required);

  DWORD written = 0;
  for (DWORD bit = 0; bit != sizeof(DWORD_PTR) * 8; ++bit) {
    const DWORD_PTR this_bit = static_cast<DWORD_PTR>(1) << bit;
    if ((active_mask & this_bit) == 0) {
      continue;
    }

    Buffer[written].ProcessorMask = this_bit;
    Buffer[written].Relationship  = RelationProcessorCore;
    // Flags is 1 when a core has more than one logical processor. Zero is the
    // approximation: without CPUID there is no way to know, and claiming no
    // SMT understates how many logical processors share a core rather than
    // overstating how many cores there are.
    Buffer[written].ProcessorCore.Flags = 0;
    ++written;
  }

  Buffer[written].ProcessorMask       = active_mask;
  Buffer[written].Relationship        = RelationNumaNode;
  Buffer[written].NumaNode.NodeNumber = 0;
  ++written;

  Buffer[written].ProcessorMask = active_mask;
  Buffer[written].Relationship  = RelationProcessorPackage;
  ++written;

  // No RelationCache entries, deliberately. Cache sizes and sharing cannot be
  // discovered without CPUID, and inventing them would be worse than leaving
  // them out: a caller cannot tell a guess from a measurement.

  *ReturnedLength = required;
  return TRUE;
}

} // extern "C"
