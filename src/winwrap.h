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

// Deliberately not public/winwrap_public.h. This header is the shims, and the
// three functions in that one are winwrap's own API; nothing here calls them,
// so including it would only re-export a consumer header through a private one
// and give two paths to the same declarations.
#include "framework.h"
#include "winwrap_version.h"

// NET_LUID, for the two iphlpapi shims. After framework.h, whose <windows.h>
// it assumes; small, not version-gated, and present in MinGW's headers too.
#include <ifdef.h>

extern "C" {

// --- Tick count ------------------------------------------------------------
// GetTickCount64: Vista+.
ULONGLONG WINAPI ww_GetTickCount64(void);
// GetSystemTimePreciseAsFileTime: Windows 8+, so absent from 7 too. Before 8
// there is no interpolated clock, and this is GetSystemTimeAsFileTime: the
// right time at tick resolution. See tick_count.cc.
VOID WINAPI ww_GetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime);

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
// FindFirstFileExW exists everywhere; what XP and Vista lack is two arguments
// Windows 7 added, FindExInfoBasic and FIND_FIRST_EX_LARGE_FETCH. Retried
// without them when the system refuses. See directories.cc.
HANDLE WINAPI ww_FindFirstFileExW(LPCWSTR lpFileName,
                                  FINDEX_INFO_LEVELS fInfoLevelId,
                                  LPVOID lpFindFileData,
                                  FINDEX_SEARCH_OPS fSearchOp,
                                  LPVOID lpSearchFilter,
                                  DWORD dwAdditionalFlags);

// --- Named pipes -----------------------------------------------------------
// Vista+. Answer only where the kernel can be made to say; see pipes.cc for
// why no inference is attempted.
BOOL WINAPI ww_GetNamedPipeClientProcessId(HANDLE Pipe, PULONG ClientProcessId);
BOOL WINAPI ww_GetNamedPipeServerProcessId(HANDLE Pipe, PULONG ServerProcessId);

// --- Threads ---------------------------------------------------------------
// Server 2003+. Derived from the CPUID initial APIC ID; see threads.cc.
DWORD WINAPI ww_GetCurrentProcessorNumber(void);
// QueryThreadCycleTime: Vista+. GetThreadTimes scaled to cycles at the
// processor's nominal rate, so it has the clock tick's resolution rather than
// the counter's. See threads.cc.
BOOL WINAPI ww_QueryThreadCycleTime(HANDLE ThreadHandle, PULONG64 CycleTime);
// GetThreadId: Server 2003+, so missing from 32-bit XP only. The same
// NtQueryInformationThread the real one is built on.
DWORD WINAPI ww_GetThreadId(HANDLE Thread);

// --- Process-wide state ----------------------------------------------------
// Both Vista+. GetErrorMode reads what the kernel already tracks;
// SetProcessDPIAware reports success because XP has no DPI virtualisation to
// opt out of. See process.cc.
UINT WINAPI ww_GetErrorMode(void);
BOOL WINAPI ww_SetProcessDPIAware(void);
// FlushProcessWriteBuffers: Vista+. The interprocessor interrupt is provoked
// by revoking write access to a page; see process.cc.
VOID WINAPI ww_FlushProcessWriteBuffers(void);
// QueryFullProcessImageNameW: Vista+. psapi's GetProcessImageFileNameW, with
// the device translated to a drive letter unless PROCESS_NAME_NATIVE asks
// otherwise. The handle needs PROCESS_QUERY_INFORMATION. See process.cc.
BOOL WINAPI ww_QueryFullProcessImageNameW(HANDLE hProcess,
                                          DWORD dwFlags,
                                          LPWSTR lpExeName,
                                          PDWORD lpdwSize);

// --- Time zones ------------------------------------------------------------
// GetDynamicTimeZoneInformation: Vista+. GetTimeZoneInformation, with the
// registry key name searched for since XP does not record it. See
// timezone.cc.
DWORD WINAPI ww_GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION pTimeZoneInformation);

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
// All Vista+, and all three or none: they share the INIT_ONCE's state word,
// so winwrap's only ever meet each other. See init_once.cc.
// InitOnceExecuteOnce: libuv's uv_once() is built directly on this.
BOOL WINAPI ww_InitOnceExecuteOnce(PINIT_ONCE InitOnce,
                                   PINIT_ONCE_FN InitFn,
                                   PVOID Parameter,
                                   LPVOID* Context);
// InitOnceBeginInitialize / InitOnceComplete: the two-call form, synchronous
// or INIT_ONCE_ASYNC.
BOOL WINAPI ww_InitOnceBeginInitialize(LPINIT_ONCE lpInitOnce,
                                       DWORD dwFlags,
                                       PBOOL fPending,
                                       LPVOID* lpContext);
