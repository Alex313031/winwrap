// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// SymGetSearchPathW and SymSetSearchPathW. dbghelp, Vista -- or rather,
// dbghelp 6.3, which is the version Vista ships. XP's is 5.1, and has both
// functions in their ANSI form only.
//
// So each is its ANSI sibling with a conversion on either side. The path is a
// list of directories, which the ANSI code page of the machine can represent
// or the directories could not have been created through the ANSI API; a
// character it cannot represent becomes the default character and that one
// entry fails to match, which is the worst that happens.
//
// What makes these worth having is the caller. V8's symbol initialisation, in
// base/debug/stack_trace_win.cc, reads the search path and writes it back with
// the executable's directory appended, and treats a failure of either as a
// failure to initialise symbols at all -- so on XP every native stack trace
// went unsymbolised, or, where dbghelp is delay-loaded, faulted in the
// delay-load helper on the first of the two.
//
// Which dbghelp matters, and it is not winwrap's to choose. A symbol session
// belongs to the module instance SymInitialize was called in: a program that
// ships its own dbghelp.dll beside the executable and one that uses the
// system's have sessions in different DLLs, and a call made into the other
// one finds nothing. So this takes whichever dbghelp is already in the
// process, which by the time a search path is being asked about is the one
// the caller initialised. Only if there is none does it load one, by bare
// name, so that the search goes exactly as it would have for the delay-load
// thunk this stands in for. That is why it does not use LoadSystemLibrary,
// which would insist on System32's.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  using winwrap::kAbsent;

  using SymGetSearchPathAFn = BOOL(WINAPI*)(HANDLE, PSTR, DWORD);
  using SymSetSearchPathAFn = BOOL(WINAPI*)(HANDLE, PCSTR);

  std::atomic<void*> g_dbghelp{nullptr};
  std::atomic<void*> g_get_wide{nullptr};
  std::atomic<void*> g_set_wide{nullptr};
  std::atomic<void*> g_get_ansi{nullptr};
  std::atomic<void*> g_set_ansi{nullptr};

  HMODULE Dbghelp() {
    void* cached = g_dbghelp.load(std::memory_order_relaxed);
    if (cached != nullptr) {
      return cached == kAbsent ? nullptr : static_cast<HMODULE>(cached);
    }
    const DWORD entry_error = ::GetLastError();
    HMODULE module          = ::GetModuleHandleW(L"dbghelp.dll");
    if (!module) {
      module = ::LoadLibraryW(L"dbghelp.dll");
    }
    g_dbghelp.store(module ? static_cast<void*>(module) : kAbsent, std::memory_order_relaxed);
    ::SetLastError(entry_error);
    return module;
  }

  // A buffer from the process heap, freed on the way out. Not the CRT's
  // allocator: a symbol path is being asked for because something has gone
  // wrong, and the less this leans on at that moment the better.
  class HeapBuffer {
   public:
    explicit HeapBuffer(size_t bytes)
        : memory_(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, bytes)) {}
    ~HeapBuffer() {
      if (memory_) {
        ::HeapFree(::GetProcessHeap(), 0, memory_);
      }
    }
    HeapBuffer(const HeapBuffer&)            = delete;
    HeapBuffer& operator=(const HeapBuffer&) = delete;

    template <typename T>
    T* As() const {
      return static_cast<T*>(memory_);
    }
    explicit operator bool() const { return memory_ != nullptr; }

   private:
    void* memory_;
  };

} // namespace

extern "C" {

BOOL WINAPI ww_SymGetSearchPathW(HANDLE hProcess, PWSTR SearchPath, DWORD SearchPathLength) {
  const HMODULE dbghelp = Dbghelp();

  // Hand straight over where this dbghelp has the function itself.
  if (const auto system_fn = reinterpret_cast<decltype(&ww_SymGetSearchPathW)>(
          ::winwrap::ResolveFrom(g_get_wide, dbghelp, "SymGetSearchPathW"))) {
    return system_fn(hProcess, SearchPath, SearchPathLength);
  }
  const auto get_ansi = reinterpret_cast<SymGetSearchPathAFn>(
      ::winwrap::ResolveFrom(g_get_ansi, dbghelp, "SymGetSearchPath"));
  if (!get_ansi) {
    ::SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }
  if (!SearchPath || SearchPathLength == 0) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  // The length is in characters, and a wide character can take two bytes in a
  // double-byte code page, so the ANSI buffer has room for twice as many.
  // Asking for less would truncate a path that fits the caller's buffer.
  const DWORD ansi_length = SearchPathLength > 0x3FFFFFFF ? 0x7FFFFFFF : SearchPathLength * 2;
  HeapBuffer ansi(ansi_length);
  if (!ansi) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }
  if (!get_ansi(hProcess, ansi.As<char>(), ansi_length)) {
    return FALSE; // With dbghelp's own error: no such session, usually.
  }
  // The ANSI function is given no room to spare, so terminate it here rather
  // than trust that a path filling the buffer was terminated there.
  ansi.As<char>()[ansi_length - 1] = '\0';

  if (ansi.As<char>()[0] == '\0') {
    SearchPath[0] = L'\0';
    return TRUE;
  }
  const int written = ::MultiByteToWideChar(CP_ACP, 0, ansi.As<char>(), -1, SearchPath,
                                            static_cast<int>(SearchPathLength));
  if (written == 0) {
    // Too long for the caller's buffer, which is ERROR_INSUFFICIENT_BUFFER
    // already. Left terminated, so a caller that reads it anyway finds a
    // string -- a whole number of entries would be better than a cut one, but
    // an empty path is at least not a wrong one.
    SearchPath[0] = L'\0';
    return FALSE;
  }
  return TRUE;
}

BOOL WINAPI ww_SymSetSearchPathW(HANDLE hProcess, PCWSTR SearchPath) {
  const HMODULE dbghelp = Dbghelp();

  if (const auto system_fn = reinterpret_cast<decltype(&ww_SymSetSearchPathW)>(
          ::winwrap::ResolveFrom(g_set_wide, dbghelp, "SymSetSearchPathW"))) {
    return system_fn(hProcess, SearchPath);
  }
  const auto set_ansi = reinterpret_cast<SymSetSearchPathAFn>(
      ::winwrap::ResolveFrom(g_set_ansi, dbghelp, "SymSetSearchPath"));
  if (!set_ansi) {
    ::SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }
  // Null is meaningful: it resets the path to dbghelp's default.
  if (!SearchPath) {
    return set_ansi(hProcess, nullptr);
  }

  const int bytes =
      ::WideCharToMultiByte(CP_ACP, 0, SearchPath, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return FALSE; // With WideCharToMultiByte's error.
  }
  HeapBuffer ansi(static_cast<size_t>(bytes));
  if (!ansi) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }
  if (::WideCharToMultiByte(CP_ACP, 0, SearchPath, -1, ansi.As<char>(), bytes, nullptr,
                            nullptr) == 0) {
    return FALSE;
  }
  return set_ansi(hProcess, ansi.As<char>());
}

} // extern "C"
