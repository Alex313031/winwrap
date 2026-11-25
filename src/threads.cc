// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Thread functions. Fibers live in fibers.cc; everything else thread-shaped
// belongs here.

#include <intrin.h> // __cpuid

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // The processor's nominal clock rate in MHz, as the HAL measured it at boot
  // and left in the registry. If it cannot be read, 1 GHz: the right order of
  // magnitude, and it keeps a cycle count proportional to time, which is the
  // property that matters. Whichever it is gets cached, so a registry that
  // will not answer is asked once.
  DWORD ProcessorMegahertz() {
    static std::atomic<DWORD> cached{0};
    DWORD megahertz = cached.load(std::memory_order_relaxed);
    if (megahertz != 0) {
      return megahertz;
    }
    HKEY processor = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                        KEY_QUERY_VALUE, &processor) == ERROR_SUCCESS) {
      DWORD size = sizeof(megahertz);
      DWORD type = 0;
      if (::RegQueryValueExW(processor, L"~MHz", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&megahertz), &size) != ERROR_SUCCESS ||
          type != REG_DWORD) {
        megahertz = 0;
      }
      ::RegCloseKey(processor);
    }
    if (megahertz == 0) {
      megahertz = 1000;
    }
    cached.store(megahertz, std::memory_order_relaxed);
    return megahertz;
  }

  // THREAD_BASIC_INFORMATION, which NtQueryInformationThread fills for class
  // 0. winternl.h declares the function and CLIENT_ID but not this. Not named
  // ThreadBasicInformation, which is what the class itself is called in
  // MinGW's winternl.h, where it is an enumerator.
  struct BasicThreadInfo {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
  };
  constexpr ULONG kThreadBasicInformationClass = 0;

  using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  using RtlNtStatusToDosErrorFn    = ULONG(NTAPI*)(NTSTATUS);

  ULONG64 AsTicks(const FILETIME& time) {
    return (static_cast<ULONG64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  }

} // namespace

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
DWORD WINAPI ww_GetCurrentProcessorNumber(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetCurrentProcessorNumber)) {
    return system_fn();
  }
  int registers[4] = {};
  __cpuid(registers, 1);
  // registers[1] is EBX; bits 31..24 hold the initial APIC ID.
  return (static_cast<DWORD>(registers[1]) >> 24) & 0xFF;
}

// How many processor cycles a thread has consumed. kernel32, Vista.
//
// Vista's scheduler reads the time stamp counter at every context switch and
// charges the difference to the outgoing thread. XP's does not: it charges
// whichever thread is running when the clock interrupt fires one whole tick,
// and that tally -- GetThreadTimes -- is the only per-thread accounting there
// is. So this is that time, kernel and user together as the real count is,
// converted to cycles at the processor's nominal rate: 100 ns units times MHz
// over ten.
//
// The conversion is there because of what callers do with the result. V8, the
// reason node imports this, divides by a time stamp counter frequency it has
// measured to get seconds back; a figure left in 100 ns units -- which is
// what YY-Thunks returns -- would come out hundreds of times too small. The
// nominal rate and V8's measured one agree to within a fraction of a percent
// on any processor with an invariant counter, and V8 uses this path on no
// other.
//
// What cannot be recovered is the resolution. The value advances in clock
// ticks, 10 or 15.6 ms each, and a thread that always yields before the tick
// can run indefinitely while appearing to consume nothing. INCOMPLETE_FUNCS.md
// has the entry.
BOOL WINAPI ww_QueryThreadCycleTime(HANDLE ThreadHandle, PULONG64 CycleTime) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, QueryThreadCycleTime)) {
    return system_fn(ThreadHandle, CycleTime);
  }
  if (!CycleTime) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  FILETIME creation = {}, exit = {}, kernel = {}, user = {};
  if (!::GetThreadTimes(ThreadHandle, &creation, &exit, &kernel, &user)) {
    return FALSE;
  }
  *CycleTime = (AsTicks(kernel) + AsTicks(user)) * ProcessorMegahertz() / 10;
  return TRUE;
}

// The ID of the thread a handle refers to. kernel32, Server 2003 -- so absent
// from 32-bit XP alone, 64-bit XP being Server 2003 underneath.
//
// kernel32 gained the function; the kernel always had the answer. A thread's
// basic information carries its CLIENT_ID, the process and thread IDs
// together, and NtQueryInformationThread has returned it since NT 3.1. That
// is what the real GetThreadId calls, so this is the same function a layer
// down rather than an approximation of it.
//
// One difference, which is the handle's access rather than the result: from
// Vista on THREAD_QUERY_LIMITED_INFORMATION is enough, and XP has no such
// right, so a handle has to carry THREAD_QUERY_INFORMATION. Server 2003's own
// GetThreadId asked the same.
DWORD WINAPI ww_GetThreadId(HANDLE Thread) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetThreadId)) {
    return system_fn(Thread);
  }
  const auto query = WW_TRYFUNC_AS(NtQueryInformationThreadFn, ::winwrap::Module::hNtDll,
                                   "NtQueryInformationThread");
  if (!query) {
    ::SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
  }

  BasicThreadInfo information = {};
  const NTSTATUS status =
      query(Thread, kThreadBasicInformationClass, &information, sizeof(information), nullptr);
  if (status < 0) {
    // Zero is the failure value and never a real thread ID. The error is the
    // kernel's own, translated the way kernel32 would have.
    const auto to_dos_error = WW_TRYFUNC_AS(RtlNtStatusToDosErrorFn, ::winwrap::Module::hNtDll,
                                            "RtlNtStatusToDosError");
    ::SetLastError(to_dos_error ? to_dos_error(status) : ERROR_INVALID_HANDLE);
    return 0;
  }
  // A thread ID is a 32-bit value carried in a pointer-sized field.
  return static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(information.ClientId.UniqueThread));
}

} // extern "C"
