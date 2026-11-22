// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Stress tests for winwrap.
//
// These exercise the Win32 names, not the winwrap_ ones, so what is under test
// is the whole mechanism: whether a call site reaches winwrap's implementation
// at all, and then whether that implementation is right. Build with
// use_winwrap = false and this instead tests the operating system, which is a
// useful thing to be able to do -- the same binary should pass either way, and
// a failure that only appears with use_winwrap = true is winwrap's.
//
// No test framework on purpose: winwrap has no dependencies and its tests
// should not acquire any.
//
// The locks are checked by assertion rather than by outcome. A correct result
// from a broken lock is the normal case -- races are rare -- so each critical
// section verifies that nobody else is inside it while it runs, which turns an
// intermittent wrong answer into a reliable complaint.

#include "winwrap_unittest.h"

// The AppUserModelID pair lives in shlobj.h, and CoTaskMemFree in objbase.h;
// neither arrives with windows.h.
#include <objbase.h>

#include <shlobj.h>

// EventWriteTransfer and EVENT_DESCRIPTOR, likewise not in windows.h. Gated on
// WINVER >= 0x0600, which this file is built at -- it deliberately does not
// take winwrap's own lowered target, since it has to be able to call the Win32
// names to test them.
#include <evntprov.h>

// _getch, for holding the window open when this was double-clicked. A CRT
// function rather than ReadConsoleInput so there is no console mode to save
// and restore, and no risk of leaving the console in a different state than it
// was found in.
#include <conio.h>

// memcmp, for comparing the two RegGetValueW results byte for byte. Named
// because nothing else here declares it: framework.h carries <stdlib.h>,
// <cstdint> and <cwchar>, but not this one -- so the only reason this has been
// compiling is that windows.h happens to drag it in.
#include <string.h>

#ifdef WINWRAP_DLL
// DLLVERSIONINFO, DLLVERSIONINFO2, MAKEDLLVERULL and DLLVER_PLATFORM_NT, for
// the DllGetVersion coverage below. framework.h pulls this in under the same
// condition, but named here because this file is what uses those four, and a
// test should not depend on a private header's conditional includes.
//
// No shlwapi.lib needed: all four are a struct, a struct, a macro and a macro,
// and DllGetVersion itself is reached through GetProcAddress. So neither
// TESTLIBS in the Makefile nor the link settings in test/BUILD.gn change.
#include <shlwapi.h>
#endif // WINWRAP_DLL

// The XP code path of ww_RegGetValueW, which TestRegGetValue compares against
// the real advapi32 function. Declared here rather than at the point of use
// because a linkage specification may only appear at namespace scope -- inside
// a function body it is ill-formed, and both gcc and clang reject it outright
// rather than warning.
//
// Not in winwrap.h: that header declares the ww_* stand-ins, and this is not
// one. See src/registry.cc for why it exists as a separate entry point.
extern "C" LSTATUS APIENTRY winwrap_RegGetValueFallbackW(HKEY hkey,
                                                         LPCWSTR lpSubKey,
                                                         LPCWSTR lpValue,
                                                         DWORD dwFlags,
                                                         LPDWORD pdwType,
                                                         PVOID pvData,
                                                         LPDWORD pcbData);

namespace {

  constexpr DWORD kThreads      = 16;
  constexpr int kLockIterations = 20000;

