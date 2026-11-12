// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINWRAP_WINWRAP_H_
#define WINWRAP_WINWRAP_H_

// winwrap provides implementations of Win32 functions that do not exist on the
// older Windows versions this fork targets, so a binary compiled against a
// current SDK still loads there. A statically imported symbol the target OS
// does not export is fatal at load, before any of our code runs, which is why
// per-call-site workarounds cannot solve it.
//
// Nothing here calls the function it replaces, and nothing here uses an API
// newer than Windows XP -- winwrap has to run on the oldest target itself.
//
// Each implementation is named winwrap_<Win32Name> and keeps the exact
// signature of the function it stands in for, so the redirect in
// static_redirects.cc is a plain assignment with no casting.

#include <windows.h>

#include "winwrap_version.h"

extern "C" {

// --- Tick count ------------------------------------------------------------
// GetTickCount64: Vista+.
ULONGLONG WINAPI winwrap_GetTickCount64(void);

// --- Slim reader/writer locks ----------------------------------------------
// The whole SRWLOCK family is Vista+, so XP needs all of it; Vista has
// everything except the two TryAcquire variants.
VOID WINAPI winwrap_InitializeSRWLock(PSRWLOCK SRWLock);
VOID WINAPI winwrap_AcquireSRWLockExclusive(PSRWLOCK SRWLock);
VOID WINAPI winwrap_ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
VOID WINAPI winwrap_AcquireSRWLockShared(PSRWLOCK SRWLock);
VOID WINAPI winwrap_ReleaseSRWLockShared(PSRWLOCK SRWLock);
BOOLEAN WINAPI winwrap_TryAcquireSRWLockExclusive(PSRWLOCK SRWLock);
BOOLEAN WINAPI winwrap_TryAcquireSRWLockShared(PSRWLOCK SRWLock);

// --- One-time initialisation -----------------------------------------------
// InitOnceExecuteOnce: Vista+. libuv's uv_once() is built directly on this.
BOOL WINAPI winwrap_InitOnceExecuteOnce(PINIT_ONCE InitOnce,
                                        PINIT_ONCE_FN InitFn,
                                        PVOID Parameter,
                                        LPVOID* Context);

// --- I/O completion ports --------------------------------------------------
// Both Vista+. OVERLAPPED_ENTRY needs no declaration here: minwinbase.h
// defines it next to OVERLAPPED with no version gate at all, so it is present
// even at NTDDI_WINXPSP3. Only the skip flags are gated.
#if !defined(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#define FILE_SKIP_SET_EVENT_ON_HANDLE 0x2
#endif

BOOL WINAPI winwrap_GetQueuedCompletionStatusEx(
    HANDLE CompletionPort,
    LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    ULONG ulCount,
    PULONG ulNumEntriesRemoved,
    DWORD dwMilliseconds,
    BOOL fAlertable);
BOOL WINAPI winwrap_SetFileCompletionNotificationModes(HANDLE FileHandle,
                                                       UCHAR Flags);

// --- I/O cancellation ------------------------------------------------------
// Both Vista+, and both exist to cancel I/O issued by another thread, which XP
// cannot do at all. CancelIoEx is best effort through CancelIo;
// CancelSynchronousIo always fails. See cancel_io.cc.
BOOL WINAPI winwrap_CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
BOOL WINAPI winwrap_CancelSynchronousIo(HANDLE hThread);

// --- Critical sections -----------------------------------------------------
// InitializeCriticalSectionEx: Vista+. The SDK hides the flag below 0x0600.
#if !defined(CRITICAL_SECTION_NO_DEBUG_INFO)
#define CRITICAL_SECTION_NO_DEBUG_INFO 0x01000000
#endif

BOOL WINAPI winwrap_InitializeCriticalSectionEx(
    LPCRITICAL_SECTION lpCriticalSection,
    DWORD dwSpinCount,
    DWORD Flags);

// --- Fiber-local storage ---------------------------------------------------
// kernel32, Server 2003 and Vista. The SDK hides the type and the sentinel
// below _WIN32_WINNT 0x0600, which winwrap compiles under, so declare them.
#if !defined(FLS_OUT_OF_INDEXES)
#define FLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
typedef VOID(WINAPI* PFLS_CALLBACK_FUNCTION)(PVOID lpFlsData);
#endif

DWORD WINAPI winwrap_FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback);
PVOID WINAPI winwrap_FlsGetValue(DWORD dwFlsIndex);
BOOL WINAPI winwrap_FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData);
BOOL WINAPI winwrap_FlsFree(DWORD dwFlsIndex);

// --- Explicit Application User Model IDs -----------------------------------
// shell32, Windows 7+, and the one part of winwrap's surface that is reached
// through a delay import rather than a static one -- so the symptom is a crash
// on first call rather than a failure to load. Electron calls both
// unconditionally from shell/common/application_info_win.cc.
//
// These are declared here rather than taken from shobjidl.h because winwrap's
// own sources compile at _WIN32_WINNT=0x0501, where the SDK hides them.
HRESULT WINAPI winwrap_SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID);
HRESULT WINAPI winwrap_GetCurrentProcessExplicitAppUserModelID(PWSTR* AppID);

}  // extern "C"

#endif  // WINWRAP_WINWRAP_H_
