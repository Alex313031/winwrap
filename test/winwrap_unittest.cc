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


// --- File information by handle --------------------------------------------

// Makes a temp file holding `bytes` bytes. Returns the handle and writes out
// the path.
HANDLE MakeTempFile(wchar_t* path_out, DWORD bytes) {
  wchar_t directory[MAX_PATH] = {};
  if (::GetTempPathW(ARRAYSIZE(directory), directory) == 0) {
    return INVALID_HANDLE_VALUE;
  }
  if (::GetTempFileNameW(directory, L"wwt", 0, path_out) == 0) {
    return INVALID_HANDLE_VALUE;
  }

  HANDLE file = ::CreateFileW(
      path_out, GENERIC_READ | GENERIC_WRITE | DELETE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return file;
  }
  if (bytes != 0) {
    auto* payload = static_cast<BYTE*>(
        ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
    DWORD written = 0;
    ::WriteFile(file, payload, bytes, &written, nullptr);
    ::FlushFileBuffers(file);
    ::HeapFree(::GetProcessHeap(), 0, payload);
  }
  return file;
}

void TestFileInformationQuery() {
  ::printf("  GetFileInformationByHandleEx\n");

  wchar_t path[MAX_PATH] = {};
  HANDLE file = MakeTempFile(path, 123);
  CHECK(file != INVALID_HANDLE_VALUE, "could not create a temp file (%lu)",
        ::GetLastError());
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  FILE_STANDARD_INFO standard = {};
  CHECK(::GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                       sizeof(standard)),
        "FileStandardInfo failed (%lu)", ::GetLastError());
  CHECK(standard.EndOfFile.QuadPart == 123, "size is %lld, expected 123",
        standard.EndOfFile.QuadPart);
  CHECK(!standard.Directory, "a file reported itself as a directory");

  FILE_BASIC_INFO basic = {};
  CHECK(::GetFileInformationByHandleEx(file, FileBasicInfo, &basic,
                                       sizeof(basic)),
        "FileBasicInfo failed (%lu)", ::GetLastError());
  CHECK(basic.FileAttributes != 0, "no attributes reported");
  CHECK(basic.LastWriteTime.QuadPart != 0, "no write time reported");

  // FileNameInfo is variable length and needs more than the bare struct.
  BYTE name_buffer[sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(WCHAR)] = {};
  auto* name = reinterpret_cast<FILE_NAME_INFO*>(name_buffer);
  CHECK(::GetFileInformationByHandleEx(file, FileNameInfo, name,
                                       sizeof(name_buffer)),
        "FileNameInfo failed (%lu)", ::GetLastError());
  CHECK(name->FileNameLength > 0, "an empty name came back");

  FILE_ATTRIBUTE_TAG_INFO tag = {};
  CHECK(::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag,
                                       sizeof(tag)),
        "FileAttributeTagInfo failed (%lu)", ::GetLastError());
  CHECK(tag.FileAttributes == basic.FileAttributes,
        "attributes disagree between classes: %lu vs %lu", tag.FileAttributes,
        basic.FileAttributes);

  // Windows 8 added this one; below that it is assembled from the volume
  // serial number and the index number, both of which must be populated.
  FILE_ID_INFO id = {};
  if (::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) {
    CHECK(id.VolumeSerialNumber != 0, "a zero volume serial number");
    bool any = false;
    for (size_t i = 0; i < sizeof(id.FileId.Identifier); ++i) {
      any = any || id.FileId.Identifier[i] != 0;
    }
    CHECK(any, "an all-zero file ID");
  } else {
    ::printf("    FileIdInfo unavailable (%lu)\n", ::GetLastError());
  }

  // A buffer too small to hold the answer, and a class that does not exist.
  // Both must be refused rather than half-filled.
  BYTE tiny[2] = {};
  CHECK(!::GetFileInformationByHandleEx(file, FileStandardInfo, tiny,
                                        sizeof(tiny)),
        "an undersized buffer was accepted");
  CHECK(!::GetFileInformationByHandleEx(
            file, static_cast<FILE_INFO_BY_HANDLE_CLASS>(99), &standard,
            sizeof(standard)),
        "an unknown information class was accepted");
  CHECK(::GetLastError() == ERROR_INVALID_PARAMETER,
        "unknown class gave error %lu, expected ERROR_INVALID_PARAMETER",
        ::GetLastError());

  ::CloseHandle(file);
  ::DeleteFileW(path);
}

