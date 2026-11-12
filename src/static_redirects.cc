// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Static-link redirection.
//
// A `__declspec(dllimport)` call site does not call the function directly; it
// loads a pointer from `__imp_<name>` and calls through it. The Windows SDK
// declares every Win32 function that way, so defining `__imp_<name>` ourselves
// is enough to capture the call -- the linker resolves the reference here and
// never extracts kernel32.lib's member for that symbol. No duplicate symbol,
// because archive members are only pulled to satisfy *still undefined*
// references, and no source change at any call site.
//
// kernel32.lib is never named in Chromium's GN: it arrives through the
// /DEFAULTLIB directives the SDK headers embed in every object file, and those
// are processed after libraries given on the command line. So an explicitly
// linked winwrap wins without having to reorder anything.
//
// The symbol name is decorated differently per architecture:
//
//   x64   __imp_GetTickCount64
//   x86   __imp__GetTickCount64@0     leading underscore, stdcall byte count
//
// The x86 form is not a legal C identifier, hence the asm label. The byte
// count is the total size of the arguments: 0 for a void function, 4 per
// pointer-sized parameter.
//
// === Why the SRWLOCK redirects are XP-only ===
//
// Do not link these on Vista. Vista exports the whole SRWLOCK family except
// TryAcquireSRWLock{Shared,Exclusive}, and redirecting only those two would
// point our CRITICAL_SECTION-based Try at a lock whose Ptr field the real
// Vista implementation owns -- reading its internals as if they were our
// pointer. The family has to be all ours or all the system's, and on Vista
// there is no way to make it all ours while leaving Acquire/Release native.
//
// That is why Vista is handled instead by teaching libuv that the Try variants
// can be absent (patches/node/06-node-xp-vista-support.patch returns UV_ENOSYS),
// and why winwrap's SRWLOCK set exists for XP, where nothing else provides it.

#include "winwrap.h"

#if defined(_M_IX86)
#define WINWRAP_IMP_SYMBOL(name, bytes) "__imp__" #name "@" #bytes
#else
#define WINWRAP_IMP_SYMBOL(name, bytes) "__imp_" #name
#endif

// Deliberately not `const`: at namespace scope a const object has internal
// linkage in C++, which would let the definition be discarded despite the asm
// label, and the reference it is meant to satisfy would fall through to
// kernel32 again.
#define WINWRAP_REDIRECT(name, bytes)                     \
  extern "C" decltype(&winwrap_##name) winwrap_imp_##name \
      __asm__(WINWRAP_IMP_SYMBOL(name, bytes)) = &winwrap_##name

// --- Available on Vista and later, missing on XP ---------------------------
WINWRAP_REDIRECT(GetTickCount64, 0);
WINWRAP_REDIRECT(InitOnceExecuteOnce, 16);

WINWRAP_REDIRECT(InitializeSRWLock, 4);
WINWRAP_REDIRECT(AcquireSRWLockExclusive, 4);
WINWRAP_REDIRECT(ReleaseSRWLockExclusive, 4);
WINWRAP_REDIRECT(AcquireSRWLockShared, 4);
WINWRAP_REDIRECT(ReleaseSRWLockShared, 4);

WINWRAP_REDIRECT(CompareStringOrdinal, 20);
WINWRAP_REDIRECT(GetLocaleInfoEx, 16);

WINWRAP_REDIRECT(GetQueuedCompletionStatusEx, 24);
WINWRAP_REDIRECT(SetFileCompletionNotificationModes, 8);

WINWRAP_REDIRECT(CancelIoEx, 8);
WINWRAP_REDIRECT(CancelSynchronousIo, 4);

WINWRAP_REDIRECT(InitializeCriticalSectionEx, 12);

WINWRAP_REDIRECT(FlsAlloc, 4);
WINWRAP_REDIRECT(FlsGetValue, 4);
WINWRAP_REDIRECT(FlsSetValue, 8);
WINWRAP_REDIRECT(FlsFree, 4);

// --- Missing on XP and Vista alike -----------------------------------------
WINWRAP_REDIRECT(TryAcquireSRWLockExclusive, 4);
WINWRAP_REDIRECT(TryAcquireSRWLockShared, 4);

// shell32 rather than kernel32, and delay-imported rather than statically
// imported, which changes when the failure appears but not how it is fixed:
// the reference is satisfied here, so shell32.lib is never consulted for it
// and no delay thunk is generated. See app_user_model_id.cc.
WINWRAP_REDIRECT(SetCurrentProcessExplicitAppUserModelID, 4);
WINWRAP_REDIRECT(GetCurrentProcessExplicitAppUserModelID, 4);
