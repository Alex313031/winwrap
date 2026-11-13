// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// InitializeCriticalSectionEx. kernel32, Vista.
//
// XP has InitializeCriticalSectionAndSpinCount, which is the same call without
// the flags argument, and the one flag that exists is reachable from XP
// anyway: the loader has always read CRITICAL_SECTION_NO_DEBUG_INFO out of the
// spin count, which is how the flag was expressed before there was a Flags
// parameter to put it in.
//
// Needed for its own sake, and needed to bootstrap: the MSVC static CRT calls
// this, so winwrap.dll cannot load on XP without it, no matter what else
// winwrap provides.

#include "winwrap.h"

extern "C" {

BOOL WINAPI winwrap_InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCriticalSection,
                                                DWORD dwSpinCount,
                                                DWORD Flags) {
  // The spin count field is 24 bits of count plus flag bits above it, so the
  // flag rides along rather than replacing anything.
  if (Flags & CRITICAL_SECTION_NO_DEBUG_INFO) {
    dwSpinCount |= CRITICAL_SECTION_NO_DEBUG_INFO;
  }
  return ::InitializeCriticalSectionAndSpinCount(lpCriticalSection, dwSpinCount);
}

} // extern "C"
