// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CancelIoEx and CancelSynchronousIo. kernel32, Vista.
//
// Both exist because Vista added the ability to cancel I/O that another thread
// issued. XP cannot do that at all: its CancelIo cancels the calling thread's
// pending I/O on a handle, all of it, and there is no supported way to reach
// another thread's. NtCancelIoFileEx is Vista-and-later too, so dropping to
// ntdll does not help.
//
// What that leaves:
//
//   CancelIoEx          best effort via CancelIo, which is right whenever the
//                       caller is the thread that issued the I/O
//   CancelSynchronousIo nothing; it is cross-thread by definition
//
// Both report failure as ERROR_NOT_FOUND, which is the documented "no matching
// request" result and therefore a value callers already handle. That choice is
// deliberate: libuv, the only caller in this tree, asserts
// `r || GetLastError() == ERROR_NOT_FOUND` at both of its call sites
// (deps/uv/src/win/pipe.c). Returning ERROR_CALL_NOT_IMPLEMENTED instead --
// which is what YY-Thunks returns -- would trip that assertion in a debug
// build while being no more informative.
//
// Known consequence, written down so it is recognised rather than rediscovered:
// uv__pipe_interrupt_read() spins on CancelSynchronousIo for a non-overlapped
// pipe, waiting for a thread-pool thread blocked in ReadFile to acknowledge.
// With no real cancellation that loop ends only when the read completes on its
// own. It is a busy wait with SwitchToThread() rather than a true deadlock,
// and it is reached only for pipes that could not be opened overlapped, but on
// XP it is a hang in everything but name. Queuing an APC does not help: a
// thread blocked in a synchronous ReadFile is not in an alertable wait.

#include "winwrap.h"

extern "C" {

BOOL WINAPI winwrap_CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped) {
  // lpOverlapped is ignored. CancelIo has no way to name one request, so a
  // caller asking for a single operation gets every operation this thread has
  // outstanding on the handle. For the callers that exist that is the same
  // set -- libuv cancels a handle's read while tearing the handle down, and
  // follows it with CancelIo for the writes anyway -- but it is a real
  // difference from the function being stood in for.
  UNREFERENCED_PARAMETER(lpOverlapped);

  if (::CancelIo(hFile)) {
    // Note that this cannot distinguish "cancelled something" from "this
    // thread had nothing outstanding on the handle", because CancelIo reports
    // success for both. The real CancelIoEx would say ERROR_NOT_FOUND for the
    // latter.
    return TRUE;
  }

  // CancelIo failing means the handle is bad or not cancellable, so pass the
  // real reason through rather than inventing one.
  return FALSE;
}

BOOL WINAPI winwrap_CancelSynchronousIo(HANDLE hThread) {
  UNREFERENCED_PARAMETER(hThread);

  // Cancelling a synchronous operation on another thread needs
  // NtCancelSynchronousIoFile, which arrived with Vista alongside this very
  // function. There is nothing to fall back to.
  ::SetLastError(ERROR_NOT_FOUND);
  return FALSE;
}

}  // extern "C"