  void RunThreads(LPTHREAD_START_ROUTINE body, DWORD count) {
    HANDLE threads[kThreads] = {};
    for (DWORD i = 0; i < count; ++i) {
      threads[i] = ::CreateThread(nullptr, 0, body,
                                  reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(i)), 0, nullptr);
      if (!threads[i]) {
        ++g_failures;
        std::wcout << L"    FAIL could not create thread " << i << std::endl;
        count = i;
        break;
      }
    }
    if (count) {
      ::WaitForMultipleObjects(count, threads, TRUE, INFINITE);
    }
    for (DWORD i = 0; i < count; ++i) {
      ::CloseHandle(threads[i]);
    }
  }

  // --- SRWLOCK, exclusive ----------------------------------------------------

  SRWLOCK g_exclusive_lock;
  volatile LONG g_inside = 0;
  // Deliberately not atomic. Its final value is only correct if the lock really
  // excluded everyone, which is the point.
  unsigned long long g_plain_counter = 0;

  DWORD WINAPI ExclusiveBody(LPVOID) {
    for (int i = 0; i < kLockIterations; ++i) {
      ::AcquireSRWLockExclusive(&g_exclusive_lock);
      LONG inside = ::InterlockedIncrement(&g_inside);
      CHECK(inside == 1, inside << L" threads inside an exclusive lock");
      ++g_plain_counter;
      ::InterlockedDecrement(&g_inside);
      ::ReleaseSRWLockExclusive(&g_exclusive_lock);
    }
    return 0;
  }

  void TestExclusive() {
    std::wcout << L"  SRWLOCK exclusive" << std::endl;
    ::InitializeSRWLock(&g_exclusive_lock);
    g_plain_counter = 0;
    RunThreads(ExclusiveBody, kThreads);

    unsigned long long expected = static_cast<unsigned long long>(kThreads) * kLockIterations;
    CHECK(g_plain_counter == expected,
          L"counter is " << g_plain_counter << L", expected " << expected);
  }

  // --- SRWLOCK, shared vs exclusive ------------------------------------------

  SRWLOCK g_shared_lock;
  volatile LONG g_readers     = 0;
  volatile LONG g_writers     = 0;
  volatile LONG g_max_readers = 0;
  // Writers keep these equal; a reader that sees them differ caught a writer
  // mid-update, which a shared lock must make impossible.
  volatile LONG g_left  = 0;
  volatile LONG g_right = 0;

  DWORD WINAPI SharedBody(LPVOID) {
    for (int i = 0; i < kLockIterations; ++i) {
      if ((i & 7) == 0) {
        ::AcquireSRWLockExclusive(&g_shared_lock);
        CHECK(::InterlockedIncrement(&g_writers) == 1, L"two writers at once");
        CHECK(g_readers == 0, g_readers << L" readers inside an exclusive lock");
        LONG value = g_left + 1;
        g_left     = value;
        ::Sleep(0); // Widen the window a torn read could fall into.
        g_right = value;
        ::InterlockedDecrement(&g_writers);
        ::ReleaseSRWLockExclusive(&g_shared_lock);
      } else {
        ::AcquireSRWLockShared(&g_shared_lock);
        LONG now = ::InterlockedIncrement(&g_readers);
        if (now > g_max_readers) {
          g_max_readers = now; // Racy, but only ever used as a lower bound.
        }
        CHECK(g_writers == 0, L"a writer is inside while " << now << L" readers are");
        CHECK(g_left == g_right, L"reader saw a torn update: " << g_left << L" vs " << g_right);
        ::InterlockedDecrement(&g_readers);
        ::ReleaseSRWLockShared(&g_shared_lock);
      }
    }
    return 0;
  }

  void TestShared() {
    std::wcout << L"  SRWLOCK shared" << std::endl;
    ::InitializeSRWLock(&g_shared_lock);
    g_max_readers = 0;
    RunThreads(SharedBody, kThreads);

    // The old CRITICAL_SECTION implementation serialised readers, so this is
    // the assertion that distinguishes a real reader/writer lock from a mutex
    // wearing its name.
    CHECK(g_max_readers > 1, L"readers never overlapped (max "
                                 << g_max_readers << L"); shared is behaving like exclusive");
    std::wcout << L"    peak concurrent readers: " << g_max_readers << std::endl;
  }

  void TestTryAcquire() {
    std::wcout << L"  SRWLOCK try variants" << std::endl;
    SRWLOCK lock;
    ::InitializeSRWLock(&lock);

    CHECK(::TryAcquireSRWLockExclusive(&lock), L"try-exclusive failed when free");
    ::ReleaseSRWLockExclusive(&lock);

    ::AcquireSRWLockShared(&lock);
    CHECK(!::TryAcquireSRWLockExclusive(&lock), L"try-exclusive succeeded while held shared");
    CHECK(::TryAcquireSRWLockShared(&lock), L"try-shared failed while held shared");
    ::ReleaseSRWLockShared(&lock);
    ::ReleaseSRWLockShared(&lock);

    ::AcquireSRWLockExclusive(&lock);
    CHECK(!::TryAcquireSRWLockShared(&lock), L"try-shared succeeded while held exclusive");
    ::ReleaseSRWLockExclusive(&lock);
  }

  // --- Condition variables ---------------------------------------------------

  constexpr int kQueueCapacity    = 8;
  constexpr int kItemsPerProducer = 4000;
  constexpr DWORD kProducers      = 4;
  constexpr DWORD kConsumers      = 4;

  CRITICAL_SECTION g_queue_lock;
  CONDITION_VARIABLE g_not_empty;
  CONDITION_VARIABLE g_not_full;
  int g_queue[kQueueCapacity];
  int g_queue_count        = 0;
  int g_queue_head         = 0;
  int g_queue_tail         = 0;
  int g_produced           = 0;
  long long g_consumed_sum = 0;
  int g_consumed_count     = 0;
  bool g_producers_done    = false;

  DWORD WINAPI ProducerBody(LPVOID) {
    for (int i = 0; i < kItemsPerProducer; ++i) {
      ::EnterCriticalSection(&g_queue_lock);
      while (g_queue_count == kQueueCapacity) {
        ::SleepConditionVariableCS(&g_not_full, &g_queue_lock, INFINITE);
      }
      g_queue[g_queue_tail] = 1;
      g_queue_tail          = (g_queue_tail + 1) % kQueueCapacity;
      ++g_queue_count;
      ++g_produced;
      ::LeaveCriticalSection(&g_queue_lock);
      ::WakeConditionVariable(&g_not_empty);
    }
    return 0;
  }

  DWORD WINAPI ConsumerBody(LPVOID) {
    for (;;) {
      ::EnterCriticalSection(&g_queue_lock);
      while (g_queue_count == 0 && !g_producers_done) {
        ::SleepConditionVariableCS(&g_not_empty, &g_queue_lock, INFINITE);
      }
      if (g_queue_count == 0 && g_producers_done) {
        ::LeaveCriticalSection(&g_queue_lock);
        return 0;
      }
      int value    = g_queue[g_queue_head];
      g_queue_head = (g_queue_head + 1) % kQueueCapacity;
      --g_queue_count;
      g_consumed_sum += value;
      ++g_consumed_count;
      ::LeaveCriticalSection(&g_queue_lock);
      ::WakeConditionVariable(&g_not_full);
    }
  }

  void TestConditionVariableQueue() {
    std::wcout << L"  CONDITION_VARIABLE producer/consumer" << std::endl;
    ::InitializeCriticalSection(&g_queue_lock);
    ::InitializeConditionVariable(&g_not_empty);
    ::InitializeConditionVariable(&g_not_full);

    HANDLE threads[kProducers + kConsumers] = {};
    DWORD n                                 = 0;
    for (DWORD i = 0; i < kConsumers; ++i) {
      threads[n++] = ::CreateThread(nullptr, 0, ConsumerBody, nullptr, 0, nullptr);
    }
    for (DWORD i = 0; i < kProducers; ++i) {
      threads[n++] = ::CreateThread(nullptr, 0, ProducerBody, nullptr, 0, nullptr);
    }

    // Let the producers finish, then release any consumer still waiting. A
    // consumer asleep here with nobody to wake it is precisely the lost-wake-up
    // this implementation has to prevent.
    ::WaitForMultipleObjects(kProducers, threads + kConsumers, TRUE, INFINITE);
    ::EnterCriticalSection(&g_queue_lock);
    g_producers_done = true;
    ::LeaveCriticalSection(&g_queue_lock);
    ::WakeAllConditionVariable(&g_not_empty);

    DWORD wait = ::WaitForMultipleObjects(kConsumers, threads, TRUE, 30000);
    CHECK(wait != WAIT_TIMEOUT, L"consumers never woke up -- lost wake-up");

    for (DWORD i = 0; i < n; ++i) {
      ::CloseHandle(threads[i]);
    }

    int expected = static_cast<int>(kProducers) * kItemsPerProducer;
    CHECK(g_produced == expected, L"produced " << g_produced << L", expected " << expected);
    CHECK(g_consumed_count == expected,
          L"consumed " << g_consumed_count << L", expected " << expected);
    ::DeleteCriticalSection(&g_queue_lock);
  }

  CONDITION_VARIABLE g_timeout_cv;
  CRITICAL_SECTION g_timeout_cs;

  DWORD WINAPI TryEnterBody(LPVOID held) {
    BOOL got = ::TryEnterCriticalSection(&g_timeout_cs);
    if (got) {
      ::LeaveCriticalSection(&g_timeout_cs);
    }
    *static_cast<BOOL*>(held) = !got;
    return 0;
  }

  void TestConditionVariableTimeout() {
    std::wcout << L"  CONDITION_VARIABLE timed wait" << std::endl;
    ::InitializeCriticalSection(&g_timeout_cs);
    ::InitializeConditionVariable(&g_timeout_cv);

    ::EnterCriticalSection(&g_timeout_cs);
    DWORD start   = ::GetTickCount();
    BOOL result   = ::SleepConditionVariableCS(&g_timeout_cv, &g_timeout_cs, 200);
    DWORD elapsed = ::GetTickCount() - start;
    DWORD error   = ::GetLastError();

    CHECK(!result, L"a timed wait with no wake-up returned success");
    CHECK(error == ERROR_TIMEOUT, L"last error is " << error << L", expected ERROR_TIMEOUT");
    // Timer granularity on these systems is ~15ms, so allow slack below.
    CHECK(elapsed >= 150, L"returned after " << elapsed << L"ms, asked for 200ms");

    // The caller must hold the lock again on return, including after a timeout.
    BOOL still_held = FALSE;
    HANDLE prober   = ::CreateThread(nullptr, 0, TryEnterBody, &still_held, 0, nullptr);
    ::WaitForSingleObject(prober, INFINITE);
    ::CloseHandle(prober);
    CHECK(still_held, L"lock was not re-acquired after a timed wait");

    ::LeaveCriticalSection(&g_timeout_cs);
    ::DeleteCriticalSection(&g_timeout_cs);
  }

  CONDITION_VARIABLE g_broadcast_cv;
  SRWLOCK g_broadcast_lock;
  bool g_broadcast_go             = false;
  volatile LONG g_broadcast_woken = 0;

  DWORD WINAPI BroadcastBody(LPVOID) {
    ::AcquireSRWLockExclusive(&g_broadcast_lock);
    while (!g_broadcast_go) {
      ::SleepConditionVariableSRW(&g_broadcast_cv, &g_broadcast_lock, INFINITE, 0);
    }
    ::ReleaseSRWLockExclusive(&g_broadcast_lock);
    ::InterlockedIncrement(&g_broadcast_woken);
    return 0;
  }

  void TestConditionVariableBroadcast() {
    std::wcout << L"  CONDITION_VARIABLE broadcast over SRWLOCK" << std::endl;
    ::InitializeSRWLock(&g_broadcast_lock);
    ::InitializeConditionVariable(&g_broadcast_cv);
    g_broadcast_go    = false;
    g_broadcast_woken = 0;

    HANDLE threads[kThreads] = {};
    for (DWORD i = 0; i < kThreads; ++i) {
      threads[i] = ::CreateThread(nullptr, 0, BroadcastBody, nullptr, 0, nullptr);
    }
    ::Sleep(100); // Give them time to actually be waiting.

    ::AcquireSRWLockExclusive(&g_broadcast_lock);
    g_broadcast_go = true;
    ::ReleaseSRWLockExclusive(&g_broadcast_lock);
    // Every waiter must be released by one call, not just the first.
    ::WakeAllConditionVariable(&g_broadcast_cv);

    DWORD wait = ::WaitForMultipleObjects(kThreads, threads, TRUE, 30000);
    CHECK(wait != WAIT_TIMEOUT, L"only " << g_broadcast_woken << L" of " << kThreads
                                         << L" waiters woke from a broadcast");
    for (DWORD i = 0; i < kThreads; ++i) {
      ::CloseHandle(threads[i]);
    }
  }

  // --- Fiber-local storage ---------------------------------------------------

  DWORD g_fls_index             = FLS_OUT_OF_INDEXES;
  volatile LONG g_fls_callbacks = 0;

  void WINAPI FlsCallback(PVOID) {
    ::InterlockedIncrement(&g_fls_callbacks);
  }

  DWORD WINAPI FlsBody(LPVOID id) {
    CHECK(::FlsGetValue(g_fls_index) == nullptr, L"a fresh thread already had an FLS value");
    PVOID mine =
        reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(reinterpret_cast<ULONG_PTR>(id) + 1));
    CHECK(::FlsSetValue(g_fls_index, mine), L"FlsSetValue failed");
    ::Sleep(5); // Let every other thread store its own value first.
    CHECK(::FlsGetValue(g_fls_index) == mine, L"another thread's FLS value leaked in");
    return 0;
  }

  void TestFls() {
    std::wcout << L"  Fiber-local storage" << std::endl;
    g_fls_callbacks = 0;
    g_fls_index     = ::FlsAlloc(FlsCallback);
    CHECK(g_fls_index != FLS_OUT_OF_INDEXES, L"FlsAlloc failed");
    if (g_fls_index == FLS_OUT_OF_INDEXES) {
      return;
    }

    RunThreads(FlsBody, kThreads);

    // The callback fires from a TLS callback at thread exit, which is the part
    // of this that XP does not do by itself.
    CHECK(g_fls_callbacks == static_cast<LONG>(kThreads),
          g_fls_callbacks << L" thread-exit callbacks, expected " << kThreads);

    CHECK(::FlsFree(g_fls_index), L"FlsFree failed");
    CHECK(!::FlsFree(g_fls_index), L"freeing an index twice succeeded");
  }

  // --- Odds and ends ---------------------------------------------------------

  INIT_ONCE g_once          = INIT_ONCE_STATIC_INIT;
  volatile LONG g_once_runs = 0;

  BOOL CALLBACK OnceBody(PINIT_ONCE, PVOID, PVOID*) {
    ::InterlockedIncrement(&g_once_runs);
    ::Sleep(10);
    return TRUE;
  }

  DWORD WINAPI OnceCaller(LPVOID) {
    CHECK(::InitOnceExecuteOnce(&g_once, OnceBody, nullptr, nullptr),
          L"InitOnceExecuteOnce failed");
    return 0;
  }

  void TestMisc() {
    std::wcout << L"  InitOnceExecuteOnce, GetTickCount64" << std::endl;

    g_once_runs = 0;
    RunThreads(OnceCaller, kThreads);
    CHECK(g_once_runs == 1, L"the once-callback ran " << g_once_runs << L" times");

    ULONGLONG first = ::GetTickCount64();
    ::Sleep(50);
    ULONGLONG second = ::GetTickCount64();
    CHECK(second >= first, L"GetTickCount64 went backwards");
    CHECK(second - first >= 30,
          L"GetTickCount64 advanced only " << (second - first) << L" ms over 50");
  }

  // --- Locale and string -----------------------------------------------------

  // Whether `module` exports `name` itself, which decides whether a call went
  // to the system or to winwrap's own code. Several of the checks below are
  // only meaningful for one of those, so they have to know which happened.
  //
  // GetModuleHandleW rather than LoadLibraryW: every module asked about here
  // is already loaded, and this way the probe cannot change what is in the
  // process.
  bool SystemHas(const wchar_t* module, const char* name) {
    HMODULE loaded = ::GetModuleHandleW(module);
    return loaded != nullptr && ::GetProcAddress(loaded, name) != nullptr;
  }

  // The LCID for en-US, spelled out. LocaleNameToLCID is itself Vista+, so
  // asking for it would make this test depend on the thing it is testing
  // around.
  const LCID kEnUs = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);

  void TestLocale() {
    std::wcout << L"  CompareStringOrdinal, GetLocaleInfoEx, LCMapStringEx, CompareStringEx"
               << std::endl;

    CHECK(::CompareStringOrdinal(L"abc", -1, L"abc", -1, FALSE) == CSTR_EQUAL,
          L"equal strings did not compare equal");
    CHECK(::CompareStringOrdinal(L"abc", -1, L"ABC", -1, TRUE) == CSTR_EQUAL,
          L"case-insensitive compare failed");
    CHECK(::CompareStringOrdinal(L"abc", -1, L"ABC", -1, FALSE) == CSTR_GREATER_THAN,
          L"lowercase did not sort after uppercase ordinally");
    CHECK(::CompareStringOrdinal(L"abc", -1, L"abcd", -1, FALSE) == CSTR_LESS_THAN,
          L"a prefix did not sort before the longer string");
    CHECK(::CompareStringOrdinal(L"abcd", 3, L"abc", -1, FALSE) == CSTR_EQUAL,
          L"an explicit length was not honoured");

    // Not every system has this locale; only check the shape of the answer.
    wchar_t buffer[64] = {};
    int written = ::GetLocaleInfoEx(L"en-US", LOCALE_SENGLANGUAGE, buffer, ARRAYSIZE(buffer));
    if (written > 0) {
      std::wcout << L"    GetLocaleInfoEx(en-US) -> \"" << buffer << L"\"" << std::endl;
    } else {
      std::wcout << L"    GetLocaleInfoEx(en-US) unavailable on this system" << std::endl;
    }

    // --- LCMapStringEx and CompareStringEx
    //
    // Both resolve the locale name to an LCID and then call the XP function,
    // so the check that matters is that doing it by name gives the same answer
    // as doing it by LCID. That is the premise the fallback rests on, and it
    // can be verified anywhere -- unlike the fallback itself, which only runs
    // on a Windows that lacks these, so a test machine never reaches it.

    // "strasse" with a sharp s, written as a universal character name rather
    // than as the character. This file is UTF-8 with no BOM and nothing passes
    // /utf-8, so cl reads it in the system code page: on the CI runner the two
    // bytes of U+00DF arrived as two CP1252 characters, which is what made the
    // log read "STRA?x.E". The mapping still agreed with itself, so the checks
    // passed while testing a different string than the one written here. A
    // \u escape is read the same way by every compiler in every code page.
    const wchar_t kSharpS[] = L"stra\u00dfe";

    wchar_t upper_ex[16] = {};
    wchar_t upper_w[16]  = {};
    int ex_len           = ::LCMapStringEx(L"en-US", LCMAP_UPPERCASE, kSharpS, -1, upper_ex,
                                           ARRAYSIZE(upper_ex), nullptr, nullptr, 0);
    int w_len = ::LCMapStringW(kEnUs, LCMAP_UPPERCASE, kSharpS, -1, upper_w, ARRAYSIZE(upper_w));
    CHECK(ex_len == w_len,
          L"LCMapStringEx returned " << ex_len << L" chars, LCMapStringW " << w_len);
    if (ex_len > 0 && ex_len == w_len) {
      CHECK(::wcscmp(upper_ex, upper_w) == 0, L"LCMapStringEx gave \""
                                                  << Printable(upper_ex) << L"\", LCMapStringW \""
                                                  << Printable(upper_w) << L"\"");
      // Escaped, because the answer is not ASCII and the console would mangle
      // it: Windows maps a lone sharp s to itself rather than to SS unless
      // asked for linguistic casing, so this reads STRA\u00dfE on a machine
      // that agrees with itself.
      std::wcout << L"    LCMapStringEx(en-US, UPPERCASE) -> \"" << Printable(upper_ex) << L"\""
                 << std::endl;
    }

    // A sort order the ordinal comparison above would get wrong, so this is
    // actually exercising the linguistic path rather than agreeing by accident.
    const int ex_cmp =
        ::CompareStringEx(L"en-US", 0, L"apple", -1, L"Banana", -1, nullptr, nullptr, 0);
    const int w_cmp = ::CompareStringW(kEnUs, 0, L"apple", -1, L"Banana", -1);
    CHECK(ex_cmp == w_cmp,
          L"CompareStringEx said " << ex_cmp << L", CompareStringW said " << w_cmp);
    CHECK(ex_cmp == CSTR_LESS_THAN,
          L"\"apple\" did not sort before \"Banana\" linguistically (" << ex_cmp << L")");

    CHECK(::CompareStringEx(L"en-US", NORM_IGNORECASE, L"abc", -1, L"ABC", -1, nullptr, nullptr,
                            0) == CSTR_EQUAL,
          L"NORM_IGNORECASE was not honoured");

    // The three arguments the Ex forms added, which winwrap refuses because XP
    // has nothing to map them onto. Only assertable where winwrap is the one
    // answering: where the system exports these, the call is forwarded and the
    // real function decides, and it accepts a sort version perfectly well.
    if (!SystemHas(L"kernel32.dll", "CompareStringEx")) {
      ::SetLastError(ERROR_SUCCESS);
      CHECK(::CompareStringEx(L"en-US", 0, L"a", -1, L"a", -1, nullptr, reinterpret_cast<LPVOID>(1),
                              0) == 0,
            L"CompareStringEx accepted a non-null lpReserved");
      CHECK(::GetLastError() == ERROR_INVALID_PARAMETER,
            L"refusing lpReserved set last error to " << Hex(::GetLastError()));
      std::wcout << L"    on the fallback path; refusal of the Ex-only arguments checked"
                 << std::endl;
    } else {
      std::wcout << L"    forwarding to the system; Ex-only arguments are its business"
                 << std::endl;
    }
  }

  void TestLocaleNames() {
    std::wcout << L"  LocaleNameToLCID, LCIDToLocaleName, ResolveLocaleName" << std::endl;

    const bool winwrap_answers = !SystemHas(L"kernel32.dll", "LocaleNameToLCID");

    // en-US both ways. Asserted against the spelled-out LCID rather than
    // against whatever the machine's default is, because this is the one
    // locale every Windows install has and 0x0409 is what it has always been.
    const LCID en_us = ::LocaleNameToLCID(L"en-US", 0);
    std::wcout << L"    en-US -> " << Hex(en_us) << std::endl;
    CHECK(en_us == kEnUs, L"en-US resolved to " << Hex(en_us) << L" rather than " << Hex(kEnUs));

    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    int written = ::LCIDToLocaleName(kEnUs, name, ARRAYSIZE(name), 0);
    std::wcout << L"    " << Hex(kEnUs) << L" -> " << Printable(name) << L" (" << written
               << L" chars)" << std::endl;
    CHECK(written > 0, L"LCIDToLocaleName failed with " << Hex(::GetLastError()));
    if (written > 0) {
      // The count includes the terminator, which is the half of this contract
      // easiest to get wrong in either direction.
      CHECK(written == static_cast<int>(::wcslen(name)) + 1,
            L"returned " << written << L" for a " << ::wcslen(name) << L" character name");
      CHECK(::CompareStringOrdinal(name, -1, L"en-US", -1, TRUE) == CSTR_EQUAL,
            L"0x0409 named itself " << Printable(name) << L" rather than en-US");
    }

    // A zero count measures, and must not touch the buffer; a short one fails.
    const int needed = ::LCIDToLocaleName(kEnUs, nullptr, 0, 0);
    CHECK(needed == written, L"the measuring call wanted " << needed << L" but the real one wrote "
                                                           << written);
    wchar_t tiny[2] = {L'!', L'!'};
    CHECK(::LCIDToLocaleName(kEnUs, tiny, 1, 0) == 0, L"a one-character buffer was accepted");
    CHECK(tiny[0] == L'!', L"the buffer was written past the length it declared");
    if (winwrap_answers) {
      CHECK(::GetLastError() == ERROR_INSUFFICIENT_BUFFER,
            L"a short buffer set last error to " << Hex(::GetLastError()));
    }

    // LOCALE_ALLOW_NEUTRAL_NAMES has to be accepted rather than rejected as an
    // unknown flag: ICU's locmap.cpp passes it on every call, so refusing it
    // would fail the only caller in the tree.
    wchar_t neutral[LOCALE_NAME_MAX_LENGTH] = {};
    const int neutral_len =
        ::LCIDToLocaleName(kEnUs, neutral, ARRAYSIZE(neutral), LOCALE_ALLOW_NEUTRAL_NAMES);
    CHECK(neutral_len > 0, L"LOCALE_ALLOW_NEUTRAL_NAMES was rejected with "
                               << Hex(::GetLastError()));

    // The invariant locale's name is the empty string, which is the inverse of
    // the mapping the name-to-LCID direction applies to it.
    wchar_t invariant[LOCALE_NAME_MAX_LENGTH] = {L'!'};
    const int invariant_len = ::LCIDToLocaleName(LOCALE_INVARIANT, invariant,
                                                 ARRAYSIZE(invariant), 0);
    CHECK(invariant_len == 1 && invariant[0] == L'\0',
          L"LOCALE_INVARIANT named itself " << Printable(invariant) << L" (" << invariant_len
                                            << L" chars) rather than the empty string");

    // Round trip, which is the property that actually matters to a caller:
    // whatever the system calls a locale has to map back to the same LCID.
    wchar_t round[LOCALE_NAME_MAX_LENGTH] = {};
    if (::LCIDToLocaleName(kEnUs, round, ARRAYSIZE(round), 0) > 0) {
      CHECK(::LocaleNameToLCID(round, 0) == kEnUs,
            L"round trip through " << Printable(round) << L" lost the LCID");
    }

    // ResolveLocaleName normalises a name the system has. Case is the part
    // both implementations agree on; the fallback half of "resolve" -- walking
    // up from a tag with no such locale -- is deliberately absent, see
    // locale.cc, so an unresolvable name is not asserted either way.
    wchar_t resolved[LOCALE_NAME_MAX_LENGTH] = {};
    const int resolved_len = ::ResolveLocaleName(L"EN-us", resolved, ARRAYSIZE(resolved));
    std::wcout << L"    EN-us resolves to " << Printable(resolved) << std::endl;
    CHECK(resolved_len > 0, L"ResolveLocaleName failed with " << Hex(::GetLastError()));
    if (resolved_len > 0) {
      CHECK(resolved_len == static_cast<int>(::wcslen(resolved)) + 1,
            L"ResolveLocaleName returned " << resolved_len << L" for a " << ::wcslen(resolved)
                                           << L" character name");
      CHECK(::CompareStringOrdinal(resolved, -1, L"en-US", -1, TRUE) == CSTR_EQUAL,
            L"EN-us resolved to " << Printable(resolved) << L" rather than en-US");
    }

    if (winwrap_answers) {
      // A flag neither function can honour. Only assertable on the fallback:
      // a real Windows may add meaning to a bit this one has never heard of.
      CHECK(::LocaleNameToLCID(L"en-US", 0x00000004) == 0,
            L"LocaleNameToLCID accepted an unsupported flag");
      CHECK(::GetLastError() == ERROR_INVALID_FLAGS,
            L"an unsupported flag set last error to " << Hex(::GetLastError()));
      std::wcout << L"    on the fallback path; flag and buffer handling checked" << std::endl;
    } else {
      std::wcout << L"    forwarding to the system" << std::endl;
    }
  }

  void TestLocaleFormatters() {
    std::wcout << L"  GetDateFormatEx, GetTimeFormatEx, GetNumberFormatEx, GetCurrencyFormatEx"
               << std::endl;

    const bool winwrap_answers = !SystemHas(L"kernel32.dll", "GetDateFormatEx");

    // A fixed moment, so the expected output does not depend on the clock.
    SYSTEMTIME when = {};
    when.wYear = 2026, when.wMonth = 11, when.wDay = 21;
    when.wHour = 14, when.wMinute = 30, when.wSecond = 45;
    when.wDayOfWeek = 6; // A Saturday, and ignored by a picture string anyway.

    // Each of the four against the LCID form of the same call. That is exactly
    // what the fallback does internally, so on XP this confirms the name
    // resolution; on a current Windows it confirms the system's Ex function
    // and its own older one agree, which is the claim the shim rests on.
    wchar_t ex[128]  = {};
    wchar_t old[128] = {};

    const int date_ex = ::GetDateFormatEx(L"en-US", 0, &when, L"yyyy'-'MM'-'dd", ex,
                                          ARRAYSIZE(ex), nullptr);
    const int date_old = ::GetDateFormatW(kEnUs, 0, &when, L"yyyy'-'MM'-'dd", old, ARRAYSIZE(old));
    std::wcout << L"    date " << Printable(ex) << L" / " << Printable(old) << std::endl;
    CHECK(date_ex > 0, L"GetDateFormatEx failed with " << Hex(::GetLastError()));
    CHECK(date_ex == date_old && ::CompareStringOrdinal(ex, -1, old, -1, FALSE) == CSTR_EQUAL,
          L"GetDateFormatEx gave " << Printable(ex) << L" where GetDateFormatW gave "
                                   << Printable(old));
    // The picture string is explicit, so the answer is not locale-dependent
    // and can be named outright.
    CHECK(::CompareStringOrdinal(ex, -1, L"2026-11-21", -1, FALSE) == CSTR_EQUAL,
          L"the fixed date formatted as " << Printable(ex) << L" rather than 2026-11-21");

    const int time_ex =
        ::GetTimeFormatEx(L"en-US", 0, &when, L"HH':'mm':'ss", ex, ARRAYSIZE(ex));
    const int time_old = ::GetTimeFormatW(kEnUs, 0, &when, L"HH':'mm':'ss", old, ARRAYSIZE(old));
    std::wcout << L"    time " << Printable(ex) << L" / " << Printable(old) << std::endl;
    CHECK(time_ex > 0, L"GetTimeFormatEx failed with " << Hex(::GetLastError()));
    CHECK(time_ex == time_old && ::CompareStringOrdinal(ex, -1, old, -1, FALSE) == CSTR_EQUAL,
          L"GetTimeFormatEx gave " << Printable(ex) << L" where GetTimeFormatW gave "
                                   << Printable(old));
    CHECK(::CompareStringOrdinal(ex, -1, L"14:30:45", -1, FALSE) == CSTR_EQUAL,
          L"the fixed time formatted as " << Printable(ex) << L" rather than 14:30:45");

    // Numbers and currency with a null format, which means the locale's own
    // conventions -- so these are compared against the LCID call rather than
    // against a literal, since the expected text depends on the machine.
    const int num_ex = ::GetNumberFormatEx(L"en-US", 0, L"-1234567.891", nullptr, ex,
                                           ARRAYSIZE(ex));
    const int num_old = ::GetNumberFormatW(kEnUs, 0, L"-1234567.891", nullptr, old,
                                           ARRAYSIZE(old));
    std::wcout << L"    number " << Printable(ex) << L" / " << Printable(old) << std::endl;
    CHECK(num_ex > 0, L"GetNumberFormatEx failed with " << Hex(::GetLastError()));
    CHECK(num_ex == num_old && ::CompareStringOrdinal(ex, -1, old, -1, FALSE) == CSTR_EQUAL,
          L"GetNumberFormatEx gave " << Printable(ex) << L" where GetNumberFormatW gave "
                                     << Printable(old));

    const int cur_ex = ::GetCurrencyFormatEx(L"en-US", 0, L"1234.5", nullptr, ex, ARRAYSIZE(ex));
    const int cur_old = ::GetCurrencyFormatW(kEnUs, 0, L"1234.5", nullptr, old, ARRAYSIZE(old));
    std::wcout << L"    currency " << Printable(ex) << L" / " << Printable(old) << std::endl;
    CHECK(cur_ex > 0, L"GetCurrencyFormatEx failed with " << Hex(::GetLastError()));
    CHECK(cur_ex == cur_old && ::CompareStringOrdinal(ex, -1, old, -1, FALSE) == CSTR_EQUAL,
          L"GetCurrencyFormatEx gave " << Printable(ex) << L" where GetCurrencyFormatW gave "
                                        << Printable(old));

    // The measuring convention carries over from the older calls untouched,
    // so a zero count has to report the length rather than format anything.
    const int measured = ::GetDateFormatEx(L"en-US", 0, &when, L"yyyy'-'MM'-'dd", nullptr, 0,
                                           nullptr);
    CHECK(measured == date_ex,
          L"the measuring call wanted " << measured << L" but the real one wrote " << date_ex);

    if (winwrap_answers) {
      // lpCalendar is the one parameter the Ex form added, and it is refused
      // rather than ignored -- a caller that asked for a different calendar
      // and silently got the locale's has a wrong date. Only assertable on the
      // fallback, since a real Windows honours it.
      CHECK(::GetDateFormatEx(L"en-US", 0, &when, L"yyyy", ex, ARRAYSIZE(ex), L"1") == 0,
            L"GetDateFormatEx accepted a calendar it cannot select");
      CHECK(::GetLastError() == ERROR_INVALID_PARAMETER,
            L"refusing lpCalendar set last error to " << Hex(::GetLastError()));
      std::wcout << L"    on the fallback path; lpCalendar refusal checked" << std::endl;
    } else {
      std::wcout << L"    forwarding to the system; lpCalendar is its business" << std::endl;
    }
  }

  // --- ETW manifest-based providers ------------------------------------------

  void TestEtw() {
    std::wcout << L"  EventWriteTransfer" << std::endl;

    const EVENT_DESCRIPTOR descriptor = {};

    // REGHANDLE 0 is never a registered provider, so this has to fail -- and
    // that is assertable on every Windows. On XP there is no manifest-based
    // ETW at all and winwrap answers; everywhere else the real function
    // answers, and both say the same thing about an invalid handle.
    const ULONG result = ::EventWriteTransfer(0, &descriptor, nullptr, nullptr, 0, nullptr);
    CHECK(result != ERROR_SUCCESS, L"writing through an unregistered handle reported success");

    if (!SystemHas(L"advapi32.dll", "EventWriteTransfer")) {
      // Where there is no system function, winwrap answered, and this is the
      // contract it promises: ERROR_INVALID_HANDLE, because on such a Windows
      // nothing could ever have produced a valid REGHANDLE.
      CHECK(result == ERROR_INVALID_HANDLE,
            L"the fallback returned " << Hex(result) << L", expected ERROR_INVALID_HANDLE");
      std::wcout << L"    no manifest-based ETW here; winwrap answered" << std::endl;
    } else {
      std::wcout << L"    forwarded to the system's own EventWriteTransfer" << std::endl;
    }
    std::wcout << L"    unregistered handle -> " << Hex(result) << std::endl;

    std::wcout << L"  EventSetInformation" << std::endl;

    // Same shape, two Windows later: this arrived in Windows 8, so Vista and
    // Windows 7 both reach winwrap for it while reaching the system for
    // everything else in the family. An unregistered handle must fail here
    // too, and that is assertable wherever this runs.
    //
    // EventProviderSetTraits is 2 in evntprov.h's EVENT_INFO_CLASS. Written as
    // the enumerator so this breaks loudly if that ever stops being true,
    // rather than passing a literal that silently means something else.
    const ULONG info = ::EventSetInformation(0, EventProviderSetTraits, nullptr, 0);
    CHECK(info != ERROR_SUCCESS, L"setting information on an unregistered handle reported success");

    if (!SystemHas(L"advapi32.dll", "EventSetInformation")) {
      // winwrap answered. ERROR_NOT_SUPPORTED and deliberately not the
      // ERROR_INVALID_HANDLE above -- see src/etw.cc for why the two siblings
      // differ, which comes down to Vista and 7 having a real EventRegister
      // where XP has none.
      CHECK(info == ERROR_NOT_SUPPORTED,
            L"the fallback returned " << Hex(info) << L", expected ERROR_NOT_SUPPORTED");
      std::wcout << L"    no provider information to set here; winwrap answered" << std::endl;
    } else {
      std::wcout << L"    forwarded to the system's own EventSetInformation" << std::endl;
    }
    std::wcout << L"    unregistered handle -> " << Hex(info) << std::endl;
  }

  // --- Registry --------------------------------------------------------------

  // RegGetValueW is the one shim here that is a reimplementation rather than a
  // translation, so it gets a self-contained key rather than reading something
  // the machine happens to have. Everything is written under HKCU and removed
  // again, so the test needs no privilege and leaves nothing behind.
  void TestRegGetValue() {
    std::wcout << L"  RegGetValueW" << std::endl;

    wchar_t subkey[64] = {};
    ::wsprintfW(subkey, L"Software\\winwrap-test-%lu", ::GetCurrentProcessId());

    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr,
                          &key, nullptr) != ERROR_SUCCESS) {
      std::wcout << L"    could not create " << subkey << L"; skipped" << std::endl;
      return;
    }

    const wchar_t kText[]       = L"winwrap";
    const wchar_t kUnexpanded[] = L"%SystemRoot%\\explorer.exe";
    const DWORD kNumber         = 0xC0FFEE;
    // Double terminated, as a REG_MULTI_SZ is supposed to be.
    const wchar_t kMulti[] = L"one\0two\0";
    // Deliberately stored without its terminator, which is the case the
    // terminator handling exists for and the one it used to get wrong.
    const wchar_t kRaw[] = L"raw";

    ::RegSetValueExW(key, L"Text", 0, REG_SZ, reinterpret_cast<const BYTE*>(kText), sizeof(kText));
    ::RegSetValueExW(key, L"Number", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&kNumber),
                     sizeof(kNumber));
    ::RegSetValueExW(key, L"Expandable", 0, REG_EXPAND_SZ,
                     reinterpret_cast<const BYTE*>(kUnexpanded), sizeof(kUnexpanded));
    ::RegSetValueExW(key, L"Multi", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(kMulti),
                     sizeof(kMulti));
    ::RegSetValueExW(key, L"Unterminated", 0, REG_SZ, reinterpret_cast<const BYTE*>(kRaw),
                     3 * sizeof(wchar_t));

    // A plain string, with the type reported back.
    wchar_t text[64] = {};
    DWORD type       = 0;
    DWORD size       = sizeof(text);
    LSTATUS status   = ::RegGetValueW(key, nullptr, L"Text", RRF_RT_REG_SZ, &type, text, &size);
    CHECK(status == ERROR_SUCCESS, L"reading Text failed with " << status);
    CHECK(::wcscmp(text, kText) == 0, L"Text read back as \"" << text << L"\"");
    CHECK(type == REG_SZ, L"Text reported type " << type);
    CHECK(size == sizeof(kText), L"Text reported " << size << L" bytes, wrote " << sizeof(kText));

    // The type filter, which is the whole reason this function exists rather
    // than RegQueryValueExW.
    size   = sizeof(text);
    status = ::RegGetValueW(key, nullptr, L"Number", RRF_RT_REG_SZ, nullptr, text, &size);
    CHECK(status == ERROR_UNSUPPORTED_TYPE,
          L"asking for a string and getting a DWORD returned " << status);

    DWORD number = 0;
    size         = sizeof(number);
    status       = ::RegGetValueW(key, nullptr, L"Number", RRF_RT_REG_DWORD, &type, &number, &size);
    CHECK(status == ERROR_SUCCESS, L"reading Number failed with " << status);
    CHECK(number == kNumber, L"Number read back as " << Hex(number));
    CHECK(type == REG_DWORD, L"Number reported type " << type);

    // Environment expansion. RRF_RT_REG_SZ on a REG_EXPAND_SZ value is what
    // asks for it; RRF_NOEXPAND is what declines.
    wchar_t expanded[MAX_PATH] = {};
    size                       = sizeof(expanded);
    status = ::RegGetValueW(key, nullptr, L"Expandable", RRF_RT_REG_SZ, &type, expanded, &size);
    CHECK(status == ERROR_SUCCESS, L"reading Expandable failed with " << status);
    CHECK(::wcschr(expanded, L'%') == nullptr,
          L"Expandable still contains a %: \"" << expanded << L"\"");

    wchar_t literal[MAX_PATH] = {};
    size                      = sizeof(literal);
    status = ::RegGetValueW(key, nullptr, L"Expandable", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type,
                            literal, &size);
    CHECK(status == ERROR_SUCCESS, L"reading Expandable unexpanded failed with " << status);
    CHECK(::wcscmp(literal, kUnexpanded) == 0,
          L"RRF_NOEXPAND gave \"" << literal << L"\" rather than the stored string");
    std::wcout << L"    expanded \"" << literal << L"\" to \"" << expanded << L"\"" << std::endl;

    // A size query, which is a null buffer and a non-null size.
    //
    // The answer is the stored size *plus room for a terminator*, and not the
    // stored size, which is worth being explicit about because it looks like
    // an off-by-one. Nothing has read the data at this point, so neither
    // implementation can know whether it carries a terminator of its own, and
    // both have promised the caller a terminated string -- so both quote the
    // worst case. The real RegGetValueW does exactly the same; this is where
    // that promise is paid for. A read into a buffer large enough reports the
    // true size instead, which is what the check further up measures.
    const DWORD kPadded = static_cast<DWORD>(sizeof(kText) + sizeof(wchar_t));

    size   = 0;
    status = ::RegGetValueW(key, nullptr, L"Text", RRF_RT_REG_SZ, nullptr, nullptr, &size);
    CHECK(status == ERROR_SUCCESS, L"a size query returned " << status);
    CHECK(size == kPadded, L"a size query reported " << size << L" bytes, expected " << kPadded);

    // And a buffer that cannot hold the answer, which gets the same worst case
    // for the same reason.
    wchar_t tiny[2] = {};
    size            = sizeof(tiny);
    status          = ::RegGetValueW(key, nullptr, L"Text", RRF_RT_REG_SZ, nullptr, tiny, &size);
    CHECK(status == ERROR_MORE_DATA, L"a short buffer returned " << status);
    CHECK(size == kPadded,
          L"a short buffer reported " << size << L" bytes needed, expected " << kPadded);

    // Where the system has its own RegGetValueW, ours must agree with it, the
    // same way the GetErrorMode check above does. Ground truth rather than an
    // expectation written here, and the only thing that can say this
    // reimplementation matches what it reimplements.
    //
    // Through winwrap_RegGetValueFallbackW -- declared above this namespace,
    // because a linkage specification may only appear at namespace scope --
    // rather than ::RegGetValueW, because the latter hands over to advapi32 on
    // any OS that has it, which is every OS this test can run on. Calling the
    // public name here compared the system function with itself and could not
    // fail, on either side of the branch: identical pointers above XP, and the
    // whole block skipped on XP for want of a system_version to compare
    // against.
    using RegGetValueFunction =
        LSTATUS(APIENTRY*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    RegGetValueFunction system_version = nullptr;
    if (HMODULE advapi32 = ::GetModuleHandleW(L"advapi32.dll")) {
      system_version =
          reinterpret_cast<RegGetValueFunction>(::GetProcAddress(advapi32, "RegGetValueW"));
    }
    if (system_version) {
      struct Probe {
        const wchar_t* value;
        DWORD flags;
        DWORD cap; // bytes offered to the call; 0 means the whole buffer
      } probes[] = {
          {L"Text", RRF_RT_REG_SZ, 0},
          {L"Number", RRF_RT_REG_DWORD, 0},
          {L"Expandable", RRF_RT_REG_SZ, 0},
          {L"Expandable", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, 0},
          // REG_MULTI_SZ needs two terminators rather than one, and had no
          // coverage at all.
          {L"Multi", RRF_RT_REG_MULTI_SZ, 0},
          // A value stored without its terminator: the shim has to add one and
          // say so in the size it reports.
          {L"Unterminated", RRF_RT_REG_SZ, 0},
          // Buffers that are exactly the stored size, which is where the
          // terminator handling decides between success and ERROR_MORE_DATA --
          // the case that used to answer ERROR_MORE_DATA spuriously for an
          // already terminated value, and to over-report its size by one
          // character.
          {L"Text", RRF_RT_REG_SZ, sizeof(kText)},
          {L"Unterminated", RRF_RT_REG_SZ, 3 * sizeof(wchar_t)},
          {L"Multi", RRF_RT_REG_MULTI_SZ, sizeof(kMulti)},
          {L"Number", RRF_RT_REG_SZ, 0}, // the rejection path
          {L"Absent", RRF_RT_ANY, 0},    // the missing-value path
      };
      for (const Probe& probe : probes) {
        BYTE ours[MAX_PATH * sizeof(wchar_t)]   = {};
        BYTE theirs[MAX_PATH * sizeof(wchar_t)] = {};
        DWORD our_type = 0, their_type = 0;
        DWORD our_size   = probe.cap ? probe.cap : sizeof(ours);
        DWORD their_size = probe.cap ? probe.cap : sizeof(theirs);

        // The size query first: it is what a caller allocates from, so the two
        // disagreeing there is as wrong as disagreeing on the data.
        DWORD our_needed = 0, their_needed = 0;
        LSTATUS our_query = winwrap_RegGetValueFallbackW(key, nullptr, probe.value, probe.flags,
                                                         nullptr, nullptr, &our_needed);
        LSTATUS their_query =
            system_version(key, nullptr, probe.value, probe.flags, nullptr, nullptr, &their_needed);
        CHECK(our_query == their_query, L"sizing " << probe.value << L"/" << Hex(probe.flags)
                                                   << L" winwrap returned " << our_query
                                                   << L", the system " << their_query);
        if (our_query == ERROR_SUCCESS) {
          CHECK(our_needed == their_needed, L"sizing " << probe.value << L" winwrap wanted "
                                                       << our_needed << L" bytes, the system "
                                                       << their_needed);
        }

        LSTATUS our_status   = winwrap_RegGetValueFallbackW(key, nullptr, probe.value, probe.flags,
                                                            &our_type, ours, &our_size);
        LSTATUS their_status = system_version(key, nullptr, probe.value, probe.flags, &their_type,
                                              theirs, &their_size);

        CHECK(our_status == their_status, L"on " << probe.value << L"/" << Hex(probe.flags)
                                                 << L" winwrap returned " << our_status
                                                 << L", the system " << their_status);
        if (our_status == ERROR_SUCCESS && their_status == ERROR_SUCCESS) {
          CHECK(our_type == their_type, L"on " << probe.value << L" winwrap said type " << our_type
                                               << L", the system " << their_type);
          CHECK(our_size == their_size, L"on " << probe.value << L" winwrap said " << our_size
                                               << L" bytes, the system " << their_size);

          // The content, compared as whatever the type says it is. For a single
          // string that means up to the terminator and no further, because past
          // it neither implementation defines what the buffer holds -- and they
          // differ there. The real RegGetValueW reads the stored value into the
          // caller's buffer and expands in place, so for
          // %SystemRoot%\explorer.exe the bytes after the expanded terminator
          // still carry the tail of the unexpanded form, where winwrap leaves
          // them as the caller had them. Comparing those compares a leftover.
          // The reported size is checked exactly just above, which is the part
          // a caller can act on.
          //
          // REG_MULTI_SZ keeps the full comparison: its embedded terminators
          // are content, so stopping at the first would check almost nothing.
          if (our_type == REG_SZ || our_type == REG_EXPAND_SZ) {
            auto* ours_str   = reinterpret_cast<const wchar_t*>(ours);
            auto* theirs_str = reinterpret_cast<const wchar_t*>(theirs);
            CHECK(::wcscmp(ours_str, theirs_str) == 0,
                  L"on " << probe.value << L" winwrap gave \"" << Printable(ours_str)
                         << L"\", the system \"" << Printable(theirs_str) << L"\"");
          } else {
            CHECK(::memcmp(ours, theirs, our_size < their_size ? our_size : their_size) == 0,
                  L"on " << probe.value << L" the two disagreed on the data");
          }
        }
      }
      std::wcout << L"    agrees with the system's own RegGetValueW" << std::endl;
    } else {
      std::wcout << L"    no system RegGetValueW to compare against" << std::endl;
    }

    ::RegCloseKey(key);
    ::RegDeleteKeyW(HKEY_CURRENT_USER, subkey);
  }

  // --- DPI awareness ---------------------------------------------------------

  void TestDpiAware() {
    std::wcout << L"  SetProcessDPIAware" << std::endl;

    // This one changes the calling process for good, and cannot be undone --
    // so it runs last of the process-wide tests, and nothing after it may
    // depend on the old state. Harmless here: this is a console program with
    // no windows to be scaled.
    const BOOL result = ::SetProcessDPIAware();

    if (::GetModuleHandleW(L"user32.dll") != nullptr &&
        SystemHas(L"user32.dll", "SetProcessDPIAware")) {
      // Forwarded. FALSE is a legitimate answer: the real function refuses if
      // awareness has already been set, which an application manifest may well
      // have done before main() ran. So the only wrong outcome is a crash.
      std::wcout << L"    forwarded to the system -> " << (result ? L"TRUE" : L"FALSE")
                 << std::endl;
    } else {
      // winwrap answered, and it promises success: XP has no DPI
      // virtualisation to opt out of, so the state it is asked for already
      // holds.
      CHECK(result, L"the fallback reported failure");
      std::wcout << L"    no system SetProcessDPIAware; winwrap reported success" << std::endl;
    }
  }

  // --- File information by handle --------------------------------------------

  // Makes a temp file holding `bytes` bytes. Returns the handle and writes out
  // the path. `flags` is for the one caller that needs FILE_FLAG_OVERLAPPED,
  // which must then ask for no content: WriteFile on an overlapped handle with
  // a null OVERLAPPED is not a valid call.
  HANDLE MakeTempFile(wchar_t* path_out, DWORD bytes, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
    wchar_t directory[MAX_PATH] = {};
    if (::GetTempPathW(ARRAYSIZE(directory), directory) == 0) {
      return INVALID_HANDLE_VALUE;
    }
    if (::GetTempFileNameW(directory, L"wwt", 0, path_out) == 0) {
      return INVALID_HANDLE_VALUE;
    }

    HANDLE file = ::CreateFileW(path_out, GENERIC_READ | GENERIC_WRITE | DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return file;
    }
    if (bytes != 0) {
      auto* payload = static_cast<BYTE*>(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
      DWORD written = 0;
      ::WriteFile(file, payload, bytes, &written, nullptr);
      ::FlushFileBuffers(file);
      ::HeapFree(::GetProcessHeap(), 0, payload);
    }
    return file;
  }

  void TestFileInformationQuery() {
    std::wcout << L"  GetFileInformationByHandleEx" << std::endl;

    wchar_t path[MAX_PATH] = {};
    HANDLE file            = MakeTempFile(path, 123);
    CHECK(file != INVALID_HANDLE_VALUE,
          L"could not create a temp file (" << ::GetLastError() << L")");
    if (file == INVALID_HANDLE_VALUE) {
      return;
    }

    FILE_STANDARD_INFO standard = {};
    CHECK(::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)),
          L"FileStandardInfo failed (" << ::GetLastError() << L")");
    CHECK(standard.EndOfFile.QuadPart == 123,
          L"size is " << (standard.EndOfFile.QuadPart) << L", expected 123");
    CHECK(!standard.Directory, L"a file reported itself as a directory");

    FILE_BASIC_INFO basic = {};
    CHECK(::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)),
          L"FileBasicInfo failed (" << ::GetLastError() << L")");
    CHECK(basic.FileAttributes != 0, L"no attributes reported");
    CHECK(basic.LastWriteTime.QuadPart != 0, L"no write time reported");

    // FileNameInfo is variable length and needs more than the bare struct.
    BYTE name_buffer[sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(WCHAR)] = {};
    auto* name = reinterpret_cast<FILE_NAME_INFO*>(name_buffer);
    CHECK(::GetFileInformationByHandleEx(file, FileNameInfo, name, sizeof(name_buffer)),
          L"FileNameInfo failed (" << ::GetLastError() << L")");
    CHECK(name->FileNameLength > 0, L"an empty name came back");

    FILE_ATTRIBUTE_TAG_INFO tag = {};
    CHECK(::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)),
          L"FileAttributeTagInfo failed (" << ::GetLastError() << L")");
    CHECK(tag.FileAttributes == basic.FileAttributes, L"attributes disagree between classes: "
                                                          << (tag.FileAttributes) << L" vs "
                                                          << (basic.FileAttributes));

    // Windows 8 added this one; below that it is assembled from the volume
    // serial number and the index number, both of which must be populated.
    FILE_ID_INFO id = {};
    if (::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) {
      CHECK(id.VolumeSerialNumber != 0, L"a zero volume serial number");
      bool any = false;
      for (size_t i = 0; i < sizeof(id.FileId.Identifier); ++i) {
        any = any || id.FileId.Identifier[i] != 0;
      }
      CHECK(any, L"an all-zero file ID");
    } else {
      std::wcout << L"    FileIdInfo unavailable (" << ::GetLastError() << L")" << std::endl;
    }

    // A buffer too small to hold the answer, and a class that does not exist.
    // Both must be refused rather than half-filled.
    BYTE tiny[2] = {};
    CHECK(!::GetFileInformationByHandleEx(file, FileStandardInfo, tiny, sizeof(tiny)),
          L"an undersized buffer was accepted");
    CHECK(!::GetFileInformationByHandleEx(file, static_cast<FILE_INFO_BY_HANDLE_CLASS>(99),
                                          &standard, sizeof(standard)),
          L"an unknown information class was accepted");
    CHECK(
        ::GetLastError() == ERROR_INVALID_PARAMETER,
        L"unknown class gave error " << ::GetLastError() << L", expected ERROR_INVALID_PARAMETER");

    ::CloseHandle(file);
    ::DeleteFileW(path);
  }

  void TestFileInformationSet() {
    std::wcout << L"  SetFileInformationByHandle" << std::endl;

    wchar_t path[MAX_PATH] = {};
    HANDLE file            = MakeTempFile(path, 500);
    if (file == INVALID_HANDLE_VALUE) {
      CHECK(false, L"could not create a temp file (" << ::GetLastError() << L")");
      return;
    }

    FILE_END_OF_FILE_INFO eof = {};
    eof.EndOfFile.QuadPart    = 10;
    CHECK(::SetFileInformationByHandle(file, FileEndOfFileInfo, &eof, sizeof(eof)),
          L"FileEndOfFileInfo failed (" << ::GetLastError() << L")");
    FILE_STANDARD_INFO standard = {};
    ::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard));
    CHECK(standard.EndOfFile.QuadPart == 10,
          L"truncation left " << (standard.EndOfFile.QuadPart) << L" bytes");

    FILE_BASIC_INFO basic = {};
    ::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY;
    CHECK(::SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic)),
          L"FileBasicInfo set failed (" << ::GetLastError() << L")");

    CHECK(!::SetFileInformationByHandle(file, static_cast<FILE_INFO_BY_HANDLE_CLASS>(99), &basic,
                                        sizeof(basic)),
          L"an unknown set class was accepted");

    // Delete-on-close through the disposition class, which is what libuv's
    // process code uses to discard a crash dump it could not finish writing.
    FILE_DISPOSITION_INFO disposition = {};
    disposition.DeleteFile            = TRUE;
    CHECK(
        ::SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)),
        L"FileDispositionInfo failed (" << ::GetLastError() << L")");
    ::CloseHandle(file);
    CHECK(::GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES,
          L"the file survived a delete-on-close disposition");
    ::DeleteFileW(path);
  }

  void TestFinalPathAndReOpen() {
    std::wcout << L"  GetFinalPathNameByHandleW, ReOpenFile" << std::endl;

    wchar_t path[MAX_PATH] = {};
    HANDLE file            = MakeTempFile(path, 8);
    if (file == INVALID_HANDLE_VALUE) {
      CHECK(false, L"could not create a temp file (" << ::GetLastError() << L")");
      return;
    }

    // The documented two-call shape: with no buffer it returns the length
    // including the terminator, and callers size their buffer from that.
    DWORD needed = ::GetFinalPathNameByHandleW(file, nullptr, 0, VOLUME_NAME_DOS);
    CHECK(needed > 0, L"length probe failed (" << ::GetLastError() << L")");
    if (needed > 0) {
      auto* buffer = static_cast<wchar_t*>(
          ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, (needed + 1) * sizeof(wchar_t)));
      DWORD written = ::GetFinalPathNameByHandleW(file, buffer, needed, VOLUME_NAME_DOS);
      CHECK(written > 0 && written < needed,
            L"second call returned " << written << L" against a probe of " << needed);
      CHECK(::wcsncmp(buffer, L"\\\\?\\", 4) == 0,
            L"the DOS form is not prefixed with \\\\?\\: \"" << buffer << L"\"");
      std::wcout << L"    final path: " << buffer << std::endl;
      ::HeapFree(::GetProcessHeap(), 0, buffer);
    }

    wchar_t nt_path[1024] = {};
    DWORD nt_written =
        ::GetFinalPathNameByHandleW(file, nt_path, ARRAYSIZE(nt_path), VOLUME_NAME_NT);
    CHECK(nt_written > 0, L"VOLUME_NAME_NT failed (" << ::GetLastError() << L")");
    if (nt_written > 0) {
      CHECK(nt_path[0] == L'\\', L"an NT path not starting with a separator");
    }

    HANDLE reopened = ::ReOpenFile(file, FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    CHECK(reopened != INVALID_HANDLE_VALUE,
          L"ReOpenFile on a file failed (" << ::GetLastError() << L")");
    if (reopened != INVALID_HANDLE_VALUE) {
      ::CloseHandle(reopened);
    }

    ::CloseHandle(file);
    ::DeleteFileW(path);

    // A directory, which is the case that breaks if the implementation forbids
    // directories outright. FILE_FLAG_BACKUP_SEMANTICS is how a caller says the
    // handle might be one, and libuv depends on it to reopen a directory for
    // FILE_WRITE_ATTRIBUTES.
    //
    // Only assertable where winwrap is the one answering. The system's own
    // ReOpenFile refuses this outright -- ERROR_ACCESS_DENIED on Windows
    // Server 2022, which is not what the documented flag list predicts, since
    // FILE_FLAG_BACKUP_SEMANTICS is not in it. winwrap's NtCreateFile path
    // does honour it, by asking for FILE_OPEN_FOR_BACKUP_INTENT rather than
    // FILE_NON_DIRECTORY_FILE, so this is a case where the emulation is the
    // more capable of the two and the test cannot demand the system match it.
    wchar_t directory[MAX_PATH] = {};
    ::GetTempPathW(ARRAYSIZE(directory), directory);
    HANDLE dir = ::CreateFileW(directory, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir != INVALID_HANDLE_VALUE) {
      HANDLE dir_again = ::ReOpenFile(dir, FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      FILE_FLAG_BACKUP_SEMANTICS);
      // Before SystemHas, whose GetProcAddress would clobber it.
      const DWORD reopen_error = ::GetLastError();
      if (!SystemHas(L"kernel32.dll", "ReOpenFile")) {
        CHECK(dir_again != INVALID_HANDLE_VALUE,
              L"ReOpenFile could not reopen a directory (" << reopen_error << L")");
      } else if (dir_again != INVALID_HANDLE_VALUE) {
        std::wcout << L"    the system's ReOpenFile reopened a directory" << std::endl;
      } else {
        std::wcout << L"    the system's ReOpenFile refuses a directory (" << reopen_error
                   << L"); winwrap's own path allows it" << std::endl;
      }
      if (dir_again != INVALID_HANDLE_VALUE) {
        ::CloseHandle(dir_again);
      }
      ::CloseHandle(dir);
    }
  }

  // --- Named pipes -----------------------------------------------------------

  void TestNamedPipes() {
    std::wcout << L"  Named pipe process IDs" << std::endl;

    wchar_t name[128] = {};
    ::wsprintfW(name, L"\\\\.\\pipe\\winwrap-test-%lu", ::GetCurrentProcessId());

    HANDLE server = ::CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512,
                                       512, 0, nullptr);
    CHECK(server != INVALID_HANDLE_VALUE, L"CreateNamedPipeW failed (" << ::GetLastError() << L")");
    if (server == INVALID_HANDLE_VALUE) {
      return;
    }

    HANDLE client =
        ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(client != INVALID_HANDLE_VALUE,
          L"connecting to the pipe failed (" << ::GetLastError() << L")");

    // Both ends belong to this process, so any answer at all must be this
    // process. Where the information is unavailable the call has to fail
    // cleanly rather than invent one: a wrong process ID here would defeat the
    // privilege check Mojo builds on top of it.
    const DWORD self = ::GetCurrentProcessId();
    ULONG pid        = 0;
    if (::GetNamedPipeServerProcessId(server, &pid)) {
      CHECK(pid == self, L"server process ID is " << pid << L", expected " << self);
    } else {
      CHECK(::GetLastError() == ERROR_NOT_SUPPORTED,
            L"server ID failed with " << ::GetLastError() << L", expected ERROR_NOT_SUPPORTED");
      std::wcout << L"    peer process IDs unavailable, as expected before Vista" << std::endl;
    }

    if (client != INVALID_HANDLE_VALUE) {
      pid = 0;
      if (::GetNamedPipeClientProcessId(server, &pid)) {
        CHECK(pid == self, L"client process ID is " << pid << L", expected " << self);
      } else {
        CHECK(::GetLastError() == ERROR_NOT_SUPPORTED,
              L"client ID failed with " << ::GetLastError() << L", expected ERROR_NOT_SUPPORTED");
      }
      ::CloseHandle(client);
    }

    // A null out parameter has to be refused. *Which* refusal differs, and the
    // test takes either: winwrap answers ERROR_INVALID_PARAMETER, while the
    // system treats it as a zero-length output buffer and answers
    // ERROR_INSUFFICIENT_BUFFER. The part that matters to a caller -- that it
    // fails rather than writing through the null -- is the same.
    CHECK(!::GetNamedPipeServerProcessId(server, nullptr), L"a null out parameter was accepted");
    const DWORD null_error = ::GetLastError();
    CHECK(null_error == ERROR_INVALID_PARAMETER || null_error == ERROR_INSUFFICIENT_BUFFER,
          L"a null out parameter gave " << null_error
                                        << L", expected ERROR_INVALID_PARAMETER or "
                                           L"ERROR_INSUFFICIENT_BUFFER");

    ::CloseHandle(server);
  }

  // --- Threads, error mode, restart ------------------------------------------

  void TestProcessorNumber() {
    std::wcout << L"  GetCurrentProcessorNumber" << std::endl;

    SYSTEM_INFO info = {};
    ::GetSystemInfo(&info);

    // Pinning the thread makes the answer stable, which is the only property
    // worth asserting: the numbering itself comes from firmware.
    DWORD_PTR previous = ::SetThreadAffinityMask(::GetCurrentThread(), 1);
    DWORD first        = ::GetCurrentProcessorNumber();
    DWORD second       = ::GetCurrentProcessorNumber();
    if (previous != 0) {
      ::SetThreadAffinityMask(::GetCurrentThread(), previous);
    }

    CHECK(first == second,
          L"pinned to one processor but reported " << first << L" then " << second);
    std::wcout << L"    reported processor " << first << L" of " << (info.dwNumberOfProcessors)
               << std::endl;
    if (first >= info.dwNumberOfProcessors) {
      // Not a failure: the APIC IDs this derives from need not be dense.
      std::wcout << L"    note: above the processor count, so IDs are sparse here" << std::endl;
    }
  }