BOOL WINAPI ww_InitOnceComplete(LPINIT_ONCE lpInitOnce, DWORD dwFlags, LPVOID lpContext);

// --- Thread pool timers and waits ------------------------------------------
// All Vista+, and built on XP's timer queues rather than wrapped. TP_TIMER,
// TP_WAIT, TP_CALLBACK_INSTANCE and both callback types are plain typedefs in
// winnt.h with no version gate, so only the functions needed declaring.
//
// A handle from these is winwrap's own structure and not an OS object, so it
// must be closed by the copy of winwrap that made it, and the
// PTP_CALLBACK_INSTANCE a callback receives is not a real one. See
// thread_pool.cc, which is also where the Concurrency Runtime's part in all
// this is written down.
PTP_TIMER WINAPI ww_CreateThreadpoolTimer(PTP_TIMER_CALLBACK pfnti,
                                          PVOID pv,
                                          PTP_CALLBACK_ENVIRON pcbe);
VOID WINAPI ww_SetThreadpoolTimer(PTP_TIMER pti,
                                  PFILETIME pftDueTime,
                                  DWORD msPeriod,
                                  DWORD msWindowLength);
VOID WINAPI ww_WaitForThreadpoolTimerCallbacks(PTP_TIMER pti, BOOL fCancelPendingCallbacks);
VOID WINAPI ww_CloseThreadpoolTimer(PTP_TIMER pti);
PTP_WAIT WINAPI ww_CreateThreadpoolWait(PTP_WAIT_CALLBACK pfnwa,
                                        PVOID pv,
                                        PTP_CALLBACK_ENVIRON pcbe);
VOID WINAPI ww_SetThreadpoolWait(PTP_WAIT pwa, HANDLE h, PFILETIME pftTimeout);
VOID WINAPI ww_WaitForThreadpoolWaitCallbacks(PTP_WAIT pwa, BOOL fCancelPendingCallbacks);
VOID WINAPI ww_CloseThreadpoolWait(PTP_WAIT pwa);
VOID WINAPI ww_FreeLibraryWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HMODULE mod);

// --- System information ----------------------------------------------------
// GetLogicalProcessorInformation: XP SP3 and Server 2003 SP1. Its absence is
// what stops an otherwise-fine binary loading on XP SP2, and the CRT imports
// it whether or not anything ever calls it. See system.cc for what the fallback
// can and cannot know.
BOOL WINAPI ww_GetLogicalProcessorInformation(
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer,
    PDWORD ReturnedLength);

// --- Locale and string -----------------------------------------------------
// All Vista+ except where noted.
//
// LOCALE_ALLOW_NEUTRAL_NAMES is gated on WINVER >= _WIN32_WINNT_WIN7 in
// winnls.h, and winwrap compiles at WINVER=0x0501 everywhere except the GN
// build -- the Makefile, winwrap.gyp and the clang-cl syntax check all lower
// it -- so it has to be spelled out or three of the four builds fail to
// compile. Needed at all because ICU passes it to LCIDToLocaleName on every
// call; see locale.cc.
//
// LOCALE_NAME_MAX_LENGTH needs no such treatment and never did: it lives in
// winnt.h with no version gate at all, so it is always visible. The one other
// constant this family needs is LOCALE_NAME_SYSTEM_DEFAULT, which *is* gated
// (WINVER >= 0x0600) and is spelled out where it is used, in locale.cc.
#if !defined(LOCALE_ALLOW_NEUTRAL_NAMES)
 #define LOCALE_ALLOW_NEUTRAL_NAMES 0x08000000
#endif

int WINAPI ww_CompareStringOrdinal(LPCWCH lpString1,
                                   int cchCount1,
                                   LPCWCH lpString2,
                                   int cchCount2,
                                   BOOL bIgnoreCase);
int WINAPI ww_GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType, LPWSTR lpLCData, int cchData);
int WINAPI ww_LCMapStringEx(LPCWSTR lpLocaleName,
                            DWORD dwMapFlags,
                            LPCWSTR lpSrcStr,
                            int cchSrc,
                            LPWSTR lpDestStr,
                            int cchDest,
                            LPNLSVERSIONINFO lpVersionInformation,
                            LPVOID lpReserved,
                            LPARAM sortHandle);
int WINAPI ww_CompareStringEx(LPCWSTR lpLocaleName,
                              DWORD dwCmpFlags,
                              LPCWCH lpString1,
                              int cchCount1,
                              LPCWCH lpString2,
                              int cchCount2,
                              LPNLSVERSIONINFO lpVersionInformation,
                              LPVOID lpReserved,
                              LPARAM lParam);

