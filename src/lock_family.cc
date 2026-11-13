// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lock_family.h"

#include <atomic>

#include "dispatch.h"

namespace winwrap {
  namespace {

    // Three states, as elsewhere: null is unresolved, kAbsent means this Windows
    // cannot supply the whole group, anything else points at g_family.
    std::atomic<void*> g_state{nullptr};
    LockFamily g_family;

    // Fills g_family and returns whether every member was found. Writing the
    // struct before publishing the pointer is what makes the relaxed load in
    // SystemLockFamily() safe to pair with: a reader either sees null and resolves
    // it again -- harmlessly, since the result is the same -- or sees the pointer,
    // and the release store below orders the struct's contents ahead of it.
    bool Populate() {
      g_family.InitializeSRWLock       = WW_TRYFUNC(Module::hKernel32, InitializeSRWLock);
      g_family.AcquireSRWLockExclusive = WW_TRYFUNC(Module::hKernel32, AcquireSRWLockExclusive);
      g_family.AcquireSRWLockShared    = WW_TRYFUNC(Module::hKernel32, AcquireSRWLockShared);
      g_family.ReleaseSRWLockExclusive = WW_TRYFUNC(Module::hKernel32, ReleaseSRWLockExclusive);
      g_family.ReleaseSRWLockShared    = WW_TRYFUNC(Module::hKernel32, ReleaseSRWLockShared);
      // The two that Vista does not have, and the reason the line is at Windows 7.
      g_family.TryAcquireSRWLockExclusive =
          WW_TRYFUNC(Module::hKernel32, TryAcquireSRWLockExclusive);
      g_family.TryAcquireSRWLockShared = WW_TRYFUNC(Module::hKernel32, TryAcquireSRWLockShared);
      g_family.InitializeConditionVariable =
          WW_TRYFUNC(Module::hKernel32, InitializeConditionVariable);
      g_family.SleepConditionVariableCS  = WW_TRYFUNC(Module::hKernel32, SleepConditionVariableCS);
      g_family.SleepConditionVariableSRW = WW_TRYFUNC(Module::hKernel32, SleepConditionVariableSRW);
      g_family.WakeConditionVariable     = WW_TRYFUNC(Module::hKernel32, WakeConditionVariable);
      g_family.WakeAllConditionVariable  = WW_TRYFUNC(Module::hKernel32, WakeAllConditionVariable);

      return g_family.InitializeSRWLock && g_family.AcquireSRWLockExclusive &&
             g_family.AcquireSRWLockShared && g_family.ReleaseSRWLockExclusive &&
             g_family.ReleaseSRWLockShared && g_family.TryAcquireSRWLockExclusive &&
             g_family.TryAcquireSRWLockShared && g_family.InitializeConditionVariable &&
             g_family.SleepConditionVariableCS && g_family.SleepConditionVariableSRW &&
             g_family.WakeConditionVariable && g_family.WakeAllConditionVariable;
    }

  } // namespace

  const LockFamily* SystemLockFamily() noexcept {
    void* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr) {
      // Two threads can arrive here together. Both resolve the same addresses
      // and write the same values into g_family, so the duplicate work is
      // wasted rather than wrong -- and it cannot be observed half-done,
      // because a thread only acts on the struct once it has seen the release
      // store that follows filling it.
      const bool complete = Populate();
      state               = complete ? static_cast<void*>(&g_family) : kAbsent;
      g_state.store(state, std::memory_order_release);
    }
    return state == kAbsent ? nullptr : static_cast<const LockFamily*>(state);
  }

} // namespace winwrap
