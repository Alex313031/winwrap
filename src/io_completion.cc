// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GetQueuedCompletionStatusEx and SetFileCompletionNotificationModes.
// kernel32, Vista.
//
// One of these can be emulated faithfully and the other cannot, which is worth
// stating plainly because the difference decides what callers have to do.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // Dequeues whatever is already queued, without waiting. Returns the count, and
  // leaves the last error as GetQueuedCompletionStatus left it when none came.
  ULONG DrainReady(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG capacity) {
    ULONG removed = 0;
    while (removed < capacity) {
      DWORD bytes             = 0;
      ULONG_PTR key           = 0;
      LPOVERLAPPED overlapped = nullptr;
      BOOL ok                 = ::GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, 0);
      if (!ok && overlapped == nullptr) {
        break;
      }
      entries[removed].lpCompletionKey            = key;
      entries[removed].lpOverlapped               = overlapped;
      entries[removed].dwNumberOfBytesTransferred = bytes;
      entries[removed].Internal                   = overlapped ? overlapped->Internal : 0;
      ++removed;
    }
    return removed;
  }

  // The fAlertable path: alternate between draining the port and sleeping
  // alertably, so queued APCs get a chance to run.
  BOOL AlertableDrain(HANDLE port,
                      LPOVERLAPPED_ENTRY entries,
                      ULONG capacity,
                      PULONG removed_out,
                      DWORD timeout_ms) {
    // Short enough that a completion is not held up noticeably, long enough not
    // to spin the CPU while idle.
    constexpr DWORD kSliceMs = 5;

    const DWORD start = ::GetTickCount();
    for (;;) {
      ULONG removed = DrainReady(port, entries, capacity);
      if (removed != 0) {
        *removed_out = removed;
        ::SetLastError(ERROR_SUCCESS);
        return TRUE;
      }
      // Nothing queued. Anything other than a timeout is a real failure of the
      // port itself and belongs to the caller.
      if (::GetLastError() != WAIT_TIMEOUT) {
        return FALSE;
      }

      DWORD slice = kSliceMs;
      if (timeout_ms != INFINITE) {
        const DWORD elapsed = ::GetTickCount() - start;
        if (elapsed >= timeout_ms) {
          ::SetLastError(WAIT_TIMEOUT);
          return FALSE;
        }
        const DWORD remaining = timeout_ms - elapsed;
        if (remaining < slice) {
          slice = remaining;
        }
      }

      if (::SleepEx(slice, TRUE) == WAIT_IO_COMPLETION) {
        ::SetLastError(WAIT_IO_COMPLETION);
        return FALSE;
      }
    }
  }

} // namespace