// The two halves of the LCID/name mapping, and the resolver built on both.
// LCIDToLocaleName and LocaleNameToLCID are Vista; ResolveLocaleName is
// Windows 7, and is the odd one out of this file in that respect.
LCID WINAPI ww_LocaleNameToLCID(LPCWSTR lpName, DWORD dwFlags);
int WINAPI ww_LCIDToLocaleName(LCID Locale, LPWSTR lpName, int cchName, DWORD dwFlags);
int WINAPI ww_ResolveLocaleName(LPCWSTR lpNameToResolve, LPWSTR lpLocaleName, int cchLocaleName);

// The four formatters, all Vista and all the older call addressed by locale
// name. NUMBERFMTW, CURRENCYFMTW and SYSTEMTIME are all available at XP's
// level, so only the functions needed shimming.
int WINAPI ww_GetDateFormatEx(LPCWSTR lpLocaleName,
                              DWORD dwFlags,
                              const SYSTEMTIME* lpDate,
                              LPCWSTR lpFormat,
                              LPWSTR lpDateStr,
                              int cchDate,
                              LPCWSTR lpCalendar);
int WINAPI ww_GetTimeFormatEx(LPCWSTR lpLocaleName,
                              DWORD dwFlags,
                              const SYSTEMTIME* lpTime,
                              LPCWSTR lpFormat,
                              LPWSTR lpTimeStr,
                              int cchTime);
int WINAPI ww_GetNumberFormatEx(LPCWSTR lpLocaleName,
                                DWORD dwFlags,
                                LPCWSTR lpValue,
                                const NUMBERFMTW* lpFormat,
                                LPWSTR lpNumberStr,
                                int cchNumber);
int WINAPI ww_GetCurrencyFormatEx(LPCWSTR lpLocaleName,
                                  DWORD dwFlags,
                                  LPCWSTR lpValue,
                                  const CURRENCYFMTW* lpFormat,
                                  LPWSTR lpCurrencyStr,
                                  int cchCurrency);

// --- ETW manifest-based providers ------------------------------------------
// advapi32. Not emulable -- XP's ETW has no manifest-based half at all -- so
// all four of these always fail. See etw.cc for why, and for the one sibling,
// EventWrite, that is not here.
//
// EventWriteTransfer is Vista+; EventSetInformation did not arrive until
// Windows 8, so Vista and Windows 7 need the second and not the first.
//
// evntprov.h is gated in its entirety on WINVER >= 0x0600, so REGHANDLE comes
// from here. Repeating a typedef with the same type is legal, which matters
// because WINVER is not something winwrap controls in every build that
// consumes it -- so this is correct whether or not that gate happened to be
// open. The width is not cosmetic: REGHANDLE is 64 bits on both
// architectures, and that is what makes the x86 stdcall argument size 28
// rather than 24.
typedef ULONGLONG REGHANDLE;

// The two descriptor pointers are deliberately opaque. winwrap never reads
// either -- it forwards them or fails -- so transcribing EVENT_DESCRIPTOR and
// EVENT_DATA_DESCRIPTOR would be surface with no behaviour behind it, and a
// pointer is a pointer as far as the ABI is concerned.
ULONG WINAPI ww_EventWriteTransfer(REGHANDLE RegHandle,
                                   const void* EventDescriptor,
                                   LPCGUID ActivityId,
                                   LPCGUID RelatedActivityId,
                                   ULONG UserDataCount,
                                   const void* UserData);

// InformationClass is an int rather than EVENT_INFO_CLASS for the same reason
// the descriptors above are void*: winwrap never reads it. Declaring it as the
// enum would mean transcribing evntprov.h's definition here, and a second
// typedef of an enum is not the harmless repetition that REGHANDLE's is -- it
// would collide wherever the real header is visible. An enum is int-sized, so
// the ABI is identical either way, and the x86 stdcall size is 20: eight for
// REGHANDLE, four each for the rest.
ULONG WINAPI ww_EventSetInformation(REGHANDLE RegHandle,
                                    int InformationClass,
                                    void* EventInformation,
                                    ULONG InformationLength);

// EventRegister and EventUnregister: Vista+. Registration fails with
// ERROR_NOT_SUPPORTED and a zero handle, which the other three then refuse.
// EnableCallback is a PENABLECALLBACK, opaque here like the descriptors.
ULONG WINAPI ww_EventRegister(LPCGUID ProviderId,
                              PVOID EnableCallback,
                              PVOID CallbackContext,
                              REGHANDLE* RegHandle);
ULONG WINAPI ww_EventUnregister(REGHANDLE RegHandle);

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

// --- Events, semaphores and waitable timers ---------------------------------
// The Ex forms: Vista+. Flags are translated onto the XP functions, and the
// handle is then duplicated down to the access mask. See sync_objects.cc.
// The SDK may hide the flag names below 0x0600.
#if !defined(CREATE_EVENT_MANUAL_RESET)
 #define CREATE_EVENT_MANUAL_RESET 0x00000001
#endif
#if !defined(CREATE_EVENT_INITIAL_SET)
 #define CREATE_EVENT_INITIAL_SET 0x00000002