void TestFileInformationSet() {
  ::printf("  SetFileInformationByHandle\n");

  wchar_t path[MAX_PATH] = {};
  HANDLE file = MakeTempFile(path, 500);
  if (file == INVALID_HANDLE_VALUE) {
    CHECK(false, "could not create a temp file (%lu)", ::GetLastError());
    return;
  }

  FILE_END_OF_FILE_INFO eof = {};
  eof.EndOfFile.QuadPart = 10;
  CHECK(::SetFileInformationByHandle(file, FileEndOfFileInfo, &eof,
                                     sizeof(eof)),
        "FileEndOfFileInfo failed (%lu)", ::GetLastError());
  FILE_STANDARD_INFO standard = {};
  ::GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                 sizeof(standard));
  CHECK(standard.EndOfFile.QuadPart == 10, "truncation left %lld bytes",
        standard.EndOfFile.QuadPart);

  FILE_BASIC_INFO basic = {};
  ::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
  basic.FileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY;
  CHECK(::SetFileInformationByHandle(file, FileBasicInfo, &basic,
                                     sizeof(basic)),
        "FileBasicInfo set failed (%lu)", ::GetLastError());

  CHECK(!::SetFileInformationByHandle(
            file, static_cast<FILE_INFO_BY_HANDLE_CLASS>(99), &basic,
            sizeof(basic)),
        "an unknown set class was accepted");

  // Delete-on-close through the disposition class, which is what libuv's
  // process code uses to discard a crash dump it could not finish writing.
  FILE_DISPOSITION_INFO disposition = {};
  disposition.DeleteFile = TRUE;
  CHECK(::SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                     sizeof(disposition)),
        "FileDispositionInfo failed (%lu)", ::GetLastError());
  ::CloseHandle(file);
  CHECK(::GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES,
        "the file survived a delete-on-close disposition");
  ::DeleteFileW(path);
}