#ifdef WINWRAP_DLL
  // Reached through GetProcAddress rather than by linking the name, which is
  // not a stylistic choice: shlwapi.h says so where it declares this protocol,
  // in as many words -- "The caller should always GetProcAddress(
  // \"DllGetVersion\"), not implicitly link to it." A dozen system DLLs export
  // the name, so going through a module handle is the only spelling that says
  // whose. It is also why winwrap_public.h does not declare it.
  //
  // DLLGETVERSIONPROC is the SDK's own typedef for the pointer, beside the
  // structures, so there is no hand-written signature here to drift from it.
  void TestDllGetVersion(HMODULE dll) {
    std::wcout << L"  DllGetVersion" << std::endl;

    const auto get_version =
        reinterpret_cast<DLLGETVERSIONPROC>(::GetProcAddress(dll, "DllGetVersion"));
    CHECK(get_version != nullptr, L"winwrap.dll does not export DllGetVersion");
    if (get_version == nullptr) {
      return;
    }

    // Null first: there is nowhere to report a size through, so this cannot be
    // folded into the cbSize handling below.
    const HRESULT null_hr = get_version(nullptr);
    CHECK(null_hr == E_POINTER,
          L"a null DLLVERSIONINFO returned " << Hex(null_hr) << L" rather than E_POINTER");

    // DLLVERSIONINFO2, the larger of the two shapes. cbSize lives in the
    // embedded info1, and the implementation fills the extended fields and
    // then falls through to the base ones, so one call answers for both.
    DLLVERSIONINFO2 info2 = {};
    info2.info1.cbSize    = sizeof(info2);
    const HRESULT hr2     = get_version(&info2.info1);

    std::wcout << L"    DLLVERSIONINFO2 -> " << Hex(hr2) << L" ullVersion "
               << Hex(info2.ullVersion) << L" dwFlags " << Hex(info2.dwFlags) << std::endl;
    std::wcout << L"    version " << info2.info1.dwMajorVersion << L"."
               << info2.info1.dwMinorVersion << L"." << info2.info1.dwBuildNumber
               << L" platform " << Hex(info2.info1.dwPlatformID) << std::endl;

    CHECK(hr2 == S_OK, L"the DLLVERSIONINFO2 call returned " << Hex(hr2));
    if (hr2 == S_OK) {
      // MAKEDLLVERULL packs major, minor, build and a fourth field winwrap has
      // no component for, which is why the last argument is a literal zero
      // here as it is in winwrap_dll.cc.
      const ULONGLONG expect = MAKEDLLVERULL(WINWRAP_MAJOR_VERSION, WINWRAP_MINOR_VERSION,
                                             WINWRAP_BUILD_VERSION, 0);
      CHECK(info2.ullVersion == expect, L"ullVersion is " << Hex(info2.ullVersion)
                                                          << L" but this test's headers pack "
                                                          << Hex(expect));
      CHECK(info2.info1.dwMajorVersion == WINWRAP_MAJOR_VERSION &&
                info2.info1.dwMinorVersion == WINWRAP_MINOR_VERSION &&
                info2.info1.dwBuildNumber == WINWRAP_BUILD_VERSION,
            L"the base fields disagree with this test's headers");
      CHECK(info2.info1.dwPlatformID == DLLVER_PLATFORM_NT,
            L"dwPlatformID is " << Hex(info2.info1.dwPlatformID)
                                << L" rather than DLLVER_PLATFORM_NT");
    }

    // DLLVERSIONINFO on its own: the backward-compatible shape a caller that
    // predates the extended one passes, and it has to report the same numbers.
    DLLVERSIONINFO info1 = {};
    info1.cbSize         = sizeof(info1);
    const HRESULT hr1    = get_version(&info1);
    CHECK(hr1 == S_OK, L"the DLLVERSIONINFO call returned " << Hex(hr1));
    if (hr1 == S_OK && hr2 == S_OK) {
      CHECK(info1.dwMajorVersion == info2.info1.dwMajorVersion &&
                info1.dwMinorVersion == info2.info1.dwMinorVersion &&
                info1.dwBuildNumber == info2.info1.dwBuildNumber &&
                info1.dwPlatformID == info2.info1.dwPlatformID,
            L"the two structure sizes report different versions");
    }

    // Neither size. The switch has no case for it, so this is the only path
    // that reaches the function's default.
    DLLVERSIONINFO odd = {};
    odd.cbSize         = sizeof(odd) + 1;
    const HRESULT odd_hr = get_version(&odd);
    CHECK(odd_hr == E_INVALIDARG,
          L"an unrecognised cbSize returned " << Hex(odd_hr) << L" rather than E_INVALIDARG");
  }
