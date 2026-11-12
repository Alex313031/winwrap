// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GetQueuedCompletionStatusEx and SetFileCompletionNotificationModes.
// kernel32, Vista.
//
// One of these can be emulated faithfully and the other cannot, which is worth
// stating plainly because the difference decides what callers have to do.

#include "winwrap.h"

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
// fAlertable is ignored: there is no alertable form of
// GetQueuedCompletionStatus to build one from. Both callers in Electron --
// libuv's loop and c-ares' event system -- pass FALSE.
BOOL WINAPI winwrap_GetQueuedCompletionStatusEx(
    HANDLE CompletionPort,
    LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    ULONG ulCount,
    PULONG ulNumEntriesRemoved,
    DWORD dwMilliseconds,
    BOOL fAlertable) {
  UNREFERENCED_PARAMETER(fAlertable);

  if (!lpCompletionPortEntries || !ulNumEntriesRemoved || ulCount == 0) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  *ulNumEntriesRemoved = 0;

  ULONG removed = 0;
  DWORD timeout = dwMilliseconds;

  for (ULONG i = 0; i < ulCount; ++i) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = nullptr;

    BOOL ok = ::GetQueuedCompletionStatus(CompletionPort, &bytes, &key,
                                          &overlapped, timeout);
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

    lpCompletionPortEntries[i].lpCompletionKey = key;
    lpCompletionPortEntries[i].lpOverlapped = overlapped;
    lpCompletionPortEntries[i].dwNumberOfBytesTransferred = bytes;
    // The real entry mirrors the status out of the OVERLAPPED, which already
    // holds it. A hand-posted packet has no OVERLAPPED and so no status.
    lpCompletionPortEntries[i].Internal =
        overlapped ? overlapped->Internal : 0;

    ++removed;
    timeout = 0;  // Everything after the first is a drain, never a wait.
  }

  *ulNumEntriesRemoved = removed;
  // Do not leave the WAIT_TIMEOUT from the drain loop behind a TRUE return.
  ::SetLastError(ERROR_SUCCESS);
  return TRUE;
}

// Cannot be emulated. Both flags change how the kernel associates a handle
// with a completion port -- whether a synchronous completion still queues a
// packet, and whether the handle's event is signalled -- and XP's I/O manager
// has no such switch. There is nothing to call and nothing to approximate.
//
// ERROR_INVALID_FUNCTION rather than ERROR_CALL_NOT_IMPLEMENTED, because it is
// what callers already test for: libuv's UDP path treats exactly that code as
// "this handle does not support the optimisation" and carries on without it
// (deps/uv/src/win/udp.c). Any other code is propagated as a hard error there.
//
// libuv's TCP path has no such tolerance -- uv__tcp_set_socket() returns the
// error, failing socket creation outright -- so it needs a patch of its own to
// skip the optimisation the way the UDP path does. Reporting success instead
// would be far worse than failing: libuv would set UV_HANDLE_SYNC_BYPASS_IOCP
// and stop expecting a completion packet for a synchronous read, while XP
// would keep posting one, and every such read would be processed twice.
BOOL WINAPI winwrap_SetFileCompletionNotificationModes(HANDLE FileHandle,
                                                       UCHAR Flags) {
  UNREFERENCED_PARAMETER(FileHandle);
  UNREFERENCED_PARAMETER(Flags);

  ::SetLastError(ERROR_INVALID_FUNCTION);
  return FALSE;
}

}  // extern "C"