void TestFinalPathAndReOpen() {
  ::printf("  GetFinalPathNameByHandleW, ReOpenFile\n");

  wchar_t path[MAX_PATH] = {};
  HANDLE file = MakeTempFile(path, 8);
  if (file == INVALID_HANDLE_VALUE) {
    CHECK(false, "could not create a temp file (%lu)", ::GetLastError());
    return;
  }

  // The documented two-call shape: with no buffer it returns the length
  // including the terminator, and callers size their buffer from that.
  DWORD needed = ::GetFinalPathNameByHandleW(file, nullptr, 0, VOLUME_NAME_DOS);
  CHECK(needed > 0, "length probe failed (%lu)", ::GetLastError());
  if (needed > 0) {
    auto* buffer = static_cast<wchar_t*>(::HeapAlloc(
        ::GetProcessHeap(), HEAP_ZERO_MEMORY, (needed + 1) * sizeof(wchar_t)));
    DWORD written =
        ::GetFinalPathNameByHandleW(file, buffer, needed, VOLUME_NAME_DOS);
    CHECK(written > 0 && written < needed,
          "second call returned %lu against a probe of %lu", written, needed);
    CHECK(::wcsncmp(buffer, L"\\\\?\\", 4) == 0,
          "the DOS form is not prefixed with \\\\?\\: \"%ls\"", buffer);
    ::printf("    final path: %ls\n", buffer);
    ::HeapFree(::GetProcessHeap(), 0, buffer);
  }

  wchar_t nt_path[1024] = {};
  DWORD nt_written = ::GetFinalPathNameByHandleW(
      file, nt_path, ARRAYSIZE(nt_path), VOLUME_NAME_NT);
  CHECK(nt_written > 0, "VOLUME_NAME_NT failed (%lu)", ::GetLastError());
  if (nt_written > 0) {
    CHECK(nt_path[0] == L'\\', "an NT path not starting with a separator");
  }

  HANDLE reopened = ::ReOpenFile(
      file, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
  CHECK(reopened != INVALID_HANDLE_VALUE, "ReOpenFile on a file failed (%lu)",
        ::GetLastError());
  if (reopened != INVALID_HANDLE_VALUE) {
    ::CloseHandle(reopened);
  }

  ::CloseHandle(file);
  ::DeleteFileW(path);

  // A directory, which is the case that breaks if the implementation forbids
  // directories outright. FILE_FLAG_BACKUP_SEMANTICS is how a caller says the
  // handle might be one, and libuv depends on it to reopen a directory for
  // FILE_WRITE_ATTRIBUTES.
  wchar_t directory[MAX_PATH] = {};
  ::GetTempPathW(ARRAYSIZE(directory), directory);
  HANDLE dir = ::CreateFileW(
      directory, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (dir != INVALID_HANDLE_VALUE) {
    HANDLE dir_again = ::ReOpenFile(
        dir, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_FLAG_BACKUP_SEMANTICS);
    CHECK(dir_again != INVALID_HANDLE_VALUE,
          "ReOpenFile could not reopen a directory (%lu)", ::GetLastError());
    if (dir_again != INVALID_HANDLE_VALUE) {
      ::CloseHandle(dir_again);
    }
    ::CloseHandle(dir);
  }
}

// --- Named pipes -----------------------------------------------------------

void TestNamedPipes() {
  ::printf("  Named pipe process IDs\n");

  wchar_t name[128] = {};
  ::wsprintfW(name, L"\\\\.\\pipe\\winwrap-test-%lu", ::GetCurrentProcessId());

  HANDLE server =
      ::CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
                         1, 512, 512, 0, nullptr);
  CHECK(server != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed (%lu)",
        ::GetLastError());
  if (server == INVALID_HANDLE_VALUE) {
    return;
  }

  HANDLE client = ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
  CHECK(client != INVALID_HANDLE_VALUE, "connecting to the pipe failed (%lu)",
        ::GetLastError());

  // Both ends belong to this process, so any answer at all must be this
  // process. Where the information is unavailable the call has to fail
  // cleanly rather than invent one: a wrong process ID here would defeat the
  // privilege check Mojo builds on top of it.
  const DWORD self = ::GetCurrentProcessId();
  ULONG pid = 0;
  if (::GetNamedPipeServerProcessId(server, &pid)) {
    CHECK(pid == self, "server process ID is %lu, expected %lu", pid, self);
  } else {
    CHECK(::GetLastError() == ERROR_NOT_SUPPORTED,
          "server ID failed with %lu, expected ERROR_NOT_SUPPORTED",
          ::GetLastError());
    ::printf("    peer process IDs unavailable, as expected before Vista\n");
  }

  if (client != INVALID_HANDLE_VALUE) {
    pid = 0;
    if (::GetNamedPipeClientProcessId(server, &pid)) {
      CHECK(pid == self, "client process ID is %lu, expected %lu", pid, self);
    } else {
      CHECK(::GetLastError() == ERROR_NOT_SUPPORTED,
            "client ID failed with %lu, expected ERROR_NOT_SUPPORTED",
            ::GetLastError());
    }
    ::CloseHandle(client);
  }

  CHECK(!::GetNamedPipeServerProcessId(server, nullptr),
        "a null out parameter was accepted");
  CHECK(::GetLastError() == ERROR_INVALID_PARAMETER,
        "a null out parameter gave %lu", ::GetLastError());

  ::CloseHandle(server);
}

// --- Threads, error mode, restart ------------------------------------------

void TestProcessorNumber() {
  ::printf("  GetCurrentProcessorNumber\n");

  SYSTEM_INFO info = {};
  ::GetSystemInfo(&info);

  // Pinning the thread makes the answer stable, which is the only property
  // worth asserting: the numbering itself comes from firmware.
  DWORD_PTR previous = ::SetThreadAffinityMask(::GetCurrentThread(), 1);
  DWORD first = ::GetCurrentProcessorNumber();
  DWORD second = ::GetCurrentProcessorNumber();
  if (previous != 0) {
    ::SetThreadAffinityMask(::GetCurrentThread(), previous);
  }

  CHECK(first == second, "pinned to one processor but reported %lu then %lu",
        first, second);
  ::printf("    reported processor %lu of %lu\n", first,
           info.dwNumberOfProcessors);
  if (first >= info.dwNumberOfProcessors) {
    // Not a failure: the APIC IDs this derives from need not be dense.
    ::printf("    note: above the processor count, so IDs are sparse here\n");
  }
}

void TestErrorModeAndRestart() {
  ::printf("  GetErrorMode, RegisterApplicationRestart\n");

  const UINT original = ::GetErrorMode();

  // A round trip through SetErrorMode is what catches an inverted bit, which
  // is the one way this is easy to get wrong.
  const UINT wanted = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX;
  ::SetErrorMode(wanted);
  CHECK(::GetErrorMode() == wanted, "set %#x, read back %#x", wanted,
        ::GetErrorMode());

  ::SetErrorMode(SEM_NOOPENFILEERRORBOX);
  CHECK(::GetErrorMode() == SEM_NOOPENFILEERRORBOX, "set %#x, read back %#x",
        SEM_NOOPENFILEERRORBOX, ::GetErrorMode());

  ::SetErrorMode(0);
  CHECK(::GetErrorMode() == 0u, "set 0, read back %#x", ::GetErrorMode());

  ::SetErrorMode(original);

  HRESULT hr = ::RegisterApplicationRestart(L"--restarted", 0);
  CHECK(FAILED(hr), "RegisterApplicationRestart reported success (%#lx)", hr);
  hr = ::UnregisterApplicationRestart();
  CHECK(FAILED(hr), "UnregisterApplicationRestart reported success (%#lx)", hr);
}

// --- Directories -----------------------------------------------------------

void TestDirectories() {
  ::printf("  CreateSymbolicLinkW, NeedCurrentDirectoryForExePathW\n");

  wchar_t directory[MAX_PATH] = {};
  ::GetTempPathW(ARRAYSIZE(directory), directory);
  wchar_t link[MAX_PATH] = {};
  ::wsprintfW(link, L"%swinwrap-link-%lu", directory, ::GetCurrentProcessId());

  if (::CreateSymbolicLinkW(link, directory, SYMBOLIC_LINK_FLAG_DIRECTORY)) {
    ::printf("    symbolic links are available here\n");
    ::RemoveDirectoryW(link);
  } else {
    ::printf("    symbolic link refused (%lu), as expected without support\n",
             ::GetLastError());
  }

  // A name that already carries a path never consults the environment.
  CHECK(::NeedCurrentDirectoryForExePathW(L"dir\\program.exe"),
        "a path-qualified name was refused the current directory");

  const wchar_t kVariable[] = L"NoDefaultCurrentDirectoryInExePath";
  const bool had = ::GetEnvironmentVariableW(kVariable, nullptr, 0) != 0;

  ::SetEnvironmentVariableW(kVariable, nullptr);
  CHECK(::NeedCurrentDirectoryForExePathW(L"program.exe"),
        "the current directory was refused with the variable unset");

  ::SetEnvironmentVariableW(kVariable, L"1");
  CHECK(!::NeedCurrentDirectoryForExePathW(L"program.exe"),
        "the current directory was allowed with the variable set");
  CHECK(::NeedCurrentDirectoryForExePathW(L"dir\\program.exe"),
        "a path-qualified name was affected by the variable");

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
          "an empty value did not count as set");
  } else {
    ::printf("    an empty value removes the variable here; skipped\n");
  }

  if (!had) {
    ::SetEnvironmentVariableW(kVariable, nullptr);
  }
}