#endif // WINWRAP_DLL

  void TestWinWrapApi() {
    std::wcout << L"  GetWinWrapVersion" << std::endl;

    // Seeded with a value no version component can be, rather than with 0,
    // because WINWRAP_MAJOR_VERSION is itself 0 -- so zero-initialising would
    // let "wrote nothing at all" pass the major-version check below by
    // accident.
    constexpr unsigned int kUnset = 0xFFFFFFFFu;
    unsigned int major = kUnset, minor = kUnset, build = kUnset;
    const wchar_t* linked = GetWinWrapVersion(&major, &minor, &build);

    CHECK(linked != nullptr, L"GetWinWrapVersion returned no string");
    if (linked == nullptr) {
      return;
    }
    std::wcout << L"    linked winwrap v" << linked << std::endl;

    // Not a tautology, because kWinWrapVer comes from the winwrap_version.h
    // this test compiled against while the string comes out of the library
    // binary. A mismatch is a winwrap.lib or winwrap.dll from a different
    // checkout than the headers beside it, which is otherwise silent until
    // some shim behaves in a way its documentation does not describe.
    CHECK(kWinWrapVer == linked, L"the linked winwrap reports v"
                                     << linked << L" but this test was built against v"
                                     << kWinWrapVer);
    CHECK(major == WINWRAP_MAJOR_VERSION && minor == WINWRAP_MINOR_VERSION &&
              build == WINWRAP_BUILD_VERSION,
          L"numbers " << major << L"." << minor << L"." << build << L" disagree with the header's "
                      << WINWRAP_MAJOR_VERSION << L"." << WINWRAP_MINOR_VERSION << L"."
                      << WINWRAP_BUILD_VERSION);

    // Every out parameter is documented as optional, so all three omitted has
    // to work rather than fault.
    CHECK(GetWinWrapVersion(nullptr, nullptr, nullptr) != nullptr,
          L"the string is not returned when no numbers are asked for");

    // Which form of winwrap this is linked against, and -- more usefully --
    // that the two ways of knowing agree. WINWRAP_DLL is what the build said
    // at compile time; winwrap.dll being in the process is what is actually
    // true. A disagreement means the library form and the test's defines were
    // chosen independently, and the half of the coverage that only exists in
    // one form was silently skipped.
    const HMODULE dll = ::GetModuleHandleW(L"winwrap.dll");
#ifdef WINWRAP_DLL
    std::wcout << L"    linked form: winwrap.dll (shared)" << std::endl;
    CHECK(dll != nullptr,
          L"built against the shared winwrap, but winwrap.dll is not loaded in this process");
    if (dll != nullptr) {
      TestDllGetVersion(dll);
    }
#else
    std::wcout << L"    linked form: winwrap.lib (static)" << std::endl;
    // Nothing builds winwrap.dll when the static form is selected, so its
    // presence here means WINWRAP_DLL was not passed to a test that is in fact
    // running against the DLL -- the exact wiring mistake that leaves
    // DllGetVersion untested while the run reports success.
    CHECK(dll == nullptr,
          L"winwrap.dll is loaded but WINWRAP_DLL was not defined for this test, so "
          L"DllGetVersion went untested -- check is_component_winwrap in test/BUILD.gn "
          L"or FORMDEF in the Makefile");
#endif // WINWRAP_DLL
  }

  // Set bits in an affinity mask, for comparing against what the topology
  // entries claim to cover.
  DWORD CountBits(DWORD_PTR mask) {
    DWORD n = 0;
    for (; mask != 0; mask &= mask - 1)
      ++n;
    return n;
  }

  void TestLogicalProcessorInformation() {
    std::wcout << L"  GetLogicalProcessorInformation" << std::endl;

    using Entry = SYSTEM_LOGICAL_PROCESSOR_INFORMATION;

    // A null length is rejected before anything else is looked at.
    CHECK(!::GetLogicalProcessorInformation(nullptr, nullptr),
          L"a null ReturnedLength was accepted");

    // The documented two-step call. A zero length must fail, must say how much
    // is needed, and that figure must be a whole number of entries.
    DWORD length = 0;
    BOOL ok      = ::GetLogicalProcessorInformation(nullptr, &length);
    DWORD err    = ok ? ERROR_SUCCESS : ::GetLastError();

    CHECK(!ok, L"the zero-length probe succeeded, leaving no length to use");
    CHECK(err == ERROR_INSUFFICIENT_BUFFER,
          L"probe reported " << err << L" rather than ERROR_INSUFFICIENT_BUFFER");
    CHECK(length > 0, L"probe asked for a zero-byte buffer");
    CHECK(length % sizeof(Entry) == 0,
          L"required length " << length << L" is not a multiple of the entry size "
                              << sizeof(Entry));

    if (length == 0 || length % sizeof(Entry) != 0) {
      return; // Nothing trustworthy to allocate from.
    }

    // std::vector<Entry> rather than of bytes: the entries carry pointer-sized
    // members, and a byte vector guarantees only one-byte alignment.
    std::vector<Entry> entries(length / sizeof(Entry));
    DWORD filled = length;
    ok           = ::GetLogicalProcessorInformation(entries.data(), &filled);

    CHECK(ok, L"the sized call failed with " << ::GetLastError());
    if (!ok) {
      return;
    }
    CHECK(filled <= length,
          L"filled " << filled << L" bytes into a " << length << L" byte buffer");
    CHECK(filled % sizeof(Entry) == 0,
          L"returned length " << filled << L" is not a whole number of entries");

    entries.resize(filled / sizeof(Entry));

    // Every entry's mask must be a subset of the active processors, and the
    // union of the core entries must cover all of them. That is the part the
    // fallback in system.cc has to get right: it is what a caller counting
    // processors actually reads.
    SYSTEM_INFO info = {};
    ::GetSystemInfo(&info);

    DWORD_PTR covered = 0;
    DWORD cores = 0, caches = 0, packages = 0, nodes = 0, other = 0;
    for (const Entry& e : entries) {
      switch (e.Relationship) {
        case RelationProcessorCore:
          ++cores;
          covered |= e.ProcessorMask;
          break;
        case RelationCache:
          ++caches;
          break;
        case RelationProcessorPackage:
          ++packages;
          break;
        case RelationNumaNode:
          ++nodes;
          break;
        default:
          ++other;
          break;
      }
      CHECK((e.ProcessorMask & ~info.dwActiveProcessorMask) == 0,
            L"an entry names processors outside the active mask: entry 0x"
                << std::hex << e.ProcessorMask << L" active 0x"
                << info.dwActiveProcessorMask << std::dec);
    }

    CHECK(cores > 0, L"no RelationProcessorCore entries at all");
    CHECK(covered == info.dwActiveProcessorMask,
          L"core entries cover 0x" << std::hex << covered << L" but the active mask is 0x"
                                   << info.dwActiveProcessorMask << std::dec);

    std::wcout << L"    " << entries.size() << L" entries: " << cores << L" core, " << caches
               << L" cache, " << packages << L" package, " << nodes << L" numa";
    if (other != 0) {
      std::wcout << L", " << other << L" other";
    }
    std::wcout << std::endl;

    // Which implementation answered, and so which result was just checked.
    // Below XP SP3 it is winwrap's, which reports no caches and no SMT -- see
    // system.cc. Saying so keeps a thin-looking result on an old machine from
    // reading as a failure.
    const bool is_system_impl = SystemHas(L"kernel32.dll", "GetLogicalProcessorInformation");
    if (is_system_impl) {
      std::wcout << L"    answered by kernel32" << std::endl;
    } else {
      std::wcout << L"    answered by winwrap: counts exact, topology approximated"
                 << std::endl;
      CHECK(caches == 0,
            L"the fallback reported " << caches << L" cache entries, which it cannot know");
      CHECK(cores == CountBits(info.dwActiveProcessorMask),
            L"the fallback reported " << cores << L" cores for "
                                      << CountBits(info.dwActiveProcessorMask)
                                      << L" active processors");
    }
  }

  void TestErrorModeAndRestart() {
    std::wcout << L"  GetErrorMode, RegisterApplicationRestart" << std::endl;

    const UINT original = ::GetErrorMode();

    // Some bits are the system's to decide and cannot be cleared. On x86 and
    // x64 the kernel always fixes alignment faults, so SEM_NOALIGNMENTFAULTEXCEPT
    // reads back set whatever was asked for, and attempts to clear it are
    // ignored. Which bits behave that way differs by Windows version -- XP
    // reports none -- so rather than hard-code a list, ask: whatever survives
    // setting zero is what the system insists on.
    ::SetErrorMode(0);
    const UINT sticky = ::GetErrorMode();
    if (sticky != 0) {
      std::wcout << L"    the system holds " << Hex(sticky) << L" set regardless" << std::endl;
    }

    // A round trip through SetErrorMode is what catches an inverted bit, which
    // is the one way this is easy to get wrong.
    const UINT wanted = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX;
    ::SetErrorMode(wanted);
    CHECK(::GetErrorMode() == (wanted | sticky),
          L"set " << Hex(wanted) << L", read back " << Hex(::GetErrorMode()));

    ::SetErrorMode(SEM_NOOPENFILEERRORBOX);
    CHECK(::GetErrorMode() == (SEM_NOOPENFILEERRORBOX | sticky),
          L"set " << Hex(SEM_NOOPENFILEERRORBOX) << L", read back " << Hex(::GetErrorMode()));

    ::SetErrorMode(0);
    CHECK(::GetErrorMode() == sticky, L"set 0, read back " << Hex(::GetErrorMode()));

    // Where the system has its own GetErrorMode, ours must agree with it
    // exactly. That is ground truth rather than an expectation written here,
    // and it is the only check in this file that can say the emulation matches
    // the thing it emulates. Absent before Vista, where it simply does not run.
    using GetErrorModeFunction          = UINT(WINAPI*)();
    GetErrorModeFunction system_version = nullptr;
    if (HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll")) {
      system_version =
          reinterpret_cast<GetErrorModeFunction>(::GetProcAddress(kernel32, "GetErrorMode"));
    }
    if (system_version) {
      const UINT probes[] = {0, SEM_FAILCRITICALERRORS,
                             SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, wanted};
      for (UINT probe : probes) {
        ::SetErrorMode(probe);
        CHECK(::GetErrorMode() == system_version(),
              L"after setting " << Hex(probe) << L" winwrap says " << Hex(::GetErrorMode())
                                << L", the system says " << Hex(system_version()));
      }
      std::wcout << L"    agrees with the system's own GetErrorMode" << std::endl;
    } else {
      std::wcout << L"    no system GetErrorMode to compare against" << std::endl;
    }

    ::SetErrorMode(original);

    // The Restart Manager, which XP has no part of. winwrap must fail rather
    // than claim a registration it cannot honour -- a caller that believes it
    // will be restarted and is not has lost whatever it was relying on that
    // for. Only assertable where winwrap is the one answering: from Vista the
    // call is forwarded and really does register, so there the test undoes it
    // and says nothing about the result.
    HRESULT hr = ::RegisterApplicationRestart(L"--restarted", 0);
    if (!SystemHas(L"kernel32.dll", "RegisterApplicationRestart")) {
      CHECK(FAILED(hr), L"RegisterApplicationRestart reported success (" << Hex(hr) << L")");
      hr = ::UnregisterApplicationRestart();
      CHECK(FAILED(hr), L"UnregisterApplicationRestart reported success (" << Hex(hr) << L")");
    } else {
      CHECK(SUCCEEDED(hr), L"the system's RegisterApplicationRestart failed (" << Hex(hr) << L")");
      hr = ::UnregisterApplicationRestart();
      CHECK(SUCCEEDED(hr),
            L"the system's UnregisterApplicationRestart failed (" << Hex(hr) << L")");
      std::wcout << L"    forwarded to the system's own Restart Manager" << std::endl;
    }
  }

  // --- Directories -----------------------------------------------------------

  void TestDirectories() {
    std::wcout << L"  CreateSymbolicLinkW, NeedCurrentDirectoryForExePathW" << std::endl;

    wchar_t directory[MAX_PATH] = {};
    ::GetTempPathW(ARRAYSIZE(directory), directory);
    wchar_t link[MAX_PATH] = {};
    ::wsprintfW(link, L"%swinwrap-link-%lu", directory, ::GetCurrentProcessId());

    if (::CreateSymbolicLinkW(link, directory, SYMBOLIC_LINK_FLAG_DIRECTORY)) {
      std::wcout << L"    symbolic links are available here" << std::endl;
      ::RemoveDirectoryW(link);
    } else {
      std::wcout << L"    symbolic link refused (" << ::GetLastError()
                 << L"), as expected without support" << std::endl;
    }

    // A name that already carries a path never consults the environment.
    CHECK(::NeedCurrentDirectoryForExePathW(L"dir\\program.exe"),
          L"a path-qualified name was refused the current directory");

    const wchar_t kVariable[] = L"NoDefaultCurrentDirectoryInExePath";
    const bool had            = ::GetEnvironmentVariableW(kVariable, nullptr, 0) != 0;

    ::SetEnvironmentVariableW(kVariable, nullptr);
    CHECK(::NeedCurrentDirectoryForExePathW(L"program.exe"),
          L"the current directory was refused with the variable unset");

    ::SetEnvironmentVariableW(kVariable, L"1");
    CHECK(!::NeedCurrentDirectoryForExePathW(L"program.exe"),
          L"the current directory was allowed with the variable set");
    CHECK(::NeedCurrentDirectoryForExePathW(L"dir\\program.exe"),
          L"a path-qualified name was affected by the variable");

    // Any value counts as set, including an empty one -- but only if the system
    // kept the variable at all. Setting an environment variable to an empty
    // string removes it outright on some Windows versions, which would make this
    // a test of SetEnvironmentVariableW rather than of the function under test,
    // so check the premise before asserting on it.
    ::SetEnvironmentVariableW(kVariable, L"");
    ::SetLastError(ERROR_SUCCESS);
    DWORD probe = ::GetEnvironmentVariableW(kVariable, nullptr, 0);
    if (probe != 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
      CHECK(!::NeedCurrentDirectoryForExePathW(L"program.exe"),
            L"an empty value did not count as set");
    } else {
      std::wcout << L"    an empty value removes the variable here; skipped" << std::endl;
    }

    if (!had) {
      ::SetEnvironmentVariableW(kVariable, nullptr);
    }
  }

  // --- Fibers ----------------------------------------------------------------

  DWORD WINAPI FiberBody(LPVOID) {
    CHECK(!::IsThreadAFiber(), L"a fresh worker thread claimed to be a fiber");

    LPVOID fiber = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    CHECK(fiber != nullptr, L"ConvertThreadToFiberEx failed (" << ::GetLastError() << L")");
    if (fiber == nullptr) {
      return 0;
    }

    CHECK(::IsThreadAFiber(), L"a converted thread did not report as a fiber");
    CHECK(::ConvertFiberToThread(), L"ConvertFiberToThread failed (" << ::GetLastError() << L")");
    CHECK(!::IsThreadAFiber(), L"a thread converted back still reports as a fiber");
    return 0;
  }

  // The sequence Electron's wWinMain actually performs on 32-bit, reproduced
  // here because every step of it depends on IsThreadAFiber being right and the
  // consequence of being wrong is invisible.
  //
  // wWinMain asks IsThreadAFiber() on the main thread; a false "yes" makes it
  // skip enlarging the stack and run Chromium startup in the default 1 MiB,
  // which overflows somewhere inside initialisation with no window and no log.
  // A false "no" on the big fiber makes the recursive entry try to convert a
  // thread that is already a fiber.
  struct SwitchState {
    LPVOID original;
    bool reported_as_fiber;
    bool ran;
  };

  VOID CALLBACK BigStackFiber(PVOID parameter) {
    auto* state = static_cast<SwitchState*>(parameter);
    state->ran  = true;
    // This is the check that matters: running on a fiber created by
    // CreateFiberEx and entered by SwitchToFiber, not merely a converted thread.
    state->reported_as_fiber = ::IsThreadAFiber() != FALSE;
    ::SwitchToFiber(state->original);
  }

  void TestMainThreadFiberSequence() {
    std::wcout << L"  Fiber state on the main thread" << std::endl;

    // Raw, because the answer is derived from it and the two systems disagreeing
    // here is what any difference in behaviour would come down to.
    std::wcout << L"    GetCurrentFiber() on the main thread = " << ::GetCurrentFiber()
               << std::endl;
    CHECK(!::IsThreadAFiber(), L"the main thread claimed to be a fiber before any conversion");

    LPVOID original = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    CHECK(original != nullptr,
          L"ConvertThreadToFiberEx failed on the main thread (" << ::GetLastError() << L")");
    if (original == nullptr) {
      return;
    }
    std::wcout << L"    after conversion GetCurrentFiber() = " << ::GetCurrentFiber() << std::endl;
    CHECK(::IsThreadAFiber(), L"the main thread did not report as a fiber after conversion");

    // 4 MiB with float switching, exactly as wWinMain asks for it. If XP
    // refuses either the size or the flag, Electron falls through to the small
    // stack, so a failure here is the whole bug.
    SwitchState state = {original, false, false};
    LPVOID big =
        ::CreateFiberEx(0, 4 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, BigStackFiber, &state);
    CHECK(big != nullptr,
          L"CreateFiberEx(4 MiB, FLOAT_SWITCH) failed (" << ::GetLastError() << L")");
    if (big != nullptr) {
      ::SwitchToFiber(big);
      CHECK(state.ran, L"the big-stack fiber never ran");
      CHECK(state.reported_as_fiber, L"code running on a created fiber did not report as a fiber");
      ::DeleteFiber(big);
    }

    CHECK(::ConvertFiberToThread(), L"ConvertFiberToThread failed (" << ::GetLastError() << L")");
    CHECK(!::IsThreadAFiber(), L"the main thread still reports as a fiber after converting back");
  }

  void TestFibers() {
    std::wcout << L"  ConvertThreadToFiberEx, IsThreadAFiber" << std::endl;
    // On a worker, so the main thread is not left converted.
    RunThreads(FiberBody, 1);
  }

  // --- Critical sections and I/O cancellation --------------------------------

  void TestCriticalSectionEx() {
    std::wcout << L"  InitializeCriticalSectionEx" << std::endl;

    CRITICAL_SECTION section;
    CHECK(::InitializeCriticalSectionEx(&section, 1000, CRITICAL_SECTION_NO_DEBUG_INFO),
          L"InitializeCriticalSectionEx failed (" << ::GetLastError() << L")");
    ::EnterCriticalSection(&section);
    CHECK(::TryEnterCriticalSection(&section), L"a critical section refused its owning thread");
    ::LeaveCriticalSection(&section);
    ::LeaveCriticalSection(&section);
    ::DeleteCriticalSection(&section);
  }

  void TestIoCancellation() {
    std::wcout << L"  CancelIoEx, CancelSynchronousIo" << std::endl;

    wchar_t path[MAX_PATH] = {};
    HANDLE file            = MakeTempFile(path, 4);
    if (file != INVALID_HANDLE_VALUE) {
      // Nothing is outstanding, and the two implementations differ on what
      // that is -- a divergence cancel_io.cc documents rather than one found
      // here. The real CancelIoEx reports ERROR_NOT_FOUND when it finds no
      // request to cancel. winwrap's goes through CancelIo, which reports
      // success either way and gives it no means of telling "cancelled
      // something" from "there was nothing to cancel", so it says success.
      //
      // Either answer is accepted, because a caller cannot act on the
      // difference: both mean nothing is left pending. What would not be
      // acceptable is a third error, which is what this catches.
      const BOOL cancelled     = ::CancelIoEx(file, nullptr);
      const DWORD cancel_error = ::GetLastError();
      CHECK(cancelled || cancel_error == ERROR_NOT_FOUND,
            L"CancelIoEx with nothing pending failed with " << cancel_error
                                                            << L", expected ERROR_NOT_FOUND");
      ::CloseHandle(file);
      ::DeleteFileW(path);
    }

    // Cancelling another thread's synchronous I/O is what cannot be done before
    // Vista, and what must not be claimed.
    if (!::CancelSynchronousIo(::GetCurrentThread())) {
      CHECK(::GetLastError() == ERROR_NOT_FOUND, L"CancelSynchronousIo failed with "
                                                     << ::GetLastError()
                                                     << L", expected ERROR_NOT_FOUND");
    }
  }

  // --- Completion ports ------------------------------------------------------

  // Counts rather than doing nothing, so the alertable test below can tell "the
  // APC ran and the wait was reported oddly" from "the wait ended without
  // running the APC at all". Those are different faults and look identical
  // from the return value.
  LONG g_apc_runs = 0;

  VOID CALLBACK NoopApc(ULONG_PTR) {
    ::InterlockedIncrement(&g_apc_runs);
  }

  void TestCompletionPort() {
    std::wcout << L"  GetQueuedCompletionStatusEx, SetFileCompletionNotificationModes" << std::endl;

    HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    CHECK(port != nullptr, L"CreateIoCompletionPort failed (" << ::GetLastError() << L")");
    if (port == nullptr) {
      return;
    }

    // Hand-posted packets carry a null OVERLAPPED and must still count as
    // dequeued; libuv posts exactly these to break its loop out of a wait.
    constexpr ULONG kPosted = 4;
    for (ULONG i = 0; i < kPosted; ++i) {
      ::PostQueuedCompletionStatus(port, i, 100 + i, nullptr);
    }

    OVERLAPPED_ENTRY entries[8] = {};
    ULONG removed               = 0;
    CHECK(::GetQueuedCompletionStatusEx(port, entries, ARRAYSIZE(entries), &removed, 1000, FALSE),
          L"dequeue failed (" << ::GetLastError() << L")");
    CHECK(removed == kPosted, L"removed " << removed << L" entries, expected " << kPosted);
    for (ULONG i = 0; i < removed && i < kPosted; ++i) {
      CHECK(entries[i].lpCompletionKey == 100 + i,
            L"entry " << i << L" has key "
                      << (static_cast<unsigned long long>(entries[i].lpCompletionKey))
                      << L", expected " << (100 + i));
      CHECK(entries[i].dwNumberOfBytesTransferred == i,
            L"entry " << i << L" has " << (entries[i].dwNumberOfBytesTransferred)
                      << L" bytes, expected " << i);
    }

    // An empty port must time out rather than report a phantom entry.
    removed = 99;
    CHECK(!::GetQueuedCompletionStatusEx(port, entries, ARRAYSIZE(entries), &removed, 0, FALSE),
          L"an empty port reported success");
    CHECK(::GetLastError() == WAIT_TIMEOUT, L"an empty port gave error " << ::GetLastError());

    // The alertable path: a queued APC must end the wait, which the
    // non-alertable path cannot do at all.
    //
    // The documented shape is FALSE with WAIT_IO_COMPLETION and nothing
    // dequeued. That is what Windows 7 and later do, and what winwrap's own
    // implementation does -- so XP agrees with 11, and the assertion below is
    // the real check for everything this library is responsible for.
    //
    // Vista's own GetQueuedCompletionStatusEx does something else entirely,
    // and winwrap forwards to it there rather than substituting, so this is
    // measured rather than asserted. What it was observed to do, on a port the
    // call just above confirmed empty:
    //
    //   returns TRUE, sets no error -- the error seen afterwards was 258, the
    //   WAIT_TIMEOUT left behind by that previous call -- and reports one
    //   entry removed.
    //
    // The *success* is the dangerous half, not the count. Windows 10 leaves 1
    // in the count too, which is unremarkable there because it fails the call
    // and an output parameter of a failed call means nothing. Vista reports
    // success, and on that path the count is authoritative: a caller is told
    // there is one completion to process when there is none. That is why this
    // prints the entry and whether the APC ran. entries is zeroed first so the
    // contents are attributable -- a zero entry means Vista wrote one, and the
    // key from the dequeue further up means it left the buffer alone.
    //
    // Not a failure here. It is not winwrap's defect, and a Vista run that is
    // permanently red teaches people to stop reading the output. See
    // docs/INCOMPLETE_FUNCS.md, which is where a caller needs to find this.
    ::ZeroMemory(entries, sizeof(entries));
    g_apc_runs = 0;
    ::QueueUserAPC(NoopApc, ::GetCurrentThread(), 0);
    removed = 99;
    const BOOL alertable =
        ::GetQueuedCompletionStatusEx(port, entries, ARRAYSIZE(entries), &removed, 1000, TRUE);
    const DWORD alertable_error = ::GetLastError();
    const LONG apc_runs         = g_apc_runs;

    if (!alertable) {
      // The error, and only the error. The count is an output parameter of a
      // call that failed, so nothing defines it -- and Windows 10 does write
      // to it: seeded at 99, it came back as 1 alongside a correct
      // FALSE/WAIT_IO_COMPLETION. Asserting it was zero was this test's
      // mistake, not the system's, and it is worth not repeating: an output
      // parameter is part of the contract only on the success path.
      CHECK(alertable_error == WAIT_IO_COMPLETION,
            L"a queued APC gave error " << alertable_error << L", expected WAIT_IO_COMPLETION");
      std::wcout << L"    a queued APC ended the wait as documented; it also left " << removed
                 << L" in the count, which means nothing on a failure" << std::endl;
    } else {
      std::wcout << L"    a queued APC ended the wait as SUCCESS, which is Vista's shape and"
                 << L" not the documented one:" << std::endl;
      std::wcout << L"      entries removed: " << removed << L" (should be none)" << std::endl;
      std::wcout << L"      first entry:     key "
                 << static_cast<unsigned long long>(entries[0].lpCompletionKey) << L", "
                 << entries[0].dwNumberOfBytesTransferred << L" bytes, overlapped "
                 << (entries[0].lpOverlapped ? L"non-null" : L"null") << std::endl;
      // The one thing worth failing on, now that Vista's shape is known: the
      // phantom entry must not look like a real completion. Measured on Vista
      // it is entirely zero, so lpOverlapped is NULL -- which is the shape of
      // a hand-posted PostQueuedCompletionStatus packet, and a caller that
      // dispatches on lpOverlapped treats it as a wake-up rather than I/O.
      // A non-null pointer here would be categorically worse, because the
      // caller would dereference whatever it found.
      CHECK(entries[0].lpOverlapped == nullptr,
            L"a phantom entry carried a non-null OVERLAPPED, which a caller would follow");
      std::wcout << L"      APC ran:         " << (apc_runs ? L"yes" : L"NO, still queued")
                 << std::endl;
    }

    ::CloseHandle(port);

    // The two notification flags are not equally safe to pretend about.
    // Skipping the handle event is harmless; skipping the completion packet is
    // not, and must be refused where it cannot actually be arranged.
    //
    // FILE_FLAG_OVERLAPPED is not decoration here. The system's own
    // SetFileCompletionNotificationModes refuses a synchronous handle with
    // ERROR_INVALID_PARAMETER -- which made both checks below fail against a
    // plain handle, the first because it expected success and the second
    // because it expected winwrap's refusal and got the system's. winwrap's
    // own implementation ignores the handle entirely, so it would have passed
    // either way; the overlapped handle is what makes this test mean the same
    // thing on both. Hence no content: see MakeTempFile.
    wchar_t path[MAX_PATH] = {};
    HANDLE file            = MakeTempFile(path, 0, FILE_FLAG_OVERLAPPED);
    if (file != INVALID_HANDLE_VALUE) {
      CHECK(::SetFileCompletionNotificationModes(file, FILE_SKIP_SET_EVENT_ON_HANDLE),
            L"skipping the handle event was refused (" << ::GetLastError() << L")");

      // The packet-skipping flag, which only winwrap has to refuse and only
      // winwrap can be held to. From Vista the system implements it and the
      // call is forwarded, and whether it accepts the flag for a handle not
      // bound to a completion port is its own affair -- an expectation written
      // here about that would be one more guess about Windows, which is what
      // sent most of this function wrong to begin with.
      const BOOL skipped =
          ::SetFileCompletionNotificationModes(file, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
      const DWORD skip_error = ::GetLastError();
      if (!SystemHas(L"kernel32.dll", "SetFileCompletionNotificationModes")) {
        CHECK(!skipped, L"winwrap accepted FILE_SKIP_COMPLETION_PORT_ON_SUCCESS");
        CHECK(skip_error == ERROR_INVALID_FUNCTION,
              L"the refusal gave " << skip_error << L", expected ERROR_INVALID_FUNCTION");
      } else {
        std::wcout << L"    the system's FILE_SKIP_COMPLETION_PORT_ON_SUCCESS -> "
                   << (skipped ? L"accepted" : L"refused") << L" (" << skip_error << L")"
                   << std::endl;
      }
      ::CloseHandle(file);
      ::DeleteFileW(path);
    }
  }

  // --- AppUserModelID --------------------------------------------------------

  void TestAppUserModelId() {
    std::wcout << L"  Get/SetCurrentProcessExplicitAppUserModelID" << std::endl;

    CHECK(::SetCurrentProcessExplicitAppUserModelID(nullptr) == E_INVALIDARG,
          L"a null application ID was accepted");

    HRESULT hr = ::SetCurrentProcessExplicitAppUserModelID(L"winwrap.test");
    CHECK(SUCCEEDED(hr), L"setting an application ID failed (" << Hex(hr) << L")");

    PWSTR current = reinterpret_cast<PWSTR>(static_cast<ULONG_PTR>(0xdeadbeef));
    hr            = ::GetCurrentProcessExplicitAppUserModelID(&current);
    if (SUCCEEDED(hr)) {
      CHECK(current != nullptr, L"success with no identifier returned");
      ::CoTaskMemFree(current);
    } else {
      // The out parameter must be cleared even on failure: callers free it
      // unconditionally, and freeing an uninitialised pointer is a crash.
      CHECK(current == nullptr, L"failure left the out parameter untouched, and callers free it");
    }
  }

  // --- Keeping the window open -----------------------------------------------

  // Whether this process created the console it is printing to, rather than
  // inheriting one that will outlive it.
  //
  // Double-clicked from Explorer, a console program gets a console of its own
  // and that console closes when the process exits, so the results flash past
  // unreadably. Run from a prompt, or by nmake, or by CI, the console belongs
  // to the shell and holding it would block whatever ran us.
  bool OwnsItsConsole() {
    // No console at all: launched by a GUI parent with no handles, or with
    // output redirected and no console allocated. Nothing to hold open.
    if (::GetConsoleWindow() == nullptr) {
      return false;
    }

    // Resolved rather than imported, which is winwrap's own habit and here for
    // winwrap's own reason: this binary has to load on XP, and a statically
    // imported function the OS does not export is fatal before any of this
    // runs. check_imports.py would not catch that either -- it only knows the
    // names winwrap covers, and this is not one of them. Treat an absent
    // function as "not ours", which keeps the old behaviour rather than
    // risking a wait nobody is there to satisfy.
    using GetConsoleProcessListFn = DWORD(WINAPI*)(LPDWORD, DWORD);
    auto* process_list            = reinterpret_cast<GetConsoleProcessListFn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetConsoleProcessList"));
    if (process_list == nullptr) {
      return false;
    }

    // One attached process is this one, so the console came with it. Two or
    // more means a shell is attached and owns the window's lifetime.
    //
    // The count is what the return value carries whether or not the buffer was
    // big enough -- too small and it reports the number required and stores
    // nothing -- so a two-element array answers the question for any number of
    // processes.
    DWORD pids[2] = {};
    return process_list(pids, ARRAYSIZE(pids)) == 1;
  }

} // namespace

