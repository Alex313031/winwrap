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
    std::wcout << L"  InitOnceExecuteOnce, GetTickCount64, CompareStringOrdinal" << std::endl;

    g_once_runs = 0;
    RunThreads(OnceCaller, kThreads);
    CHECK(g_once_runs == 1, L"the once-callback ran " << g_once_runs << L" times");

    ULONGLONG first = ::GetTickCount64();
    ::Sleep(50);
    ULONGLONG second = ::GetTickCount64();
    CHECK(second >= first, L"GetTickCount64 went backwards");
    CHECK(second - first >= 30,
          L"GetTickCount64 advanced only " << (second - first) << L" ms over 50");

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

    HANDLE file = ::CreateFileW(path_out, GENERIC_READ | GENERIC_WRITE | DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    wchar_t directory[MAX_PATH] = {};
    ::GetTempPathW(ARRAYSIZE(directory), directory);
    HANDLE dir = ::CreateFileW(directory, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir != INVALID_HANDLE_VALUE) {
      HANDLE dir_again = ::ReOpenFile(dir, FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      FILE_FLAG_BACKUP_SEMANTICS);
      CHECK(dir_again != INVALID_HANDLE_VALUE,
            L"ReOpenFile could not reopen a directory (" << ::GetLastError() << L")");
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

    CHECK(!::GetNamedPipeServerProcessId(server, nullptr), L"a null out parameter was accepted");
    CHECK(::GetLastError() == ERROR_INVALID_PARAMETER,
          L"a null out parameter gave " << ::GetLastError());

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

    HRESULT hr = ::RegisterApplicationRestart(L"--restarted", 0);
    CHECK(FAILED(hr), L"RegisterApplicationRestart reported success (" << Hex(hr) << L")");
    hr = ::UnregisterApplicationRestart();
    CHECK(FAILED(hr), L"UnregisterApplicationRestart reported success (" << Hex(hr) << L")");
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
      // Nothing is outstanding, which counts as a successful cancel.
      CHECK(::CancelIoEx(file, nullptr),
            L"CancelIoEx failed with nothing pending (" << ::GetLastError() << L")");
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

  VOID CALLBACK NoopApc(ULONG_PTR) {
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

    // The alertable path: a queued APC must end the wait and be reported as
    // such, which the non-alertable path cannot do at all.
    ::QueueUserAPC(NoopApc, ::GetCurrentThread(), 0);
    removed = 0;
    BOOL alertable =
        ::GetQueuedCompletionStatusEx(port, entries, ARRAYSIZE(entries), &removed, 1000, TRUE);
    CHECK(!alertable, L"an empty port reported success while alertable");
    CHECK(::GetLastError() == WAIT_IO_COMPLETION,
          L"a queued APC gave error " << ::GetLastError() << L", expected WAIT_IO_COMPLETION");

    ::CloseHandle(port);

    // The two notification flags are not equally safe to pretend about.
    // Skipping the handle event is harmless; skipping the completion packet is
    // not, and must be refused where it cannot actually be arranged.
    wchar_t path[MAX_PATH] = {};
    HANDLE file            = MakeTempFile(path, 1);
    if (file != INVALID_HANDLE_VALUE) {
      CHECK(::SetFileCompletionNotificationModes(file, FILE_SKIP_SET_EVENT_ON_HANDLE),
            L"skipping the handle event was refused (" << ::GetLastError() << L")");
      if (!::SetFileCompletionNotificationModes(file, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) {
        CHECK(::GetLastError() == ERROR_INVALID_FUNCTION,
              L"the refusal gave " << ::GetLastError() << L", expected ERROR_INVALID_FUNCTION");
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

} // namespace

int wmain(int argc, WCHAR* argv[]) {
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
  TestMainThreadFiberSequence();
  TestCriticalSectionEx();
  TestIoCancellation();
  TestCompletionPort();
  TestAppUserModelId();

  if (g_failures == 0) {
    std::wcout << L"\nAll Passed!" << std::endl;
    return 0;
  }
  std::wcout << L"\n" << g_failures << L" Failure(s)" << std::endl;
  return 1;
}
