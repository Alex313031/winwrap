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

#include "framework.h"
#include "sys.h"
#include "winwrap_version.h"

extern "C" {

// --- Tick count ------------------------------------------------------------
// GetTickCount64: Vista+.
ULONGLONG WINAPI ww_GetTickCount64(void);

// --- Slim reader/writer locks ----------------------------------------------
// The whole SRWLOCK family is Vista+, so XP needs all of it; Vista has
// everything except the two TryAcquire variants.
VOID WINAPI ww_InitializeSRWLock(PSRWLOCK SRWLock);
VOID WINAPI ww_AcquireSRWLockExclusive(PSRWLOCK SRWLock);
VOID WINAPI ww_ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
VOID WINAPI ww_AcquireSRWLockShared(PSRWLOCK SRWLock);
VOID WINAPI ww_ReleaseSRWLockShared(PSRWLOCK SRWLock);
BOOLEAN WINAPI ww_TryAcquireSRWLockExclusive(PSRWLOCK SRWLock);
BOOLEAN WINAPI ww_TryAcquireSRWLockShared(PSRWLOCK SRWLock);

// --- File information by handle --------------------------------------------
// Vista+ (ReOpenFile is Server 2003+). The information class parameter is
// typed int rather than FILE_INFO_BY_HANDLE_CLASS because that enum is gated
// on NTDDI_LONGHORN and so does not exist here; the ABI is identical.
BOOL WINAPI ww_GetFileInformationByHandleEx(HANDLE hFile,
                                            int FileInformationClass,
                                            LPVOID lpFileInformation,
                                            DWORD dwBufferSize);
BOOL WINAPI ww_SetFileInformationByHandle(HANDLE hFile,
                                          int FileInformationClass,
                                          LPVOID lpFileInformation,
                                          DWORD dwBufferSize);
DWORD WINAPI ww_GetFinalPathNameByHandleW(HANDLE hFile,
                                          LPWSTR lpszFilePath,
                                          DWORD cchFilePath,
                                          DWORD dwFlags);
HANDLE WINAPI ww_ReOpenFile(HANDLE hOriginalFile,
                            DWORD dwDesiredAccess,
                            DWORD dwShareMode,
                            DWORD dwFlagsAndAttributes);

// --- Directories -----------------------------------------------------------
// CreateSymbolicLinkW is Vista+ and not emulable: XP has no symbolic links at
// all, so it always fails with ERROR_NOT_SUPPORTED. Note the BOOLEAN return,
// unlike its neighbours. NeedCurrentDirectoryForExePathW is Server 2003+ and
// is implemented in full; its rule lives entirely in user space.
BOOLEAN WINAPI ww_CreateSymbolicLinkW(LPCWSTR lpSymlinkFileName,
                                      LPCWSTR lpTargetFileName,
                                      DWORD dwFlags);
BOOL WINAPI ww_NeedCurrentDirectoryForExePathW(LPCWSTR ExeName);

// --- Named pipes -----------------------------------------------------------
// Vista+. Answer only where the kernel can be made to say; see pipes.cc for
// why no inference is attempted.
BOOL WINAPI ww_GetNamedPipeClientProcessId(HANDLE Pipe, PULONG ClientProcessId);
BOOL WINAPI ww_GetNamedPipeServerProcessId(HANDLE Pipe, PULONG ServerProcessId);

// --- Threads ---------------------------------------------------------------
// Server 2003+. Derived from the CPUID initial APIC ID; see threads.cc.
DWORD WINAPI ww_GetCurrentProcessorNumber(void);

// --- Process-wide state ----------------------------------------------------
// Both Vista+. GetErrorMode reads what the kernel already tracks;
// SetProcessDPIAware reports success because XP has no DPI virtualisation to
// opt out of. See process.cc.
UINT WINAPI ww_GetErrorMode(void);
BOOL WINAPI ww_SetProcessDPIAware(void);

// --- Registry --------------------------------------------------------------
// Vista+. RegQueryValueEx plus a type filter, guaranteed string termination
// and environment expansion. See registry.cc.
LSTATUS APIENTRY ww_RegGetValueW(HKEY hkey,
                                 LPCWSTR lpSubKey,
                                 LPCWSTR lpValue,
                                 DWORD dwFlags,
                                 LPDWORD pdwType,
                                 PVOID pvData,
                                 LPDWORD pcbData);

// --- Miscellaneous ---------------------------------------------------------
// RegisterApplicationRestart is Vista+ and always fails: XP has no Restart
// Manager to register with.
HRESULT WINAPI ww_RegisterApplicationRestart(PCWSTR pwzCommandline, DWORD dwFlags);
HRESULT WINAPI ww_UnregisterApplicationRestart(void);

// --- Fibers ----------------------------------------------------------------
// Vista+. XP has fibers, just not these two on top of them.
LPVOID WINAPI ww_ConvertThreadToFiberEx(LPVOID lpParameter, DWORD dwFlags);
LPVOID WINAPI ww_CreateFiberEx(SIZE_T dwStackCommitSize,
                               SIZE_T dwStackReserveSize,
                               DWORD dwFlags,
                               LPFIBER_START_ROUTINE lpStartAddress,
                               LPVOID lpParameter);
BOOL WINAPI ww_IsThreadAFiber(void);

// --- Condition variables ---------------------------------------------------
// Vista+. CONDITION_VARIABLE and the lockmode flag are not version-gated --
// only the functions are -- so only these declarations are needed.
VOID WINAPI ww_InitializeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
BOOL WINAPI ww_SleepConditionVariableCS(PCONDITION_VARIABLE ConditionVariable,
                                        PCRITICAL_SECTION CriticalSection,
                                        DWORD dwMilliseconds);
BOOL WINAPI ww_SleepConditionVariableSRW(PCONDITION_VARIABLE ConditionVariable,
                                         PSRWLOCK SRWLock,
                                         DWORD dwMilliseconds,
                                         ULONG Flags);
VOID WINAPI ww_WakeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
VOID WINAPI ww_WakeAllConditionVariable(PCONDITION_VARIABLE ConditionVariable);

// --- One-time initialisation -----------------------------------------------
// InitOnceExecuteOnce: Vista+. libuv's uv_once() is built directly on this.
BOOL WINAPI ww_InitOnceExecuteOnce(PINIT_ONCE InitOnce,
                                   PINIT_ONCE_FN InitFn,
                                   PVOID Parameter,
                                   LPVOID* Context);

// --- Locale and string -----------------------------------------------------
// Both Vista+. LOCALE_NAME_MAX_LENGTH is gated below 0x0600.
#if !defined(LOCALE_NAME_MAX_LENGTH)
 #define LOCALE_NAME_MAX_LENGTH 85
#endif

int WINAPI ww_CompareStringOrdinal(LPCWCH lpString1,
                                   int cchCount1,
                                   LPCWCH lpString2,
                                   int cchCount2,
                                   BOOL bIgnoreCase);
int WINAPI ww_GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType, LPWSTR lpLCData, int cchData);

