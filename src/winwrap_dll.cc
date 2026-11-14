// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// winwrap.dll functions

// clang-format off
#include "winwrap.h"

#include <shlwapi.h>
// clang-format on

extern "C" {

[[maybe_unused]] static bool was_static_load = false;

// Main entry point when loading/unloading .DLL from address space of another process/thread.
// MUST have exact function signature BOOL WINAPI, and C linkage: the CRT's
// DllMainCRTStartup looks for the unmangled name, and would silently fall back
// to its own do-nothing DllMain if this one were mangled. Not an export: the loader
// reaches it through the entry point, not the export table.
// See https://www.transmissionzero.co.uk/computing/advanced-mingw-dll-topics/
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD dwReason, LPVOID lpvReserved) {
  switch (dwReason) {
    // Called on LoadLibrary or static import
    case DLL_PROCESS_ATTACH: {
      // Non-NULL means dll was statically loaded.
      if (lpvReserved != nullptr) {
        was_static_load = true;
      }
      return TRUE;
    }
    // Called when threads are run by parent process after DLL_PROCESS_ATTACH
    // (kept for completeness)
    case DLL_THREAD_ATTACH: {
      return TRUE;
    }
    // Called when FreeLibrary is called or program shutting down
    case DLL_PROCESS_DETACH: {
      // If lpvReserved is NULL, DLL load failed or FreeLibrary was called.
      if (lpvReserved == nullptr) {
        return TRUE;
      } else {
        // Process is terminating
        return TRUE;
      }
    }
    case DLL_THREAD_DETACH: {
      return TRUE; // Unused in winwrap.dll
    }
    default:
      // Should never reach here.
      return FALSE;
  }
}

// The shell's standard version query (what shlwapi's DllGetVersion protocol
// expects): STDAPI shape, i.e. C linkage + stdcall, exported undecorated via
// the .def. Fills whichever of DLLVERSIONINFO / DLLVERSIONINFO2 the caller
// sized. No exceptions cross this boundary - a bad size is a return code.
HRESULT CALLBACK DllGetVersion(DLLVERSIONINFO* pDvi) {
  if (pDvi == nullptr) {
    return E_POINTER;
  }

  switch (pDvi->cbSize) {
    case sizeof(DLLVERSIONINFO2): {
      // Caller passed DLLVERSIONINFO2 - fill extended fields
      DLLVERSIONINFO2* pDvi2 = reinterpret_cast<DLLVERSIONINFO2*>(pDvi);
      pDvi2->dwFlags         = 0;
      pDvi2->ullVersion =
          MAKEDLLVERULL(WINWRAP_MAJOR_VERSION, WINWRAP_MINOR_VERSION, WINWRAP_BUILD_VERSION, 0);
    }
      // No break, fall through for filling backward compatibile DLLVERSIONINFO
      [[fallthrough]];
    case sizeof(DLLVERSIONINFO): {
      // Fill base DLLVERSIONINFO fields
      pDvi->dwMajorVersion = WINWRAP_MAJOR_VERSION;
      pDvi->dwMinorVersion = WINWRAP_MINOR_VERSION;
      pDvi->dwBuildNumber  = WINWRAP_BUILD_VERSION;
      pDvi->dwPlatformID   = DLLVER_PLATFORM_NT;
      return S_OK;
    }
    default:
      break;
  }
  // cbSize is neither structure's - unsupported
  return E_INVALIDARG;
}

} // extern "C"