#endif
#if !defined(CREATE_WAITABLE_TIMER_MANUAL_RESET)
 #define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif

HANDLE WINAPI ww_CreateEventExW(LPSECURITY_ATTRIBUTES lpEventAttributes,
                                LPCWSTR lpName,
                                DWORD dwFlags,
                                DWORD dwDesiredAccess);
HANDLE WINAPI ww_CreateSemaphoreExW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes,
                                    LONG lInitialCount,
                                    LONG lMaximumCount,
                                    LPCWSTR lpName,
                                    DWORD dwFlags,
                                    DWORD dwDesiredAccess);
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is refused, as before Windows 10 1803.
HANDLE WINAPI ww_CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES lpTimerAttributes,
                                        LPCWSTR lpTimerName,
                                        DWORD dwFlags,
                                        DWORD dwDesiredAccess);

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

// --- Known folders ---------------------------------------------------------
// SHGetKnownFolderPath: shell32, Vista+. Maps the folder IDs XP has a CSIDL
// for onto SHGetFolderPathW; see shell32.cc, including for why this is
// the one shim that loads a library.
//
// `rfid` is REFKNOWNFOLDERID, spelled out because the typedef lives in
// shtypes.h, which nothing else here needs. A KNOWNFOLDERID is a GUID.
HRESULT WINAPI ww_SHGetKnownFolderPath(const GUID& rfid,
                                       DWORD dwFlags,
                                       HANDLE hToken,
                                       PWSTR* ppszPath);

// --- Symbol search path ----------------------------------------------------
// dbghelp 6.3, the one Vista ships; XP's dbghelp 5.1 has only the ANSI forms,
// which these convert to and from. V8 treats a failure of either as a failure
// to initialise symbols, so without them nothing is symbolised on XP. See
// dbghelp.cc, including for which dbghelp.dll they call into.
BOOL WINAPI ww_SymGetSearchPathW(HANDLE hProcess, PWSTR SearchPath, DWORD SearchPathLength);
BOOL WINAPI ww_SymSetSearchPathW(HANDLE hProcess, PCWSTR SearchPath);

// --- Network interfaces ----------------------------------------------------
// iphlpapi, Vista+. An index to a NET_LUID and a NET_LUID to its name, which
// together are libuv's uv_if_indextoname(). XP has no LUIDs; one is made from
// the interface's type and index. See net.cc.
//
// NET_LUID comes from ifdef.h, included at the top of this file. Naming the
// union by its tag instead would have spared the include, and did until this
// was built with MinGW, whose ifdef.h gives the same typedef a different tag.
DWORD WINAPI ww_ConvertInterfaceIndexToLuid(ULONG InterfaceIndex, NET_LUID* InterfaceLuid);
DWORD WINAPI ww_ConvertInterfaceLuidToNameW(const NET_LUID* InterfaceLuid,
                                            PWSTR InterfaceName,
                                            SIZE_T Length);

// --- Notification area icons -----------------------------------------------
// Shell_NotifyIconW exists everywhere; XP's rejects the NOTIFYICONDATAW size
// Vista introduced, which is the size a current SDK gives every caller.
// Shell_NotifyIconGetRect is Windows 7's and answers with the notification
// area's rectangle where the system cannot say. See shell32.cc.
//
// Both structures by tag: shellapi.h is not included here, and the second is
// not declared at all below _WIN32_WINNT 0x0601.
struct _NOTIFYICONDATAW;
struct _NOTIFYICONIDENTIFIER;
BOOL WINAPI ww_Shell_NotifyIconW(DWORD dwMessage, struct _NOTIFYICONDATAW* lpData);
HRESULT WINAPI ww_Shell_NotifyIconGetRect(const struct _NOTIFYICONIDENTIFIER* identifier,
                                          RECT* iconLocation);

// --- Task dialogs ----------------------------------------------------------
// comctl32 version 6, Vista. Where the system has none, a modal dialog built
// from user32's own controls: the texts, the icon, common and custom buttons,
// radio buttons, the checkbox and the callback. See task_dialog.cc, and
// INCOMPLETE_FUNCS.md for what it leaves out.
//
// By tag, as above: commctrl.h is not included here and does not declare the
// structure below NTDDI_VISTA.
struct _TASKDIALOGCONFIG;
HRESULT WINAPI ww_TaskDialogIndirect(const struct _TASKDIALOGCONFIG* pTaskConfig,
                                     int* pnButton,
                                     int* pnRadioButton,
                                     BOOL* pfVerificationFlagChecked);

#ifdef WINWRAP_DLL
HRESULT CALLBACK DllGetVersion(DLLVERSIONINFO* pDvi);
#endif // WINWRAP_DLL

} // extern "C"

#endif // WINWRAP_WINWRAP_H_
