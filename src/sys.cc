// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// System Information functions, including some of our own.

#include "sys.h"

#include <atomic>

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // _WIN32_WINNT packs the two numbers one byte each: 5.1 is 0x0501.
  inline constexpr uint32_t Pack(ULONG major, ULONG minor) {
    return ((major & 0xFF) << 8) | (minor & 0xFF);
  }

  /* Report the *true* NT kernel version. Two Windows mechanisms lie about
   it: since 8.1, GetVersionEx reports 6.2 (Windows 8) for apps without a
   compatibility manifest; and since XP, the "compatibility mode" tab can
   shim the version an app sees. Resolve dynamically (older targets lack
   the newer calls) and pick, per OS, the API that defeats every lie that
   applies there:
     RtlGetNtVersionNumbers: XP+, undocumented. Reads ntdll's baked-in
       build number, so it ignores BOTH the manifest lie and the compat
       tab. Preferred whenever present.
     RtlGetVersion: 2000+, documented. Defeats the manifest lie, but the
       compatibility tab can still shim it, so it is only reached on
       2000, which has neither a compat tab nor RtlGetNtVersionNumbers by
       name (only by ordinal), and where it is therefore exact. */
  uint32_t QueryRealNTVersion() {
    // RtlGetNtVersionNumbers first, per the comment above: the only one that
    // defeats both lies. Its build number carries flag bits in the top nibble,
    // which is why only major and minor are read out of it.
    typedef void(NTAPI * RtlGetNtVersionNumbers_t)(LPDWORD, LPDWORD, LPDWORD);
    const auto pversion_numbers = WW_TRYFUNC_AS(RtlGetNtVersionNumbers_t, ::winwrap::Module::hNtDll,
                                                "RtlGetNtVersionNumbers");
    if (pversion_numbers != nullptr) {
      DWORD major = 0;
      DWORD minor = 0;
      DWORD build = 0;
      pversion_numbers(&major, &minor, &build);
      if (major != 0) {
        return Pack(major, minor);
      }
    }

    // Then RtlGetVersion, reached by name only where the above is absent --
    // which is 2000, where it is exact.
    typedef LONG(NTAPI * RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    const auto pget_version =
        WW_TRYFUNC_AS(RtlGetVersion_t, ::winwrap::Module::hNtDll, "RtlGetVersion");
    if (pget_version != nullptr) {
      RTL_OSVERSIONINFOW info  = {};
      info.dwOSVersionInfoSize = sizeof(info);
      if (pget_version(&info) == STATUS_SUCCESS && info.dwMajorVersion != 0) {
        return Pack(info.dwMajorVersion, info.dwMinorVersion);
      }
    }

    // Deliberately no GetVersionEx fallback: both lies above apply to it, so a
    // wrong answer would be indistinguishable from a right one. 0 is the
    // honest result, and callers are told to read it as "older than anything".
    return 0;
  }

} // namespace

extern "C" {

uint32_t GetRealNTVersion() {
  // Cached because the answer cannot change while the process runs, and this
  // is cheap enough to be called from a branch rather than hoisted. Same
  // benign race as the dispatch slots: two threads compute the same value.
  static std::atomic<uint32_t> cached{0};
  uint32_t version = cached.load(std::memory_order_relaxed);
  if (version == 0) {
    // A query must not disturb the caller's error state, the way resolving a
    // dispatch slot must not.
    const DWORD entry_error = ::GetLastError();
    version                 = QueryRealNTVersion();
    cached.store(version, std::memory_order_relaxed);
    ::SetLastError(entry_error);
  }
  return version;
}

bool IsRunningOnWine(const char** outWineVer) {
  // Cleaner one-liner via a typedef than splitting the function-pointer
  // declaration and the assignment across two lines.
  typedef const char*(CDECL * WineGetVersion_t)(void);
  const WineGetVersion_t pwine_get_version =
      WW_TRYFUNC_AS(WineGetVersion_t, ::winwrap::Module::hNtDll, "wine_get_version");
  if (pwine_get_version == nullptr) {
    return false;
  }
  // Wine's implementation always returns a valid string in practice, but
  // handing a caller a null and calling it a success would not be, so guard
  // it: a true return has to mean outWineVer is usable.
  const char* wineVer = pwine_get_version();
  if (wineVer == nullptr) {
    return false;
  }
  // outWineVer is optional: callers that only care about the bool can pass
  // nullptr. Without this null-check we'd crash on the dereference below.
  if (outWineVer != nullptr) {
    *outWineVer = wineVer;
  }
  return true;
}

} // extern "C"
