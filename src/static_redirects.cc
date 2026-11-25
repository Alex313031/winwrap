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
// That only holds if winwrap is searched before whatever else can satisfy
// `__imp_<name>`. In Chromium's GN build it is, for free: kernel32.lib is
// never named there, it arrives through the /DEFAULTLIB directives the SDK
// headers embed in every object file, and those are processed after libraries
// given on the command line. So an explicitly linked winwrap wins without
// having to reorder anything.
//
// That is a fact about Chromium's GN build and not about linking in general,
// which is worth saying because it reads like the latter. A build that names
// kernel32.lib explicitly puts it on the command line too, and then order
// decides -- kernel32 resolves `__imp_<name>` first, the redirect table is
// never extracted because nothing is left undefined to pull it, and winwrap
// contributes nothing while appearing to be linked. GYP-era Chromium is
// exactly that case, and the consumer has to force the table in by naming
// winwrap_static_redirects below in /INCLUDE:. See winwrap.gyp for the per
// architecture spelling and for what was tried before that.
//
// Which table gets forced in depends on the build: this file in the GN one,
// forwarders_x86.asm or forwarders_x64.asm in the MSVC one, since cl.exe has
// no asm labels to name the decorated symbols with. They carry the same set
// of slots and the same anchor, so the consumer-side instruction is the same
// either way.
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
// === The SRWLOCK and CONDITION_VARIABLE entries are not independent ===
//
// Redirecting a slot here only decides where the call lands. For every other
// function that is the whole story, because the arguments mean the same thing
// to winwrap and to the system. For these twelve it is not: an SRWLOCK is one
// opaque pointer-sized word, and the two implementations keep different things
// in it, so half a lock from each is not a slower lock but memory corruption.
//
// The grouping that prevents that is at runtime, in lock_family.cc, not here.
// SystemLockFamily() resolves all twelve or none, which is why Vista -- which
// exports AcquireSRWLock*/ReleaseSRWLock* but not TryAcquireSRWLock*, and has
// no RtlTryAcquireSRWLock* underneath either -- keeps winwrap's whole family
// rather than a mixture. So these redirects are linked everywhere the rest
// are, and nothing about them is XP-only.

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
//
// `bytes` is not checked by anything here. decltype validates the type of the
// pointer, but the asm label is an opaque string to the compiler, so a wrong
// count builds cleanly and defines __imp__Name@wrong, which nothing
// references -- leaving the real slot to fall through to kernel32 and the
// binary to fail at load on XP. The MASM table is the stricter of the two:
// forwarders_x86.asm carries the same count on its `EXTERN _ww_Name@bytes`,
// where a wrong one cannot resolve and is an LNK2019 at build time. So when a
// count changes, regenerate with tools/gen_forwarders.py and build the MSVC
// side; that is what turns a silent miss into an error.

// The function under its own name as well as its import slot, for MinGW.
//
// An MSVC-style consumer reaches a system function through `__imp_<name>` and
// nothing else, because the SDK declares every one of them
// __declspec(dllimport); the slot above is all such a build needs. MinGW's
// headers are not so uniform. Some functions are declared without dllimport --
// the SRW lock family, for a start -- and a call to one of those references
// the function itself, `_Name@bytes`, expecting the linker to find a body. The
// import library has one, in the same member that defines the slot, so pulling
// it in for the name brings a second definition of the slot with it and the
// link fails with "multiple definition of `_imp__Name@bytes'".
//
// So the MinGW build defines the name too, as a one-instruction jump to the
// shim. Then whichever way a consumer spells the reference, it is settled
// here and the import library's member is never extracted. This is the same
// problem, and the same answer, as SHGetKnownFolderPath in shell32.cc -- there
// it is the SDK's own declaration that lacks dllimport, so that file handles
// it for the MSVC builds, and defers to this for MinGW.
//
// Assembly rather than a C++ forwarding function, because it has to work for
// every signature in the table without knowing any of them, and a jump does:
// the arguments are already where the shim expects them.
#if defined(__MINGW32__)
 #if defined(_M_IX86)
  #define WINWRAP_PLAIN_SYMBOL(name, bytes) "_" #name "@" #bytes
  #define WINWRAP_SHIM_SYMBOL(name, bytes) "_ww_" #name "@" #bytes
 #else
  #define WINWRAP_PLAIN_SYMBOL(name, bytes) #name
  #define WINWRAP_SHIM_SYMBOL(name, bytes) "ww_" #name
 #endif
 // .def/.scl/.type mark the symbol as an external function, which is what a
 // compiler would have emitted for one.
 #define WINWRAP_PLAIN_NAME(name, bytes)                                \
   __asm__(".text\n"                                                    \
           ".globl " WINWRAP_PLAIN_SYMBOL(name, bytes) "\n"             \
           ".def " WINWRAP_PLAIN_SYMBOL(name, bytes)                    \
           "; .scl 2; .type 32; .endef\n"                               \
           WINWRAP_PLAIN_SYMBOL(name, bytes) ":\n"                      \
           "\tjmp " WINWRAP_SHIM_SYMBOL(name, bytes) "\n")
