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

#include <stdio.h>

namespace {

constexpr DWORD kThreads = 16;
constexpr int kLockIterations = 20000;

void RunThreads(LPTHREAD_START_ROUTINE body, DWORD count) {
  HANDLE threads[kThreads] = {};
  for (DWORD i = 0; i < count; ++i) {
    threads[i] = ::CreateThread(nullptr, 0, body,
                                reinterpret_cast<LPVOID>(
                                    static_cast<ULONG_PTR>(i)),
                                0, nullptr);
    if (!threads[i]) {
      ++g_failures;
      ::printf("    FAIL could not create thread %lu\n", i);
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
    CHECK(inside == 1, "%ld threads inside an exclusive lock", inside);
    ++g_plain_counter;
    ::InterlockedDecrement(&g_inside);
    ::ReleaseSRWLockExclusive(&g_exclusive_lock);
  }
  return 0;
}

void TestExclusive() {
  ::printf("  SRWLOCK exclusive\n");
  ::InitializeSRWLock(&g_exclusive_lock);
  g_plain_counter = 0;
  RunThreads(ExclusiveBody, kThreads);

  unsigned long long expected =
      static_cast<unsigned long long>(kThreads) * kLockIterations;
  CHECK(g_plain_counter == expected, "counter is %llu, expected %llu",
        g_plain_counter, expected);
}

// --- SRWLOCK, shared vs exclusive ------------------------------------------

SRWLOCK g_shared_lock;
volatile LONG g_readers = 0;
volatile LONG g_writers = 0;
volatile LONG g_max_readers = 0;
// Writers keep these equal; a reader that sees them differ caught a writer
// mid-update, which a shared lock must make impossible.
volatile LONG g_left = 0;
volatile LONG g_right = 0;

DWORD WINAPI SharedBody(LPVOID) {
  for (int i = 0; i < kLockIterations; ++i) {
    if ((i & 7) == 0) {
      ::AcquireSRWLockExclusive(&g_shared_lock);
      CHECK(::InterlockedIncrement(&g_writers) == 1, "two writers at once");
      CHECK(g_readers == 0, "%ld readers inside an exclusive lock", g_readers);
      LONG value = g_left + 1;
      g_left = value;
      ::Sleep(0);  // Widen the window a torn read could fall into.
      g_right = value;
      ::InterlockedDecrement(&g_writers);
      ::ReleaseSRWLockExclusive(&g_shared_lock);
    } else {
      ::AcquireSRWLockShared(&g_shared_lock);
      LONG now = ::InterlockedIncrement(&g_readers);
      if (now > g_max_readers) {
        g_max_readers = now;  // Racy, but only ever used as a lower bound.
      }
      CHECK(g_writers == 0, "a writer is inside while %ld readers are", now);
      CHECK(g_left == g_right, "reader saw a torn update: %ld vs %ld", g_left,
            g_right);
      ::InterlockedDecrement(&g_readers);
      ::ReleaseSRWLockShared(&g_shared_lock);
    }
  }
  return 0;
}

void TestShared() {
  ::printf("  SRWLOCK shared\n");
  ::InitializeSRWLock(&g_shared_lock);
  g_max_readers = 0;
  RunThreads(SharedBody, kThreads);

  // The old CRITICAL_SECTION implementation serialised readers, so this is
  // the assertion that distinguishes a real reader/writer lock from a mutex
  // wearing its name.
  CHECK(g_max_readers > 1, "readers never overlapped (max %ld); shared is "
                           "behaving like exclusive",
        g_max_readers);
  ::printf("    peak concurrent readers: %ld\n", g_max_readers);
}

void TestTryAcquire() {
  ::printf("  SRWLOCK try variants\n");
  SRWLOCK lock;
  ::InitializeSRWLock(&lock);

  CHECK(::TryAcquireSRWLockExclusive(&lock), "try-exclusive failed when free");
  ::ReleaseSRWLockExclusive(&lock);

  ::AcquireSRWLockShared(&lock);
  CHECK(!::TryAcquireSRWLockExclusive(&lock),
        "try-exclusive succeeded while held shared");
  CHECK(::TryAcquireSRWLockShared(&lock), "try-shared failed while held shared");
  ::ReleaseSRWLockShared(&lock);
  ::ReleaseSRWLockShared(&lock);

  ::AcquireSRWLockExclusive(&lock);
  CHECK(!::TryAcquireSRWLockShared(&lock),
        "try-shared succeeded while held exclusive");
  ::ReleaseSRWLockExclusive(&lock);
}

// --- Condition variables ---------------------------------------------------

constexpr int kQueueCapacity = 8;
constexpr int kItemsPerProducer = 4000;
constexpr DWORD kProducers = 4;
constexpr DWORD kConsumers = 4;

CRITICAL_SECTION g_queue_lock;
CONDITION_VARIABLE g_not_empty;
CONDITION_VARIABLE g_not_full;
int g_queue[kQueueCapacity];
int g_queue_count = 0;
int g_queue_head = 0;
int g_queue_tail = 0;
int g_produced = 0;
long long g_consumed_sum = 0;
int g_consumed_count = 0;
bool g_producers_done = false;

DWORD WINAPI ProducerBody(LPVOID) {
  for (int i = 0; i < kItemsPerProducer; ++i) {
    ::EnterCriticalSection(&g_queue_lock);
    while (g_queue_count == kQueueCapacity) {
      ::SleepConditionVariableCS(&g_not_full, &g_queue_lock, INFINITE);
    }
    g_queue[g_queue_tail] = 1;
    g_queue_tail = (g_queue_tail + 1) % kQueueCapacity;
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
    int value = g_queue[g_queue_head];
    g_queue_head = (g_queue_head + 1) % kQueueCapacity;
    --g_queue_count;
    g_consumed_sum += value;
    ++g_consumed_count;
    ::LeaveCriticalSection(&g_queue_lock);
    ::WakeConditionVariable(&g_not_full);
  }
}

void TestConditionVariableQueue() {
  ::printf("  CONDITION_VARIABLE producer/consumer\n");
  ::InitializeCriticalSection(&g_queue_lock);
  ::InitializeConditionVariable(&g_not_empty);
  ::InitializeConditionVariable(&g_not_full);

  HANDLE threads[kProducers + kConsumers] = {};
  DWORD n = 0;
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
  CHECK(wait != WAIT_TIMEOUT, "consumers never woke up -- lost wake-up");

  for (DWORD i = 0; i < n; ++i) {
    ::CloseHandle(threads[i]);
  }

  int expected = static_cast<int>(kProducers) * kItemsPerProducer;
  CHECK(g_produced == expected, "produced %d, expected %d", g_produced,
        expected);
  CHECK(g_consumed_count == expected, "consumed %d, expected %d",
        g_consumed_count, expected);
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
  ::printf("  CONDITION_VARIABLE timed wait\n");
  ::InitializeCriticalSection(&g_timeout_cs);
  ::InitializeConditionVariable(&g_timeout_cv);

  ::EnterCriticalSection(&g_timeout_cs);
  DWORD start = ::GetTickCount();
  BOOL result = ::SleepConditionVariableCS(&g_timeout_cv, &g_timeout_cs, 200);
  DWORD elapsed = ::GetTickCount() - start;
  DWORD error = ::GetLastError();

  CHECK(!result, "a timed wait with no wake-up returned success");
  CHECK(error == ERROR_TIMEOUT, "last error is %lu, expected ERROR_TIMEOUT",
        error);
  // Timer granularity on these systems is ~15ms, so allow slack below.
  CHECK(elapsed >= 150, "returned after %lums, asked for 200ms", elapsed);

  // The caller must hold the lock again on return, including after a timeout.
  BOOL still_held = FALSE;
  HANDLE prober =
      ::CreateThread(nullptr, 0, TryEnterBody, &still_held, 0, nullptr);
  ::WaitForSingleObject(prober, INFINITE);
  ::CloseHandle(prober);
  CHECK(still_held, "lock was not re-acquired after a timed wait");

  ::LeaveCriticalSection(&g_timeout_cs);
  ::DeleteCriticalSection(&g_timeout_cs);
}

CONDITION_VARIABLE g_broadcast_cv;
SRWLOCK g_broadcast_lock;
bool g_broadcast_go = false;
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
  ::printf("  CONDITION_VARIABLE broadcast over SRWLOCK\n");
  ::InitializeSRWLock(&g_broadcast_lock);
  ::InitializeConditionVariable(&g_broadcast_cv);
  g_broadcast_go = false;
  g_broadcast_woken = 0;

  HANDLE threads[kThreads] = {};
  for (DWORD i = 0; i < kThreads; ++i) {
    threads[i] = ::CreateThread(nullptr, 0, BroadcastBody, nullptr, 0, nullptr);
  }
  ::Sleep(100);  // Give them time to actually be waiting.

  ::AcquireSRWLockExclusive(&g_broadcast_lock);
  g_broadcast_go = true;
  ::ReleaseSRWLockExclusive(&g_broadcast_lock);
  // Every waiter must be released by one call, not just the first.
  ::WakeAllConditionVariable(&g_broadcast_cv);

  DWORD wait = ::WaitForMultipleObjects(kThreads, threads, TRUE, 30000);
  CHECK(wait != WAIT_TIMEOUT, "only %ld of %lu waiters woke from a broadcast",
        g_broadcast_woken, kThreads);
  for (DWORD i = 0; i < kThreads; ++i) {
    ::CloseHandle(threads[i]);
  }
}

// --- Fiber-local storage ---------------------------------------------------

DWORD g_fls_index = FLS_OUT_OF_INDEXES;
volatile LONG g_fls_callbacks = 0;

void WINAPI FlsCallback(PVOID) {
  ::InterlockedIncrement(&g_fls_callbacks);
}

DWORD WINAPI FlsBody(LPVOID id) {
  CHECK(::FlsGetValue(g_fls_index) == nullptr,
        "a fresh thread already had an FLS value");
  PVOID mine = reinterpret_cast<PVOID>(
      static_cast<ULONG_PTR>(reinterpret_cast<ULONG_PTR>(id) + 1));
  CHECK(::FlsSetValue(g_fls_index, mine), "FlsSetValue failed");
  ::Sleep(5);  // Let every other thread store its own value first.
  CHECK(::FlsGetValue(g_fls_index) == mine, "another thread's FLS value leaked in");
  return 0;
}

void TestFls() {
  ::printf("  Fiber-local storage\n");
  g_fls_callbacks = 0;
  g_fls_index = ::FlsAlloc(FlsCallback);
  CHECK(g_fls_index != FLS_OUT_OF_INDEXES, "FlsAlloc failed");
  if (g_fls_index == FLS_OUT_OF_INDEXES) {
    return;
  }

  RunThreads(FlsBody, kThreads);

  // The callback fires from a TLS callback at thread exit, which is the part
  // of this that XP does not do by itself.
  CHECK(g_fls_callbacks == static_cast<LONG>(kThreads),
        "%ld thread-exit callbacks, expected %lu", g_fls_callbacks, kThreads);

  CHECK(::FlsFree(g_fls_index), "FlsFree failed");
  CHECK(!::FlsFree(g_fls_index), "freeing an index twice succeeded");
}

// --- Odds and ends ---------------------------------------------------------

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
volatile LONG g_once_runs = 0;

BOOL CALLBACK OnceBody(PINIT_ONCE, PVOID, PVOID*) {
  ::InterlockedIncrement(&g_once_runs);
  ::Sleep(10);
  return TRUE;
}

DWORD WINAPI OnceCaller(LPVOID) {
  CHECK(::InitOnceExecuteOnce(&g_once, OnceBody, nullptr, nullptr),
        "InitOnceExecuteOnce failed");
  return 0;
}

void TestMisc() {
  ::printf("  InitOnceExecuteOnce, GetTickCount64, CompareStringOrdinal\n");

  g_once_runs = 0;
  RunThreads(OnceCaller, kThreads);
  CHECK(g_once_runs == 1, "the once-callback ran %ld times", g_once_runs);

  ULONGLONG first = ::GetTickCount64();
  ::Sleep(50);
  ULONGLONG second = ::GetTickCount64();
  CHECK(second >= first, "GetTickCount64 went backwards");
  CHECK(second - first >= 30, "GetTickCount64 advanced only %llu ms over 50",
        second - first);

  CHECK(::CompareStringOrdinal(L"abc", -1, L"abc", -1, FALSE) == CSTR_EQUAL,
        "equal strings did not compare equal");
  CHECK(::CompareStringOrdinal(L"abc", -1, L"ABC", -1, TRUE) == CSTR_EQUAL,
        "case-insensitive compare failed");
  CHECK(::CompareStringOrdinal(L"abc", -1, L"ABC", -1, FALSE) ==
            CSTR_GREATER_THAN,
        "lowercase did not sort after uppercase ordinally");
  CHECK(::CompareStringOrdinal(L"abc", -1, L"abcd", -1, FALSE) == CSTR_LESS_THAN,
        "a prefix did not sort before the longer string");
  CHECK(::CompareStringOrdinal(L"abcd", 3, L"abc", -1, FALSE) == CSTR_EQUAL,
        "an explicit length was not honoured");

  // Not every system has this locale; only check the shape of the answer.
  wchar_t buffer[64] = {};
  int written = ::GetLocaleInfoEx(L"en-US", LOCALE_SENGLANGUAGE, buffer,
                                  ARRAYSIZE(buffer));
  if (written > 0) {
    ::printf("    GetLocaleInfoEx(en-US) -> \"%ls\"\n", buffer);
  } else {
    ::printf("    GetLocaleInfoEx(en-US) unavailable on this system\n");
  }
}

}  // namespace

int wmain(int argc, WCHAR* argv[]) {
  ::printf("winwrap stress tests\n");

  TestExclusive();
  TestShared();
  TestTryAcquire();
  TestConditionVariableQueue();
  TestConditionVariableTimeout();
  TestConditionVariableBroadcast();
  TestFls();
  TestMisc();

  if (g_failures == 0) {
    ::printf("all passed\n");
    return 0;
  }
  ::printf("%d failure(s)\n", g_failures);
  return 1;
}
