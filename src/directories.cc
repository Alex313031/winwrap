// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Directory-shaped odds and ends: CreateSymbolicLinkW and
// NeedCurrentDirectoryForExePathW.
//
// === CreateSymbolicLinkW. kernel32, Vista ===
//
// Not a missing entry point over an existing capability -- XP has no symbolic
// links. NTFS gained them in Vista, along with the reparse tag, the privilege
// that guards them and the loader support that follows them. There is nothing
// underneath to call.
//
// So this fails, and says why. ERROR_NOT_SUPPORTED is the code Windows itself
// returns when the target filesystem cannot do this (a symlink onto FAT32, for
// instance), which puts the failure in the family callers already handle --
// libuv maps it to UV_ENOTSUP, so Node's fs.symlink() reports ENOTSUP rather
// than something that reads as a bug in the runtime.
//
// Deliberately not ERROR_INVALID_PARAMETER. libuv treats that specially: it
// assumes unprivileged symlink creation was refused, clears
// SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE and calls straight back in
// (deps/uv/src/win/fs.c, fs__symlink). It terminates, but it is a wasted round
// trip after a wrong diagnosis.
//
// === Why not quietly make a junction ===
//
// A directory junction is the nearest thing XP has, and it is tempting for the
// SYMBOLIC_LINK_FLAG_DIRECTORY case. It is not the same object: junctions are
// directory-only, resolve only absolute paths, and read back differently. A
// caller that asked for a symlink and silently received one of these would get
// different answers from readlink and different behaviour for a relative
// target.
//
// It would also be redundant where it matters. libuv already exposes junctions
// as their own request type, UV_FS_SYMLINK_JUNCTION, handled by
// fs__create_junction() without ever reaching this function -- and that is the
// path npm takes on Windows. The callers who want a junction can already ask
// for one; the callers who arrive here asked for something else.

#include <string.h> // wcschr

#include "dispatch.h"
#include "winwrap.h"

#if !defined(SYMBOLIC_LINK_FLAG_DIRECTORY)
 #define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif

extern "C" {

// BOOLEAN, not BOOL: this one returns a byte. Worth stating because every
// neighbouring function in winbase.h returns BOOL, and on x86 the difference
// is invisible until a caller tests the full register.
BOOLEAN WINAPI winwrap_CreateSymbolicLinkW(LPCWSTR lpSymlinkFileName,
                                           LPCWSTR lpTargetFileName,
                                           DWORD dwFlags) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WINWRAP_SYSTEM(::winwrap::Module::hKernel32, CreateSymbolicLinkW)) {
    return system_fn(lpSymlinkFileName, lpTargetFileName, dwFlags);
  }
  UNREFERENCED_PARAMETER(lpSymlinkFileName);
  UNREFERENCED_PARAMETER(lpTargetFileName);
  UNREFERENCED_PARAMETER(dwFlags);

  ::SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}

// Whether the current directory belongs in the search path used to find
// ExeName. kernel32, Server 2003.
//
// Unlike its neighbour above this is fully implementable on XP, because the
// rule is entirely in user space: a name that already carries a path is
// resolved relative to it, so the question does not arise; otherwise the
// current directory is used unless NoDefaultCurrentDirectoryInExePath exists
// in the environment. Nothing consults the kernel, so the answer here is the
// same answer a later Windows would give.
//
// What the answer does *not* do is change XP's own behaviour. XP's loader
// always searches the current directory, so a caller that asks and then calls
// CreateProcess with a bare name still gets the current directory searched.
// The function is advice for callers that do their own path search -- which
// is exactly the caller here: libuv's search_path() asks before deciding
// whether to look in the cwd first (deps/uv/src/win/process.c).
//
// Only a backslash counts as "has a path", matching the real implementation.
// A forward slash does not, even though much of Win32 accepts one.
BOOL WINAPI winwrap_NeedCurrentDirectoryForExePathW(LPCWSTR ExeName) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WINWRAP_SYSTEM(::winwrap::Module::hKernel32, NeedCurrentDirectoryForExePathW)) {
    return system_fn(ExeName);
  }
  if (ExeName != nullptr && ::wcschr(ExeName, L'\\') != nullptr) {
    return TRUE;
  }

  // Presence, not value: any value at all turns the behaviour off. The size
  // probe alone is not enough to test it, because a variable whose value is
  // the empty string can report a size of zero, which is also what a missing
  // variable reports. Only the error code separates the two.
  ::SetLastError(ERROR_SUCCESS);
  DWORD size   = ::GetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", nullptr, 0);
  bool present = size != 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
  return present ? FALSE : TRUE;
}

} // extern "C"
