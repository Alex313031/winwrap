// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Explicit Application User Model IDs. shell32, Windows 7 and later.
//
// An AppUserModelID identifies a process to the Windows 7 taskbar so that its
// windows group under a single button and a jump list can be attached to them.
// Vista's taskbar has neither grouping nor jump lists, so there is no state to
// keep and nothing to report. These are not partial implementations; they are
// the whole of the concept as it exists before Windows 7.
//
// Unlike the rest of winwrap, these two are *delay*-imported, so an absent
// export is not fatal at load. It surfaces on the first call, where Chromium's
// delay-load failure hook ends in NOTREACHED() -- an int3, reported by Windows
// as exception code 0x80000003 and nothing else. static_redirects.cc keeps
// the call from ever reaching shell32, and it works for a delay import for
// the same reason it works for a static one: the reference is satisfied
// here, so shell32.lib's member is never extracted and no delay thunk is
// generated for it. winwrap.dll.lib is a link input, which the toolchain lists
// ahead of `libs`, so it is reached before the explicit shell32.lib.

#include "winwrap.h"

extern "C" {

HRESULT WINAPI winwrap_SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID) {
  // Reject a null ID the way the real function does, rather than reporting
  // success for a call that could not have meant anything. There is no ID to
  // store here, so the argument is otherwise unused -- but a shim that answers
  // S_OK to everything teaches a caller that its input was accepted, and the
  // first place that matters is a caller being ported to this platform.
  if (!AppID) {
    return E_INVALIDARG;
  }

  // The documented constraints on a valid ID -- at most 128 characters, no
  // spaces -- are deliberately not enforced. Windows rejects those, so a
  // caller that trips them is already handling a failure on Win7 and later,
  // and accepting one here costs nothing because nothing is kept. Enforcing
  // them would only add a way for this to disagree with the real function in
  // the opposite, harder-to-notice direction.
  return S_OK;
}

HRESULT WINAPI winwrap_GetCurrentProcessExplicitAppUserModelID(PWSTR* AppID) {
  // E_FAIL is what Windows itself returns for a process that never called Set,
  // so every caller already handles it -- Electron's GetRawAppUserModelID()
  // takes that branch and derives an ID from the application name instead
  // (electron/shell/common/application_info_win.cc).
  //
  // Clear the out parameter regardless of what the caller passed in. That same
  // Electron function declares an uninitialised PWSTR and hands it to
  // CoTaskMemFree() unconditionally, after the branch where this call failed,
  // which is only safe if the pointer is null.
  if (AppID) {
    *AppID = nullptr;
  }
  return E_FAIL;
}

} // extern "C"