// --- I/O completion ports --------------------------------------------------
// Both Vista+. OVERLAPPED_ENTRY needs no declaration here: minwinbase.h
// defines it next to OVERLAPPED with no version gate at all, so it is present
// even at NTDDI_WINXPSP3. Only the skip flags are gated.
#if !defined(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
 #define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
 #define FILE_SKIP_SET_EVENT_ON_HANDLE        0x2
#endif

BOOL WINAPI ww_GetQueuedCompletionStatusEx(HANDLE CompletionPort,
                                           LPOVERLAPPED_ENTRY lpCompletionPortEntries,
                                           ULONG ulCount,
                                           PULONG ulNumEntriesRemoved,
                                           DWORD dwMilliseconds,
                                           BOOL fAlertable);
BOOL WINAPI ww_SetFileCompletionNotificationModes(HANDLE FileHandle, UCHAR Flags);

// --- I/O cancellation ------------------------------------------------------
// Both Vista+, and both exist to cancel I/O issued by another thread, which XP
// cannot do at all. CancelIoEx is best effort through CancelIo;
// CancelSynchronousIo always fails. See cancel_io.cc.
BOOL WINAPI ww_CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
BOOL WINAPI ww_CancelSynchronousIo(HANDLE hThread);

// --- Critical sections -----------------------------------------------------
// InitializeCriticalSectionEx: Vista+. The SDK hides the flag below 0x0600.
#if !defined(CRITICAL_SECTION_NO_DEBUG_INFO)
 #define CRITICAL_SECTION_NO_DEBUG_INFO 0x01000000
#endif

BOOL WINAPI ww_InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCriticalSection,
                                           DWORD dwSpinCount,
                                           DWORD Flags);

// --- Fiber-local storage ---------------------------------------------------
// kernel32, Server 2003 and Vista. The SDK hides the type and the sentinel
// below _WIN32_WINNT 0x0600, which winwrap compiles under, so declare them.
#if !defined(FLS_OUT_OF_INDEXES)
 #define FLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
typedef VOID(WINAPI* PFLS_CALLBACK_FUNCTION)(PVOID lpFlsData);
#endif

DWORD WINAPI ww_FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback);
PVOID WINAPI ww_FlsGetValue(DWORD dwFlsIndex);
BOOL WINAPI ww_FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData);
BOOL WINAPI ww_FlsFree(DWORD dwFlsIndex);

// --- Explicit Application User Model IDs -----------------------------------
// shell32, Windows 7+, and the one part of winwrap's surface that is reached
// through a delay import rather than a static one -- so the symptom is a crash
// on first call rather than a failure to load. Electron calls both
// unconditionally from shell/common/application_info_win.cc.
//
// These are declared here rather than taken from shobjidl.h because winwrap's
// own sources compile at _WIN32_WINNT=0x0501, where the SDK hides them.
HRESULT WINAPI ww_SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID);
HRESULT WINAPI ww_GetCurrentProcessExplicitAppUserModelID(PWSTR* AppID);

} // extern "C"

#endif // WINWRAP_WINWRAP_H_
