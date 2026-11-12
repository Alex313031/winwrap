// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// See parking_lot.h for what this is and why it is not keyed events.

#include "parking_lot.h"

namespace winwrap {
namespace {

// Buckets are only ever held for the few instructions it takes to walk a short
// list, so collisions cost very little and the table does not need to be
// large. A power of two keeps the index a mask.
constexpr size_t kBucketCount = 256;
constexpr DWORD kBucketSpinCount = 2000;

struct Waiter {
  Waiter* next;
  const void* address;
  Queue queue;
  HANDLE event;
};

struct Bucket {
  CRITICAL_SECTION lock;
  Waiter* head;
  Waiter* tail;
};

Bucket g_buckets[kBucketCount];

// 0 = untouched, 1 = in progress, 2 = ready. InitOnceExecuteOnce is itself one
// of the functions winwrap supplies on XP, so this cannot use it without
// depending on the order the two end up initialised in.
volatile LONG g_buckets_state = 0;

DWORD g_thread_event_tls = TLS_OUT_OF_INDEXES;

Bucket* BucketFor(const void* address) {
  for (;;) {
    LONG state = ::InterlockedCompareExchange(&g_buckets_state, 1, 0);
    if (state == 2) {
      break;
    }
    if (state == 0) {
      for (size_t i = 0; i < kBucketCount; ++i) {
        if (!::InitializeCriticalSectionAndSpinCount(&g_buckets[i].lock,
                                                     kBucketSpinCount)) {
          ::RaiseException(STATUS_NO_MEMORY, EXCEPTION_NONCONTINUABLE, 0,
                           nullptr);
        }
      }
      g_thread_event_tls = ::TlsAlloc();
      ::InterlockedExchange(&g_buckets_state, 2);
      break;
    }
    // Another thread is partway through; it only takes a moment.
    ::SwitchToThread();
  }

  // Shift off the bits an aligned address never varies in, then mix: lock
  // addresses are frequently a fixed stride apart inside one object, and the
  // low bits alone would pile those into a single bucket.
  uintptr_t key = reinterpret_cast<uintptr_t>(address) >> 4;
  key *= static_cast<uintptr_t>(0x9E3779B9u);
  return &g_buckets[(key >> 8) & (kBucketCount - 1)];
}

// One auto-reset event per thread, reused for every park. Auto-reset is what
// makes a wake that arrives before the wait harmless: the event stays
// signalled and the wait returns at once.
HANDLE ThreadEvent() {
  HANDLE existing = ::TlsGetValue(g_thread_event_tls);
  if (existing) {
    return existing;
  }
  HANDLE fresh = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fresh) {
    ::RaiseException(STATUS_NO_MEMORY, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return nullptr;
  }
  ::TlsSetValue(g_thread_event_tls, fresh);
  return fresh;
}

void NTAPI OnThreadEvent(PVOID, DWORD reason, PVOID) {
  if (reason != DLL_THREAD_DETACH || g_thread_event_tls == TLS_OUT_OF_INDEXES) {
    return;
  }
  if (HANDLE event = ::TlsGetValue(g_thread_event_tls)) {
    ::CloseHandle(event);
    ::TlsSetValue(g_thread_event_tls, nullptr);
  }
}

// Unlinks `waiter` if it is still queued. Caller holds the bucket.
bool RemoveIfQueued(Bucket* bucket, Waiter* waiter) {
  Waiter* prev = nullptr;
  for (Waiter* node = bucket->head; node; node = node->next) {
    if (node == waiter) {
      if (prev) {
        prev->next = node->next;
      } else {
        bucket->head = node->next;
      }
      if (bucket->tail == node) {
        bucket->tail = prev;
      }
      return true;
    }
    prev = node;
  }
  return false;
}

}  // namespace

ParkResult Park(const void* address,
                Queue queue,
                bool (*validate)(void* context),
                void (*before_sleep)(void* context),
                void* context,
                DWORD timeout_ms) {
  Bucket* bucket = BucketFor(address);
  HANDLE event = ThreadEvent();

  Waiter waiter = {nullptr, address, queue, event};

  ::EnterCriticalSection(&bucket->lock);
  if (validate && !validate(context)) {
    ::LeaveCriticalSection(&bucket->lock);
    return ParkResult::kDeclined;
  }
  // Appended, so waiters of one class are woken oldest first. Not a fairness
  // guarantee -- a thread that has not parked can still take the lock first,
  // exactly as a real SRWLOCK allows -- but it keeps a queued waiter from
  // being passed over indefinitely by its own peers.
  if (bucket->tail) {
    bucket->tail->next = &waiter;
  } else {
    bucket->head = &waiter;
  }
  bucket->tail = &waiter;
  ::LeaveCriticalSection(&bucket->lock);

  // Queued but not yet asleep, which is the only safe moment to let go of
  // whatever lock the caller is holding.
  if (before_sleep) {
    before_sleep(context);
  }

  if (::WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0) {
    return ParkResult::kWoken;
  }

  // The timeout expired, but an unparker may have dequeued this waiter and be
  // about to signal it. Whether we are still queued is the authoritative
  // answer, and the bucket lock is what makes it authoritative.
  ::EnterCriticalSection(&bucket->lock);
  bool still_queued = RemoveIfQueued(bucket, &waiter);
  ::LeaveCriticalSection(&bucket->lock);

  if (still_queued) {
    return ParkResult::kTimedOut;
  }

  // Already dequeued, so a signal is on its way. Consume it, or it would be
  // left pending on this thread's event and swallow the next park.
  ::WaitForSingleObject(event, INFINITE);
  return ParkResult::kWoken;
}

void Unpark(const void* address,
            Queue queue,
            size_t max_to_wake,
            void (*on_dequeued)(void* context, size_t woken, bool any_left),
            void* context) {
  Bucket* bucket = BucketFor(address);

  // Dequeued waiters are moved onto a local chain and signalled after the
  // bucket is free; signalling under it would hand a woken thread a lock it
  // may immediately want back. The chain reuses the nodes' own next pointers,
  // so a broadcast to any number of waiters costs no allocation.
  Waiter* wake_head = nullptr;
  Waiter** wake_tail = &wake_head;
  size_t woken = 0;

  ::EnterCriticalSection(&bucket->lock);

  Waiter* prev = nullptr;
  Waiter* node = bucket->head;
  while (node) {
    Waiter* next = node->next;
    if (node->address == address && node->queue == queue &&
        woken < max_to_wake) {
      if (prev) {
        prev->next = next;
      } else {
        bucket->head = next;
      }
      if (bucket->tail == node) {
        bucket->tail = prev;
      }
      *wake_tail = node;
      wake_tail = &node->next;
      ++woken;
    } else {
      prev = node;
    }
    node = next;
  }
  *wake_tail = nullptr;

  bool any_left = false;
  for (Waiter* n = bucket->head; n; n = n->next) {
    if (n->address == address && n->queue == queue) {
      any_left = true;
      break;
    }
  }

  if (on_dequeued) {
    on_dequeued(context, woken, any_left);
  }
  ::LeaveCriticalSection(&bucket->lock);

  // A waiter's node lives on its own stack, so it can cease to exist the
  // instant its event is signalled. Read everything needed out of the node
  // before signalling it.
  for (Waiter* n = wake_head; n;) {
    Waiter* next = n->next;
    HANDLE event = n->event;
    n = next;
    ::SetEvent(event);
  }
}

}  // namespace winwrap

// Closes this thread's event when the thread goes away. See fls.cc for the
// same mechanism; the loader walks every entry in .CRT$XLB, so both coexist.
#pragma section(".CRT$XLB", long, read)

extern "C" __declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK
    winwrap_parking_lot_tls_callback = winwrap::OnThreadEvent;

#if defined(_M_IX86)
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_winwrap_parking_lot_tls_callback")
#else
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:winwrap_parking_lot_tls_callback")
#endif