// --- Fibers ----------------------------------------------------------------

DWORD WINAPI FiberBody(LPVOID) {
  CHECK(!::IsThreadAFiber(), "a fresh thread claimed to be a fiber");

  LPVOID fiber = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
  CHECK(fiber != nullptr, "ConvertThreadToFiberEx failed (%lu)",
        ::GetLastError());
  if (fiber == nullptr) {
    return 0;
  }

  CHECK(::IsThreadAFiber(), "a converted thread did not report as a fiber");
  CHECK(::ConvertFiberToThread(), "ConvertFiberToThread failed (%lu)",
        ::GetLastError());
  CHECK(!::IsThreadAFiber(),
        "a thread converted back still reports as a fiber");
  return 0;
}

void TestFibers() {
  ::printf("  ConvertThreadToFiberEx, IsThreadAFiber\n");
  // On a worker, so the main thread is not left converted.
  RunThreads(FiberBody, 1);
}

// --- Critical sections and I/O cancellation --------------------------------

void TestCriticalSectionEx() {
  ::printf("  InitializeCriticalSectionEx\n");

  CRITICAL_SECTION section;
  CHECK(::InitializeCriticalSectionEx(&section, 1000,
                                      CRITICAL_SECTION_NO_DEBUG_INFO),
        "InitializeCriticalSectionEx failed (%lu)", ::GetLastError());
  ::EnterCriticalSection(&section);
  CHECK(::TryEnterCriticalSection(&section),
        "a critical section refused its owning thread");
  ::LeaveCriticalSection(&section);
  ::LeaveCriticalSection(&section);
  ::DeleteCriticalSection(&section);
}