int wmain(int argc, WCHAR* argv[]) {
  // argc/argv unused for now.
  UNREFERENCED_PARAMETER(argc);
  UNREFERENCED_PARAMETER(argv);
  // Send wide characters to the console as they are, instead of letting
  // std::wcout convert them through the C locale, which is ASCII-only. This
  // matters more than it looks: a path or locale name carrying a character
  // outside ASCII does not merely print wrong, it fails the conversion, sets
  // badbit and silences every later line -- so a non-English user profile
  // would hide most of the run rather than one word of it. The narrow printf
  // this replaced dropped the character and carried on, so without this the
  // output is less robust than it used to be, not more.
  //
  // The cost is that redirected output is UTF-16 rather than bytes.
  ::_setmode(::_fileno(stdout), _O_U16TEXT);

  std::wcout << L"Winwrap Stress Tests" << std::endl;
  std::wcout << L"Built against winwrap v" << kWinWrapVer << std::endl;

  // Which OS the dispatch in dispatch.h is actually deciding against. Printed
  // rather than asserted: the right answer differs per machine, and a test run
  // whose results depend on the OS should say which one it was.
  const uint32_t nt = GetRealNTVersion();
  std::wcout << L"Real NT version " << Hex(nt);
  if (nt == 0) {
    std::wcout << L" (could not be determined)";
  }
  std::wcout << std::endl;

  const char* wine_version = nullptr;
  if (IsRunningOnWine(&wine_version)) {
    std::wcout << L"Running on Wine " << (wine_version ? wine_version : "?")
               << L" -- failures below may be Wine's, not winwrap's" << std::endl;
  }

  // First: winwrap's own API rather than a shim, and a version mismatch
  // between the library and these headers would explain anything below it.
  TestWinWrapApi();

  TestExclusive();
  TestShared();
  TestTryAcquire();
  TestConditionVariableQueue();
  TestConditionVariableTimeout();
  TestConditionVariableBroadcast();
  TestFls();
  TestMisc();
  TestLocale();
  TestLocaleNames();
  TestLocaleFormatters();
  TestEtw();

  TestFileInformationQuery();
  TestFileInformationSet();
  TestFinalPathAndReOpen();
  TestNamedPipes();
  TestProcessorNumber();
  TestLogicalProcessorInformation();
  TestErrorModeAndRestart();
  TestDirectories();
  TestFibers();
  TestMainThreadFiberSequence();
  TestCriticalSectionEx();
  TestIoCancellation();
  TestCompletionPort();
  TestAppUserModelId();
  TestRegGetValue();

  // Last: it alters the process irreversibly.
  TestDpiAware();

  if (g_failures == 0) {
    std::wcout << L"\nAll Passed!" << std::endl;
  } else {
    std::wcout << L"\n" << g_failures << L" Failure(s)" << std::endl;
  }

  // After the summary and before the return, so there is something to read and
  // the exit code is unaffected: the harness reports through its return value
  // and CI checks it, so waiting must not become part of the result.
  //
  // Only when this process owns the console. From a prompt, from nmake, or
  // from CI there is a shell attached and nothing waits.
  if (OwnsItsConsole()) {
    std::wcout << L"\nPress any key to exit." << std::endl;
    ::_getch();
  }

  return g_failures == 0 ? 0 : 1;
}
