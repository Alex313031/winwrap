// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Functions with no better home. Process-wide state lives in process.cc.

#include "winwrap.h"

extern "C" {

// Register for automatic restart after a crash or an update. kernel32, Vista.
//
// Part of Application Recovery and Restart, which XP does not have in any
// form: there is no Restart Manager, and Windows Error Reporting on XP has no
// mechanism for relaunching an application it terminated. Nothing to call and
// nothing to approximate.
//
// E_NOTIMPL rather than a Win32-derived HRESULT, because that is the plainest
// statement of the situation and callers test only for failure. The sole
// caller in Chromium resolves this through GetProcAddress and simply does not
// register when it is absent (chrome/browser/chrome_browser_main_win.cc).
HRESULT WINAPI winwrap_RegisterApplicationRestart(PCWSTR pwzCommandline, DWORD dwFlags) {
  UNREFERENCED_PARAMETER(pwzCommandline);
  UNREFERENCED_PARAMETER(dwFlags);
  return E_NOTIMPL;
}

// The other half of the pair, and the same answer.
//
// S_OK would be defensible -- nothing can be registered, so the state this
// asks for already holds, and returning success would spare a caller a
// pointless error path. It is rejected because the two halves should not
// disagree about whether the facility exists: a caller told E_NOTIMPL by
// RegisterApplicationRestart learns the same thing here, instead of being told
// that an unregistration it never made has been undone.
//
// Nothing in Electron imports this. It is here because a library that
// implements one half of a pair and not the other is a worse library, and this
// half costs a line.
HRESULT WINAPI winwrap_UnregisterApplicationRestart(void) {
  return E_NOTIMPL;
}

} // extern "C"