void TestIoCancellation() {
  ::printf("  CancelIoEx, CancelSynchronousIo\n");

  wchar_t path[MAX_PATH] = {};
  HANDLE file = MakeTempFile(path, 4);
  if (file != INVALID_HANDLE_VALUE) {
    // Nothing is outstanding, which counts as a successful cancel.
    CHECK(::CancelIoEx(file, nullptr),
          "CancelIoEx failed with nothing pending (%lu)", ::GetLastError());
    ::CloseHandle(file);
    ::DeleteFileW(path);
  }

  // Cancelling another thread's synchronous I/O is what cannot be done before
  // Vista, and what must not be claimed.
  if (!::CancelSynchronousIo(::GetCurrentThread())) {
    CHECK(::GetLastError() == ERROR_NOT_FOUND,
          "CancelSynchronousIo failed with %lu, expected ERROR_NOT_FOUND",
          ::GetLastError());
  }
}

// --- Completion ports ------------------------------------------------------

VOID CALLBACK NoopApc(ULONG_PTR) {}

void TestCompletionPort() {
  ::printf("  GetQueuedCompletionStatusEx, SetFileCompletionNotificationModes\n");

  HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
  CHECK(port != nullptr, "CreateIoCompletionPort failed (%lu)",
        ::GetLastError());
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
  ULONG removed = 0;
  CHECK(::GetQueuedCompletionStatusEx(port, entries, ARRAYSIZE(entries),
                                      &removed, 1000, FALSE),
        "dequeue failed (%lu)", ::GetLastError());
  CHECK(removed == kPosted, "removed %lu entries, expected %lu", removed,
        kPosted);
  for (ULONG i = 0; i < removed && i < kPosted; ++i) {
    CHECK(entries[i].lpCompletionKey == 100 + i,
          "entry %lu has key %llu, expected %lu", i,
          static_cast<unsigned long long>(entries[i].lpCompletionKey),
          100 + i);
    CHECK(entries[i].dwNumberOfBytesTransferred == i,
          "entry %lu has %lu bytes, expected %lu", i,
          entries[i].dwNumberOfBytesTransferred, i);
  }

  // An empty port must time out rather than report a phantom entry.
  removed = 99;
  CHECK(!::GetQueuedCompletionStatusEx(port, entries, ARRAYSIZE(entries),
                                       &removed, 0, FALSE),
        "an empty port reported success");
  CHECK(::GetLastError() == WAIT_TIMEOUT, "an empty port gave error %lu",
        ::GetLastError());

  // The alertable path: a queued APC must end the wait and be reported as
  // such, which the non-alertable path cannot do at all.
  ::QueueUserAPC(NoopApc, ::GetCurrentThread(), 0);
  removed = 0;
  BOOL alertable = ::GetQueuedCompletionStatusEx(
      port, entries, ARRAYSIZE(entries), &removed, 1000, TRUE);
  CHECK(!alertable, "an empty port reported success while alertable");
  CHECK(::GetLastError() == WAIT_IO_COMPLETION,
        "a queued APC gave error %lu, expected WAIT_IO_COMPLETION",
        ::GetLastError());

  ::CloseHandle(port);

  // The two notification flags are not equally safe to pretend about.
  // Skipping the handle event is harmless; skipping the completion packet is
  // not, and must be refused where it cannot actually be arranged.
  wchar_t path[MAX_PATH] = {};
  HANDLE file = MakeTempFile(path, 1);
  if (file != INVALID_HANDLE_VALUE) {
    CHECK(::SetFileCompletionNotificationModes(file,
                                               FILE_SKIP_SET_EVENT_ON_HANDLE),
          "skipping the handle event was refused (%lu)", ::GetLastError());
    if (!::SetFileCompletionNotificationModes(
            file, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
      CHECK(::GetLastError() == ERROR_INVALID_FUNCTION,
            "the refusal gave %lu, expected ERROR_INVALID_FUNCTION",
            ::GetLastError());
    }
    ::CloseHandle(file);
    ::DeleteFileW(path);
  }
}

// --- AppUserModelID --------------------------------------------------------

void TestAppUserModelId() {
  ::printf("  Get/SetCurrentProcessExplicitAppUserModelID\n");

  CHECK(::SetCurrentProcessExplicitAppUserModelID(nullptr) == E_INVALIDARG,
        "a null application ID was accepted");

  HRESULT hr = ::SetCurrentProcessExplicitAppUserModelID(L"winwrap.test");
  CHECK(SUCCEEDED(hr), "setting an application ID failed (%#lx)", hr);

  PWSTR current = reinterpret_cast<PWSTR>(static_cast<ULONG_PTR>(0xdeadbeef));
  hr = ::GetCurrentProcessExplicitAppUserModelID(&current);
  if (SUCCEEDED(hr)) {
    CHECK(current != nullptr, "success with no identifier returned");
    ::CoTaskMemFree(current);
  } else {
    // The out parameter must be cleared even on failure: callers free it
    // unconditionally, and freeing an uninitialised pointer is a crash.
    CHECK(current == nullptr,
          "failure left the out parameter untouched, and callers free it");
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

  TestFileInformationQuery();
  TestFileInformationSet();
  TestFinalPathAndReOpen();
  TestNamedPipes();
  TestProcessorNumber();
  TestErrorModeAndRestart();
  TestDirectories();
  TestFibers();
  TestCriticalSectionEx();
  TestIoCancellation();
  TestCompletionPort();
  TestAppUserModelId();

  if (g_failures == 0) {
    ::printf("all passed\n");
    return 0;
  }
  ::printf("%d failure(s)\n", g_failures);
  return 1;
}
