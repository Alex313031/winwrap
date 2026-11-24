// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CreateEventExW, CreateSemaphoreExW and CreateWaitableTimerExW. kernel32,
// Vista.
//
// Each is its XP ancestor with two changes: the BOOL parameters became a
// flags word, and the caller chooses the access mask instead of always
// getting everything. The flags translate exactly. The access mask takes a
// second step, because CreateEventW and its siblings hand back a handle with
// *_ALL_ACCESS and XP has no way to ask them for less: the handle is
// duplicated down to the mask the caller named and the original closed, so
// what comes back can do what was asked for and no more. Finish below.
//
// The same helper covers the one case where the old functions fail and the
// new ones would not. A *named* object that already exists is opened rather
// than created, and the old functions open it asking for all access; against
// an object whose security descriptor grants this caller only part of that,
// they fail with ERROR_ACCESS_DENIED where the Vista function, asking only
// for what the caller wanted, succeeds. On that failure the object is opened
// by name with the requested mask.
//
// The one flag with no translation is CREATE_WAITABLE_TIMER_HIGH_RESOLUTION.
// It dates from Windows 10 1803 and every earlier Windows, Vista through
// 1709, rejects it with ERROR_INVALID_PARAMETER -- so that is the answer
// here too, and callers that use it already expect it. V8's
// PreciseSleepTimer, which is why node imports the timer function at all,
// tries the flag and falls back to Sleep when the handle comes back null.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  bool Inheritable(const SECURITY_ATTRIBUTES* attributes) {
    return attributes && attributes->bInheritHandle;
  }

  // Finishes a Create*W call by making the handle's access what the caller
  // asked for. `all_access` is the type's *_ALL_ACCESS, which is what
  // `created` carries; `open` is OpenEventW or one of its siblings.
  HANDLE Finish(HANDLE created,
                HANDLE(WINAPI* open)(DWORD, BOOL, LPCWSTR),
                const SECURITY_ATTRIBUTES* attributes,
                LPCWSTR name,
                DWORD desired_access,
                DWORD all_access) {
    if (!created) {
      if (!name || ::GetLastError() != ERROR_ACCESS_DENIED) {
        return nullptr;
      }
      // Possibly an existing object that grants less than everything. Ask
      // for just the requested access; a handle obtained this way reports
      // ERROR_ALREADY_EXISTS, as the create functions do whenever the object
      // was there first.
      HANDLE opened = open(desired_access, Inheritable(attributes), name);
      // And possibly not: the create may have been denied for another reason
      // -- no right to create in the object directory, say -- in which case
      // the open fails with ERROR_FILE_NOT_FOUND, which is not what happened.
      // Access was denied, whether to the create or to the narrower open.
      ::SetLastError(opened ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED);
      return opened;
    }

    // Nothing to narrow when everything was asked for, which is the usual
    // request. MAXIMUM_ALLOWED is left alone as well: it means "whatever I am
    // entitled to", and CreateEventW succeeding has just shown that to be
    // everything.
    if (desired_access == all_access || (desired_access & MAXIMUM_ALLOWED)) {
      return created;
    }

    // The create's last error is part of its result -- ERROR_ALREADY_EXISTS
    // or zero -- so it is carried across the duplicate. DUPLICATE_CLOSE_SOURCE
    // closes `created` whether or not the duplicate succeeds, so a failure
    // here leaks nothing; it leaves the caller with the duplicate's error and
    // no handle, which is what asking for access that cannot be granted
    // (ACCESS_SYSTEM_SECURITY without the privilege) gets from the real call.
    const DWORD create_error = ::GetLastError();
    const HANDLE process     = ::GetCurrentProcess();
    HANDLE narrowed          = nullptr;
    if (!::DuplicateHandle(process, created, process, &narrowed, desired_access,
                           Inheritable(attributes), DUPLICATE_CLOSE_SOURCE)) {
      return nullptr;
    }
    ::SetLastError(create_error);
    return narrowed;
  }

} // namespace

extern "C" {

HANDLE WINAPI ww_CreateEventExW(LPSECURITY_ATTRIBUTES lpEventAttributes,
                                LPCWSTR lpName,
                                DWORD dwFlags,
                                DWORD dwDesiredAccess) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CreateEventExW)) {
    return system_fn(lpEventAttributes, lpName, dwFlags, dwDesiredAccess);
  }
  if (dwFlags & ~static_cast<DWORD>(CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  HANDLE event = ::CreateEventW(lpEventAttributes, (dwFlags & CREATE_EVENT_MANUAL_RESET) != 0,
                                (dwFlags & CREATE_EVENT_INITIAL_SET) != 0, lpName);
  return Finish(event, ::OpenEventW, lpEventAttributes, lpName, dwDesiredAccess,
                EVENT_ALL_ACCESS);
}

HANDLE WINAPI ww_CreateSemaphoreExW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes,
                                    LONG lInitialCount,
                                    LONG lMaximumCount,
                                    LPCWSTR lpName,
                                    DWORD dwFlags,
                                    DWORD dwDesiredAccess) {
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CreateSemaphoreExW)) {
    return system_fn(lpSemaphoreAttributes, lInitialCount, lMaximumCount, lpName, dwFlags,
                     dwDesiredAccess);
  }
  // Reserved, and required to be zero.
  if (dwFlags) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  HANDLE semaphore =
      ::CreateSemaphoreW(lpSemaphoreAttributes, lInitialCount, lMaximumCount, lpName);
  return Finish(semaphore, ::OpenSemaphoreW, lpSemaphoreAttributes, lpName, dwDesiredAccess,
                SEMAPHORE_ALL_ACCESS);
}

HANDLE WINAPI ww_CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES lpTimerAttributes,
                                        LPCWSTR lpTimerName,
                                        DWORD dwFlags,
                                        DWORD dwDesiredAccess) {
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CreateWaitableTimerExW)) {
    return system_fn(lpTimerAttributes, lpTimerName, dwFlags, dwDesiredAccess);
  }
  // Anything but manual-reset, which takes in the high-resolution flag: see
  // the note at the top.
  if (dwFlags & ~static_cast<DWORD>(CREATE_WAITABLE_TIMER_MANUAL_RESET)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  HANDLE timer = ::CreateWaitableTimerW(
      lpTimerAttributes, (dwFlags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0, lpTimerName);
  return Finish(timer, ::OpenWaitableTimerW, lpTimerAttributes, lpTimerName, dwDesiredAccess,
                TIMER_ALL_ACCESS);
}

} // extern "C"
