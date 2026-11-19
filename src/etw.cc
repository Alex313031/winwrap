// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// EventWriteTransfer. advapi32, Vista.
//
// This is the manifest-based ETW provider API from evntprov.h -- the one
// mc.exe generates calls to from a compiled manifest. XP has only the classic
// model from evntrace.h: RegisterTraceGuids, GetTraceLoggerHandle, TraceEvent.
// The two are not variations on one another. A classic provider is identified
// by a GUID registered at run time and writes a flat event structure; a
// manifest provider is described by a manifest the system must already have
// been told about, and its events carry a descriptor naming fields in it.
// There is nothing on XP for a manifest event to be written to, so this one
// cannot be emulated, only failed honestly -- see docs/INCOMPLETE_FUNCS.md.
//
// ERROR_INVALID_HANDLE rather than ERROR_NOT_SUPPORTED, because on XP the
// RegHandle genuinely is invalid: EventRegister is absent as well, so nothing
// can ever have produced a valid one, and an invalid handle is exactly the
// condition the real function reports this way. A caller already has to
// tolerate that return, which is not true of a code it has never seen.
//
// On which import this is, and which it is not. Chromium's base already
// intercepts EventRegister, EventWrite and EventUnregister itself, in
// trace_event_etw_export_win.cc: it defines functions with those names,
// resolves the real ones with GetProcAddress, and no-ops when they are
// absent. That was written when M49 still supported XP and it was sufficient,
// because the mc.exe of the day generated calls to EventWrite. The current
// Windows SDK's mc.exe generates EventWriteTransfer instead, which that shim
// does not cover -- so the import falls through to advapi32 and the process
// dies at load.
//
// EventSetInformation is here for the same reason, two Windows later. It
// arrived in Windows 8, so Vista AND Windows 7 both have the rest of the
// manifest-based provider API from advapi32 and lack only this one -- which
// is exactly how it presented: node.exe started on XP's successor only to
// stop at a single missing entry point. So this slot is live on three
// Windows versions rather than the one EventWriteTransfer covers.
//
// The remaining siblings -- EventRegister, EventWrite and EventUnregister --
// are deliberately left alone. A consumer that intercepts them itself, as
// Chromium's base does in trace_event_etw_export_win.cc, would otherwise find
// winwrap competing for the same symbols, and whichever won would be decided
// by link order rather than by intent. Add them only for a consumer that
// imports them directly and does not shim them itself.

#include "dispatch.h"
#include "winwrap.h"

extern "C" {

ULONG WINAPI ww_EventWriteTransfer(REGHANDLE RegHandle,
                                   const void* EventDescriptor,
                                   LPCGUID ActivityId,
                                   LPCGUID RelatedActivityId,
                                   ULONG UserDataCount,
                                   const void* UserData) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hAdvapi32, EventWriteTransfer)) {
    return system_fn(RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount,
                     UserData);
  }
  return ERROR_INVALID_HANDLE;
}

ULONG WINAPI ww_EventSetInformation(REGHANDLE RegHandle,
                                    int InformationClass,
                                    void* EventInformation,
                                    ULONG InformationLength) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hAdvapi32, EventSetInformation)) {
    return system_fn(RegHandle, InformationClass, EventInformation, InformationLength);
  }
  // ERROR_NOT_SUPPORTED rather than the ERROR_INVALID_HANDLE above, and the
  // difference is which Windows each sibling went missing on.
  // EventWriteTransfer can say the handle is invalid because EventRegister is
  // absent on XP too, so nothing there can ever have produced a valid one.
  // That reasoning does not carry here: this one did not arrive until
  // Windows 8, and Vista and 7 both have a real EventRegister, so a caller
  // reaching this is quite likely holding a perfectly good handle, and
  // calling it invalid would be a lie about the one thing it could check.
  //
  // What is actually true is that this Windows cannot set provider
  // information, which is what ERROR_NOT_SUPPORTED says. It is also a code
  // the real function already returns for an InformationClass it does not
  // recognise, so a caller that handles this function failing at all handles
  // this.
  return ERROR_NOT_SUPPORTED;
}

} // extern "C"
