// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Finding the system's own implementation, when it has one.
//
// Every shim here substitutes a function that newer Windows exports natively,
// and on that newer Windows the system's version is the better one -- it is
// what the rest of the OS uses, it has no emulation gaps, and it is the one
// that will keep working. So each shim asks for the real function first and
// only falls back to its own code when the answer is no. That is what lets a
// single binary with winwrap linked in run from XP to 11 without shipping two
// builds.
//
// The cost of asking has to be near zero, because some of these are called
// inside locks, per I/O completion, or per thread start. The resolved pointer
// is cached in a relaxed atomic, so a call on Windows 7 is:
//
//     mov  eax, [slot]      ; one load, already resolved
//     test eax, eax         ; cheap compare
//     jmp  eax              ; tail call into kernel32
//
// There is no guard variable, no lock, and no call into the CRT's
// thread-safe-statics machinery. Two threads racing to resolve the same slot
// both call GetProcAddress, which is idempotent and returns the same address,
// so the duplicate store is harmless -- which is why relaxed ordering is
// enough and why this needs no initialisation step to have run first. That
// matters more than it looks: winwrap cannot rely on a CRT initialiser,
// because the CRT itself calls some of these functions while starting up.
//
// A function the running Windows does not have resolves to a sentinel rather
// than being looked up again, so the XP path costs one compare per call and
// never re-enters the loader.

#ifndef WINWRAP_DISPATCH_H_
#define WINWRAP_DISPATCH_H_

#include <atomic>

#include "framework.h"

namespace winwrap {

  // The name of every DLL winwrap substitutes something from. Here rather than
  // beside the code that loads each one, so there is a single place to read
  // what the library reaches into.
  inline constexpr wchar_t kKernel32[] = L"kernel32.dll";
  inline constexpr wchar_t kUser32[]   = L"user32.dll";
  inline constexpr wchar_t kAdvapi32[] = L"advapi32.dll";
  inline constexpr wchar_t kShell32[]  = L"shell32.dll";
  inline constexpr wchar_t kNtDll[]    = L"ntdll.dll";

  // Which of those a function comes from. An enum rather than the name at each
  // call site, so the module handle is cached once per DLL instead of once per
  // function. Named h* for the handle each one stands for, leaving the k*
  // above for the strings; kCount keeps its k because it counts rather than
  // naming a module.
  enum class Module { hKernel32, hUser32, hAdvapi32, hShell32, hNtDll, kCount };

  // Stored in a slot when the running Windows has no such function. Any value
  // that cannot be a real code address will do; 1 is not writable, not
  // executable and not page-aligned.
  inline void* const kAbsent = reinterpret_cast<void*>(1u);

  // Looks the function up and writes the result -- the address, or kAbsent --
  // into the slot. Returns what it wrote. Only ever called on the first use of
  // a given slot, or again by a thread that raced with the first.
  void* ResolveSlot(std::atomic<void*>& slot, Module module, const char* name);

} // namespace winwrap

// The system's `name` from `module`, with the type given explicitly and the
// name as a string. WW_TRYFUNC below is this with the type inferred.
//
// For the ntdll entry points winwrap builds on -- NtQueryInformationFile,
// RtlGetNtVersionNumbers and the rest. Those have no ww_ shim to borrow a
// signature from, and no declaration here either, since winternl.h exposes
// only a fraction of them.
//
// Worth using in preference to a function-local static holding the result of
// a GetProcAddress lambda, which is the shape this replaced. That shape is
// not merely slower -- its guard goes through the CRT's thread-safe-statics
// machinery, and winwrap is called *by* the CRT while the CRT is still
// starting up. Resolving through a relaxed atomic owes the CRT nothing.
#define WW_TRYFUNC_AS(type, module, name)                                               \
  ([]() noexcept -> type {                                                              \
    static std::atomic<void*> slot{nullptr};                                            \
    void* resolved = slot.load(std::memory_order_relaxed);                              \
    if (resolved == nullptr) {                                                          \
      resolved = ::winwrap::ResolveSlot(slot, module, name);                            \
    }                                                                                   \
    return resolved == ::winwrap::kAbsent ? nullptr : reinterpret_cast<type>(resolved); \
  }())

// The system's `name` from `module`, typed from winwrap's own shim, or nullptr
// if this Windows does not export it.
//
// The type comes from the shim rather than the real declaration because the
// real one is not visible here: these sources compile with _WIN32_WINNT set to
// XP, which is the entire point of the library, and the SDK hides every
// function newer than that behind a version guard. So decltype(&RegGetValueW)
// does not compile -- there is no such declaration -- while
// decltype(&ww_RegGetValueW) is the same signature by construction, since a
// shim that did not match the function it substitutes would be wrong anyway.
// It also means a signature that drifts is a compile error here rather than a
// corrupt stack at runtime.
#define WW_TRYFUNC(module, name) WW_TRYFUNC_AS(decltype(&ww_##name), module, #name)

#endif // WINWRAP_DISPATCH_H_