extern "C" {

// Dequeues up to ulCount packets. XP has the singular form, so this is it in a
// loop: the first call carries the caller's timeout, and the rest ask for
// whatever is already queued. That is the same shape as the real function,
// which also returns as soon as the port runs dry rather than waiting for
// ulCount packets to accumulate.
//
// The subtlety is telling "a packet arrived" from "nothing arrived", because
// GetQueuedCompletionStatus overloads its return value three ways:
//
//   TRUE,  lpOverlapped != NULL   an operation completed
//   TRUE,  lpOverlapped == NULL   a packet posted by hand, no operation
//   FALSE, lpOverlapped != NULL   an operation completed, and it failed
//   FALSE, lpOverlapped == NULL   nothing was dequeued
//
// Only the last is an empty hand. Treating FALSE as failure would drop failed
// I/O on the floor, and treating a NULL overlapped as empty would swallow the
// wake-up packets libuv posts to break its loop out of a wait.
//
// fAlertable has no direct support to build on -- GetQueuedCompletionStatus is
// never alertable -- so it is approximated: poll the port without waiting,
// then give up the rest of the slice in SleepEx, which is alertable. An APC
// that runs ends the call with WAIT_IO_COMPLETION, as the real function does.
// The cost is that a packet arriving mid-slice waits out the remainder, so the
// alertable path trades a few milliseconds of latency for the ability to
// deliver APCs at all. Both callers in Electron pass FALSE and never reach it.
BOOL WINAPI winwrap_GetQueuedCompletionStatusEx(HANDLE CompletionPort,
                                                LPOVERLAPPED_ENTRY lpCompletionPortEntries,
                                                ULONG ulCount,
                                                PULONG ulNumEntriesRemoved,
                                                DWORD dwMilliseconds,
                                                BOOL fAlertable) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WINWRAP_SYSTEM(::winwrap::Module::hKernel32, GetQueuedCompletionStatusEx)) {
    return system_fn(CompletionPort, lpCompletionPortEntries, ulCount, ulNumEntriesRemoved,
                     dwMilliseconds, fAlertable);
  }
  if (!lpCompletionPortEntries || !ulNumEntriesRemoved || ulCount == 0) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  *ulNumEntriesRemoved = 0;

  if (fAlertable) {
    return AlertableDrain(CompletionPort, lpCompletionPortEntries, ulCount, ulNumEntriesRemoved,
                          dwMilliseconds);
  }

  ULONG removed = 0;
  DWORD timeout = dwMilliseconds;

  for (ULONG i = 0; i < ulCount; ++i) {
    DWORD bytes             = 0;
    ULONG_PTR key           = 0;
    LPOVERLAPPED overlapped = nullptr;

    BOOL ok = ::GetQueuedCompletionStatus(CompletionPort, &bytes, &key, &overlapped, timeout);
    if (!ok && overlapped == nullptr) {
      // Nothing came out. If this was the blocking first pass, the caller gets
      // the real reason -- WAIT_TIMEOUT, or a genuine error, which libuv
      // distinguishes and treats the second as fatal. Otherwise the port is
      // simply drained and what we already have is the answer.
      if (removed == 0) {
        return FALSE;
      }
      break;
    }

    lpCompletionPortEntries[i].lpCompletionKey            = key;
    lpCompletionPortEntries[i].lpOverlapped               = overlapped;
    lpCompletionPortEntries[i].dwNumberOfBytesTransferred = bytes;
    // The real entry mirrors the status out of the OVERLAPPED, which already
    // holds it. A hand-posted packet has no OVERLAPPED and so no status.
    lpCompletionPortEntries[i].Internal = overlapped ? overlapped->Internal : 0;

    ++removed;
    timeout = 0; // Everything after the first is a drain, never a wait.
  }

  *ulNumEntriesRemoved = removed;
  // Do not leave the WAIT_TIMEOUT from the drain loop behind a TRUE return.
  ::SetLastError(ERROR_SUCCESS);
  return TRUE;
}

// Neither flag can be honoured -- both change how the kernel associates a
// handle with a completion port, and XP's I/O manager has no such switch --
// but they are not equally dangerous to pretend about, so they are answered
// differently.
//
// FILE_SKIP_SET_EVENT_ON_HANDLE only suppresses signalling the handle's own
// event on completion. A caller that gets the signalling it asked to skip is
// not harmed by it; the event is simply set and ignored. Reporting success is
// safe, and it matters: c-ares requests this flag alone and treats failure as
// fatal to its whole event backend.
//
// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is the opposite. Claiming success tells
// the caller that a synchronously completed operation will *not* queue a
// packet, so it stops waiting for one and may free the OVERLAPPED -- and then
// XP queues the packet anyway, into a structure that no longer exists.
// Reporting failure costs an optimisation; reporting success costs a wild
// pointer. YY-Thunks arrived at the same split after a bug report
// (Chuyu-Team/YY-Thunks#130).
//
// The failure is ERROR_INVALID_FUNCTION because that is what callers test for:
// libuv's UDP path treats exactly that code as "this handle does not support
// the optimisation" and carries on without it (deps/uv/src/win/udp.c). Its TCP
// path has no such tolerance and still needs a patch of its own.
BOOL WINAPI winwrap_SetFileCompletionNotificationModes(HANDLE FileHandle, UCHAR Flags) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WINWRAP_SYSTEM(::winwrap::Module::hKernel32, SetFileCompletionNotificationModes)) {
    return system_fn(FileHandle, Flags);
  }
  UNREFERENCED_PARAMETER(FileHandle);

  if ((Flags & ~FILE_SKIP_SET_EVENT_ON_HANDLE) == 0) {
    return TRUE;
  }

  ::SetLastError(ERROR_INVALID_FUNCTION);
  return FALSE;
}

} // extern "C"