#else
 #define WINWRAP_PLAIN_NAME(name, bytes) static_assert(true, "")
#endif

//
// The braced form of extern "C", and not `extern "C" T x = v;`, which means
// the same thing but which GCC warns about on every line -- "initialized and
// declared extern" -- with no option to turn it off. WINWRAP_PLAIN_NAME
// comes last so that it is what takes the semicolon the call site writes.
#define WINWRAP_REDIRECT(name, bytes)                                                \
  extern "C" {                                                                       \
  decltype(&ww_##name) winwrap_imp_##name __asm__(WINWRAP_IMP_SYMBOL(name, bytes)) = \
      &ww_##name;                                                                    \
  }                                                                                  \
  WINWRAP_PLAIN_NAME(name, bytes)

// The anchor a consumer names in /INCLUDE: to force this object into the link,
// for the builds described in winwrap.gyp where something else would otherwise
// satisfy every `__imp_<name>` first and leave this object unextracted.
//
// It exists so that contract is a symbol of winwrap's own rather than one of
// the shims. Naming a shim works just as well -- any symbol in this object
// pulls all of it -- but it couples every consumer to that particular function
// still being here, and a consumer should not have to care which functions
// winwrap happens to cover this release.
//
// Not `const`: at namespace scope that would give it internal linkage and
// there would be nothing for /INCLUDE: to find. Decorated the usual way, so
// x86 wants a leading underscore and x64 does not -- unlike the redirects
// above, which carry asm labels precisely to avoid being decorated.
extern "C" {
int winwrap_static_redirects = 0;
}

// --- Available on Vista and later, missing on XP ---------------------------
WINWRAP_REDIRECT(GetTickCount64, 0);
WINWRAP_REDIRECT(InitOnceExecuteOnce, 16);
WINWRAP_REDIRECT(InitOnceBeginInitialize, 16);
WINWRAP_REDIRECT(InitOnceComplete, 12);

WINWRAP_REDIRECT(GetFileInformationByHandleEx, 16);
WINWRAP_REDIRECT(SetFileInformationByHandle, 16);
WINWRAP_REDIRECT(GetFinalPathNameByHandleW, 16);
WINWRAP_REDIRECT(ReOpenFile, 16);

WINWRAP_REDIRECT(CreateSymbolicLinkW, 12);
WINWRAP_REDIRECT(NeedCurrentDirectoryForExePathW, 4);

WINWRAP_REDIRECT(GetNamedPipeClientProcessId, 8);
WINWRAP_REDIRECT(GetNamedPipeServerProcessId, 8);

WINWRAP_REDIRECT(GetCurrentProcessorNumber, 0);
WINWRAP_REDIRECT(GetThreadId, 4);
WINWRAP_REDIRECT(GetErrorMode, 0);
WINWRAP_REDIRECT(SetProcessDPIAware, 0);
WINWRAP_REDIRECT(RegGetValueW, 28);

// advapi32, and 28 rather than 24 because REGHANDLE is 64 bits even on x86.
// EventSetInformation is 20 for the same reason: 8 + 4 + 4 + 4.
//
// EventSetInformation is also the one entry the heading above does not
// describe: it arrived in Windows 8, so its slot is live on 7 too. It stays
// beside its ETW sibling anyway, because what the two share -- the provider
// API, the REGHANDLE, the byte count just above -- says more about this line
// than the version each went missing on does. etw.cc has that per function.
WINWRAP_REDIRECT(EventWriteTransfer, 28);
WINWRAP_REDIRECT(EventSetInformation, 20);
// Four pointers, and then the one REGHANDLE: 16 and 8.
WINWRAP_REDIRECT(EventRegister, 16);
WINWRAP_REDIRECT(EventUnregister, 8);
WINWRAP_REDIRECT(RegisterApplicationRestart, 8);
WINWRAP_REDIRECT(UnregisterApplicationRestart, 0);

WINWRAP_REDIRECT(ConvertThreadToFiberEx, 8);
WINWRAP_REDIRECT(CreateFiberEx, 20);
WINWRAP_REDIRECT(IsThreadAFiber, 0);

WINWRAP_REDIRECT(InitializeConditionVariable, 4);
WINWRAP_REDIRECT(SleepConditionVariableCS, 12);
WINWRAP_REDIRECT(SleepConditionVariableSRW, 16);
WINWRAP_REDIRECT(WakeConditionVariable, 4);
WINWRAP_REDIRECT(WakeAllConditionVariable, 4);

WINWRAP_REDIRECT(InitializeSRWLock, 4);
WINWRAP_REDIRECT(AcquireSRWLockExclusive, 4);
WINWRAP_REDIRECT(ReleaseSRWLockExclusive, 4);
WINWRAP_REDIRECT(AcquireSRWLockShared, 4);
WINWRAP_REDIRECT(ReleaseSRWLockShared, 4);

WINWRAP_REDIRECT(CompareStringOrdinal, 20);
WINWRAP_REDIRECT(GetLocaleInfoEx, 16);
WINWRAP_REDIRECT(LCMapStringEx, 36);
WINWRAP_REDIRECT(CompareStringEx, 36);

// The LCID/name mapping and the four formatters built on it. ResolveLocaleName
// is Windows 7 rather than Vista, so its slot is live one Windows later than
// the rest of this group -- see locale.cc.
WINWRAP_REDIRECT(LocaleNameToLCID, 8);
WINWRAP_REDIRECT(LCIDToLocaleName, 16);
WINWRAP_REDIRECT(ResolveLocaleName, 12);
WINWRAP_REDIRECT(GetDateFormatEx, 28);
WINWRAP_REDIRECT(GetTimeFormatEx, 24);
WINWRAP_REDIRECT(GetNumberFormatEx, 24);
WINWRAP_REDIRECT(GetCurrencyFormatEx, 24);

WINWRAP_REDIRECT(GetQueuedCompletionStatusEx, 24);
WINWRAP_REDIRECT(SetFileCompletionNotificationModes, 8);

WINWRAP_REDIRECT(CancelIoEx, 8);
WINWRAP_REDIRECT(CancelSynchronousIo, 4);

WINWRAP_REDIRECT(InitializeCriticalSectionEx, 12);

WINWRAP_REDIRECT(CreateEventExW, 16);
WINWRAP_REDIRECT(CreateSemaphoreExW, 24);
WINWRAP_REDIRECT(CreateWaitableTimerExW, 16);

// 0 for the first, which takes nothing; 8 for the second, a handle and a
// pointer -- the ULONG64 is written through, not passed; 4 for the third, a
// pointer to the structure it fills.
WINWRAP_REDIRECT(FlushProcessWriteBuffers, 0);
WINWRAP_REDIRECT(QueryThreadCycleTime, 8);
WINWRAP_REDIRECT(GetDynamicTimeZoneInformation, 4);
// A handle, a DWORD and two pointers.
WINWRAP_REDIRECT(QueryFullProcessImageNameW, 16);

// The thread pool timer and wait objects. These are the entries whose handles
// are winwrap's own allocations rather than OS objects, so a process with two
// statically linked copies of winwrap has two incompatible sets -- see the
// note at the top of thread_pool.cc. Nothing about the redirects themselves is
// unusual; the coupling is in what the handles mean.
WINWRAP_REDIRECT(CreateThreadpoolTimer, 12);
WINWRAP_REDIRECT(SetThreadpoolTimer, 16);
WINWRAP_REDIRECT(WaitForThreadpoolTimerCallbacks, 8);
WINWRAP_REDIRECT(CloseThreadpoolTimer, 4);
WINWRAP_REDIRECT(CreateThreadpoolWait, 12);
WINWRAP_REDIRECT(SetThreadpoolWait, 12);
WINWRAP_REDIRECT(WaitForThreadpoolWaitCallbacks, 8);
WINWRAP_REDIRECT(CloseThreadpoolWait, 4);
WINWRAP_REDIRECT(FreeLibraryWhenCallbackReturns, 8);

WINWRAP_REDIRECT(FlsAlloc, 4);
WINWRAP_REDIRECT(FlsGetValue, 4);
WINWRAP_REDIRECT(FlsSetValue, 8);
WINWRAP_REDIRECT(FlsFree, 4);

// shell32, and delay-imported by the consumers this was written for; see the
// note on the Application User Model pair at the end of this file, which
// applies here unchanged. 16: a reference is a pointer, then a DWORD, a
// handle and a pointer.
//
// The slot is half of it. The SDK does not declare this function dllimport,
// so most callers never touch the slot and reference the function by name
// instead; shell32.cc defines that name as well, and says why at length.
WINWRAP_REDIRECT(SHGetKnownFolderPath, 16);

// iphlpapi, delay-imported by node as shell32 is. 8 and 12: an index and a
// pointer, then two pointers and a SIZE_T. net.cc defines the plain names too.
WINWRAP_REDIRECT(ConvertInterfaceIndexToLuid, 8);
WINWRAP_REDIRECT(ConvertInterfaceLuidToNameW, 12);

// dbghelp, which consumers delay-import too, or ship beside the executable.
// 12 and 8: a handle, a pointer and a DWORD, then a handle and a pointer.
WINWRAP_REDIRECT(SymGetSearchPathW, 12);
WINWRAP_REDIRECT(SymSetSearchPathW, 8);

// --- Present on XP SP3, missing on SP2 and earlier -------------------------
//
// Its own section because the heading above does not describe it: XP SP3 and
// Server 2003 SP1 export this, so the slot is dead on every system the rest of
// this file is for. The floor it moves is a service pack rather than a Windows
// version, which is why the measurement in FUNCTION_LIST.md says XP SP3 -- and
// why finding the others like it means running check_imports.py against an SP2
// kernel32 rather than against XP's.
//
// 8 because both parameters are pointers. See system.cc for what the fallback
// can and cannot know; on x64 it never runs, the oldest x64 Windows being
// Server 2003 SP1 underneath.
WINWRAP_REDIRECT(GetLogicalProcessorInformation, 8);

// --- Missing on XP and Vista alike -----------------------------------------
WINWRAP_REDIRECT(TryAcquireSRWLockExclusive, 4);
WINWRAP_REDIRECT(TryAcquireSRWLockShared, 4);

// Missing on 7 as well: Windows 8. Here rather than under a heading of its
// own because the section is about slots that are live past XP, and this is
// the one that is live furthest.
WINWRAP_REDIRECT(GetSystemTimePreciseAsFileTime, 4);

// shell32 rather than kernel32, and delay-imported rather than statically
// imported, which changes when the failure appears but not how it is fixed:
// the reference is satisfied here, so shell32.lib is never consulted for it
// and no delay thunk is generated. See shell32.cc.
WINWRAP_REDIRECT(SetCurrentProcessExplicitAppUserModelID, 4);
WINWRAP_REDIRECT(GetCurrentProcessExplicitAppUserModelID, 4);
