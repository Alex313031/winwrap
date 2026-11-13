// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dispatch.h"

namespace winwrap {
  namespace {

    // One cached handle per DLL. Same benign-race reasoning as the function
    // slots: GetModuleHandleW returns the same handle every time, so two threads
    // arriving together simply write the same value twice.
    //
    // GetModuleHandleW rather than LoadLibraryW throughout, for two reasons. It
    // takes no reference, so there is nothing to release and no chance of pinning
    // a DLL that the process was going to unload; and it cannot pull in a library
    // that was not already there, so asking whether Windows has a function can
    // never change what is loaded. Every DLL named here is either already present
    // in any process that could call these functions, or genuinely absent -- in
    // which case the answer is "no such function", which is the right one.
    std::atomic<void*> g_modules[static_cast<size_t>(Module::kCount)];

    const wchar_t* ModuleName(Module module) {
      switch (module) {
        case Module::hKernel32:
          return kKernel32;
        case Module::hUser32:
          return kUser32;
        case Module::hAdvapi32:
          return kAdvapi32;
        case Module::hShell32:
          return kShell32;
        case Module::hNtDll:
          return kNtDll;
        case Module::kCount:
          break;
      }
      return nullptr;
    }

    HMODULE ModuleHandle(Module module) {
      const size_t index       = static_cast<size_t>(module);
      std::atomic<void*>& slot = g_modules[index];

      void* cached = slot.load(std::memory_order_relaxed);
      if (cached != nullptr) {
        return cached == kAbsent ? nullptr : static_cast<HMODULE>(cached);
      }

      const wchar_t* name = ModuleName(module);
      HMODULE handle      = name ? ::GetModuleHandleW(name) : nullptr;
      slot.store(handle ? static_cast<void*>(handle) : kAbsent, std::memory_order_relaxed);
      return handle;
    }

  } // namespace

  void* ResolveSlot(std::atomic<void*>& slot, Module module, const char* name) {
    // Looking a function up has to be invisible. A miss leaves
    // ERROR_PROC_NOT_FOUND behind, and the shim is about to run its own
    // implementation whose success would then be read against a last error it
    // never set -- a bug that only appears on the OS that lacks the function,
    // and only for callers that check GetLastError() after a success. Saving and
    // restoring the whole value is simpler than reasoning about which calls
    // below do and do not set it.
    const DWORD entry_error = ::GetLastError();

    HMODULE handle = ModuleHandle(module);
    // GetProcAddress takes narrow names whatever the project's character set, so
    // the name arrives as the stringised identifier and is used as-is.
    void* address = handle ? reinterpret_cast<void*>(::GetProcAddress(handle, name)) : nullptr;

    void* result = address ? address : kAbsent;
    slot.store(result, std::memory_order_relaxed);

    ::SetLastError(entry_error);
    return result;
  }

} // namespace winwrap
