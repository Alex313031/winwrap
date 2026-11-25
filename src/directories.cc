// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Directory-shaped odds and ends: CreateSymbolicLinkW,
// NeedCurrentDirectoryForExePathW and FindFirstFileExW.
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

// For ww_FindFirstFileExW, at the end of this file.
namespace {

  constexpr int kFindExInfoBasic               = 1; // FINDEX_INFO_LEVELS
  constexpr DWORD kFindFirstExLargeFetch       = 0x00000002;
  constexpr DWORD kFindFirstExOnDiskEntriesOnly = 0x00000004; // Windows 10

  // Set once the system has refused the Windows 7 arguments, and once it has
  // refused the Windows 10 flag. Kept apart so that Windows 7 and 8, which
  // refuse only the flag, go on getting the two arguments they do accept.
  std::atomic<bool> g_find_drop_win7_args{false};
  std::atomic<bool> g_find_drop_win10_flag{false};

} // namespace

extern "C" {

// BOOLEAN, not BOOL: this one returns a byte. Worth stating because every
// neighbouring function in winbase.h returns BOOL, and on x86 the difference
// is invisible until a caller tests the full register.
BOOLEAN WINAPI ww_CreateSymbolicLinkW(LPCWSTR lpSymlinkFileName,
                                      LPCWSTR lpTargetFileName,
                                      DWORD dwFlags) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CreateSymbolicLinkW)) {
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
BOOL WINAPI ww_NeedCurrentDirectoryForExePathW(LPCWSTR ExeName) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, NeedCurrentDirectoryForExePathW)) {
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

// === FindFirstFileExW. kernel32, and present on every Windows winwrap runs on ===
//
// The only redirect here for a function the system already has. What XP and
// Vista lack is two of its arguments, both added in Windows 7:
//
//   FindExInfoBasic            do not look up the 8.3 short name
//   FIND_FIRST_EX_LARGE_FETCH  use a bigger buffer for the directory query
//
// Both are optimisations -- neither changes which files are found -- and an
// older kernel32 rejects either with ERROR_INVALID_PARAMETER. Code written
// since passes them without a second thought: the MSVC C++ runtime's
// std::filesystem directory iterator asks for FindExInfoBasic and has no
// fallback, so on XP every recursive_directory_iterator and every
// std::filesystem::remove_all fails with "The parameter is incorrect". Node's
// fs.rmSync() is remove_all, which is how this was found. Chromium's
// base::FileEnumerator passes both.
//
// So the call is made as written, and if it is refused for that reason, made
// again without the two. That the system refused once is remembered, so an
// old Windows pays for the discovery a single time rather than on every
// directory. Asking first and falling back, instead of testing the version,
// means there is no table here of which Windows accepts what.
//
// Windows 10 added a third of the same kind, FIND_FIRST_EX_ON_DISK_ENTRIES_ONLY,
// which skips placeholder files that no older Windows has. It is taken off
// first and on its own, and remembered on its own: Windows 7 and 8 refuse it
// but accept the other two, and one caller passing it must not cost every
// later search there its FindExInfoBasic.
//
// The one visible difference between the two info levels is that Basic
// leaves cAlternateFileName empty. A caller that asked for Basic is given
// that for the first entry, which is the one this function fills in. The
// entries after it come from FindNextFileW, which is not redirected, so after
// a retry they carry short names as the standard level does. Basic is a
// promise not to need the short name rather than a promise it is absent, so
// nothing is owed there; tracking find handles to blank a field nobody reads
// would be the larger risk.
//
// The real function is reached through its address, never by calling
// FindFirstFileExW: in a consumer that call is redirected here, which is the
// point, and inside winwrap it would be a call to itself.
HANDLE WINAPI ww_FindFirstFileExW(LPCWSTR lpFileName,
                                  FINDEX_INFO_LEVELS fInfoLevelId,
                                  LPVOID lpFindFileData,
                                  FINDEX_SEARCH_OPS fSearchOp,
                                  LPVOID lpSearchFilter,
                                  DWORD dwAdditionalFlags) {
  const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, FindFirstFileExW);
  if (!system_fn) {
    // Not a case that exists -- the function is in NT 4 -- but a null call is
    // not how to find out otherwise.
    ::SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return INVALID_HANDLE_VALUE;
  }

  const bool wants_basic = static_cast<int>(fInfoLevelId) == kFindExInfoBasic;
  const bool has_win7_args =
      wants_basic || (dwAdditionalFlags & kFindFirstExLargeFetch) != 0;
  const bool has_win10_flag = (dwAdditionalFlags & kFindFirstExOnDiskEntriesOnly) != 0;

  // What is already known to be refused comes off before the first attempt.
  // A Windows that refuses the Windows 7 arguments refuses the Windows 10
  // flag as well.
  const bool known_win7 = g_find_drop_win7_args.load(std::memory_order_relaxed);
  bool drop_win7_args   = has_win7_args && known_win7;
  bool drop_win10_flag =
      has_win10_flag && (known_win7 || g_find_drop_win10_flag.load(std::memory_order_relaxed));

  HANDLE found = INVALID_HANDLE_VALUE;
  for (;;) {
    DWORD flags = dwAdditionalFlags;
    if (drop_win10_flag) {
      flags &= ~kFindFirstExOnDiskEntriesOnly;
    }
    if (drop_win7_args) {
      flags &= ~kFindFirstExLargeFetch;
    }
    found = system_fn(lpFileName,
                      (drop_win7_args && wants_basic) ? FindExInfoStandard : fInfoLevelId,
                      lpFindFileData, fSearchOp, lpSearchFilter, flags);
    if (found != INVALID_HANDLE_VALUE) {
      break;
    }
    if (::GetLastError() != ERROR_INVALID_PARAMETER) {
      return found; // Not a refusal of the arguments; the caller's to see.
    }
    // Refused. Take off the newest thing still being passed and ask again:
    // the Windows 10 flag first, then the Windows 7 arguments.
    if (has_win10_flag && !drop_win10_flag) {
      drop_win10_flag = true;
    } else if (has_win7_args && !drop_win7_args) {
      drop_win7_args = true;
    } else {
      return found; // Nothing left to take off; something else is wrong.
    }
  }

  // It worked without them, so it was the new arguments and not something
  // else wrong with the call. A failure proves nothing either way, which is
  // why nothing is remembered on the paths that return above.
  if (drop_win10_flag) {
    g_find_drop_win10_flag.store(true, std::memory_order_relaxed);
  }
  if (drop_win7_args) {
    g_find_drop_win7_args.store(true, std::memory_order_relaxed);
    if (wants_basic && lpFindFileData) {
      static_cast<WIN32_FIND_DATAW*>(lpFindFileData)->cAlternateFileName[0] = L'\0';
    }
  }
  return found;
}

} // extern "C"
