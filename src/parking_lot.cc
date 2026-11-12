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

}  // namespace

bool Park(const void* address, Queue queue, bool (*validate)(void* context),
          void* context) {
  Bucket* bucket = BucketFor(address);
  HANDLE event = ThreadEvent();

  Waiter waiter = {nullptr, address, queue, event};

  ::EnterCriticalSection(&bucket->lock);
  if (!validate(context)) {
    ::LeaveCriticalSection(&bucket->lock);
    return false;
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

  ::WaitForSingleObject(event, INFINITE);
  return true;
}

void Unpark(const void* address, Queue queue,
            void (*on_dequeued)(void* context, size_t woken, bool any_left),
            void* context) {
  Bucket* bucket = BucketFor(address);

  // At most one for kExclusive; kShared wakes every reader, and the count is
  // bounded by the threads that exist, so collect handles on the way through
  // and signal them after the bucket is free. Signalling under the lock would
  // hand a woken thread a bucket it immediately needs.
  constexpr size_t kInlineWakes = 16;
  HANDLE inline_events[kInlineWakes];
  HANDLE* events = inline_events;
  size_t capacity = kInlineWakes;
  size_t woken = 0;
  bool overflowed = false;

  ::EnterCriticalSection(&bucket->lock);

  Waiter* prev = nullptr;
  Waiter* node = bucket->head;
  while (node) {
    Waiter* next = node->next;
    bool take = node->address == address && node->queue == queue &&
                (queue == Queue::kShared || woken == 0);
    if (take) {
      if (woken == capacity) {
        // More readers than fit inline. Rather than allocate while holding a
        // bucket -- which could reenter the allocator's own locks -- stop
        // here and let the remaining readers be woken by whoever releases
        // next; any_left below reports them, so the bit stays set.
        overflowed = true;
        break;
      }
      events[woken++] = node->event;
      if (prev) {
        prev->next = next;
      } else {
        bucket->head = next;
      }
      if (bucket->tail == node) {
        bucket->tail = prev;
      }
    } else {
      prev = node;
    }
    node = next;
  }

  bool any_left = overflowed;
  if (!any_left) {
    for (Waiter* n = bucket->head; n; n = n->next) {
      if (n->address == address && n->queue == queue) {
        any_left = true;
        break;
      }
    }
  }

  if (on_dequeued) {
    on_dequeued(context, woken, any_left);
  }
  ::LeaveCriticalSection(&bucket->lock);

  for (size_t i = 0; i < woken; ++i) {
    ::SetEvent(events[i]);
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
