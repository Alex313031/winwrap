// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The manifest-based ETW provider functions: EventRegister, EventUnregister,
// EventWriteTransfer and EventSetInformation. advapi32, Vista, except the
// last, which is Windows 8.
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
// RegHandle genuinely is invalid: EventRegister never succeeds there, so
// nothing can ever have produced a valid one, and an invalid handle is exactly the
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
// EventRegister and EventUnregister were once deliberately left out, on the
// grounds that a consumer intercepting them itself, as that Chromium file
// does, would find winwrap competing for the same symbols. That was more
// caution than the mechanism warrants. A consumer that defines a function
// called EventRegister defines the name; what winwrap defines, outside MinGW,
// is the `__imp_` slot, a different symbol, which only a caller going through
// the import table ever references. The two do not meet -- and a caller that
// does go through the import table is exactly the one with nothing protecting
// it. V8's TraceLogging providers were that caller: with these two absent
// there was no way to link them for XP at all, and node's
// 16-v8-no-etw-legacy-windows.patch compiles them out instead.
//
// EventWrite is still not here. Nothing has turned up that imports it.

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
  // EventWriteTransfer can say the handle is invalid because EventRegister
  // never succeeds on XP, so nothing there can ever have produced a valid one.
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

// Register a provider. Fails, and that is the whole of what can be said: a
// manifest provider registers with a session infrastructure XP does not have.
//
// ERROR_NOT_SUPPORTED, for the reason EventSetInformation gives -- it is what
// is true, this Windows cannot do it -- and not ERROR_INVALID_HANDLE, there
// being no handle yet to call invalid.
//
// The handle is zeroed on the way out, and that is what makes the rest of the
// family hang together. A caller that ignores the result, as TraceLogging's
// registration macro lets it, goes on to write and unregister with whatever
// is in that variable; zero is the value every sibling here, and the real
// functions too, already turn away as ERROR_INVALID_HANDLE.
//
// EnableCallback is a PENABLECALLBACK and declared as a plain pointer, for
// the reason the descriptors above are: it is never called from here, only
// forwarded, and evntprov.h is not visible at the level winwrap compiles at.
ULONG WINAPI ww_EventRegister(LPCGUID ProviderId,
                              PVOID EnableCallback,
                              PVOID CallbackContext,
                              REGHANDLE* RegHandle) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hAdvapi32, EventRegister)) {
    return system_fn(ProviderId, EnableCallback, CallbackContext, RegHandle);
  }
  if (!ProviderId || !RegHandle) {
    return ERROR_INVALID_PARAMETER;
  }
  *RegHandle = 0;
  return ERROR_NOT_SUPPORTED;
}

// Unregister one. ERROR_INVALID_HANDLE, as EventWriteTransfer says and for
// its reason: where this code runs, EventRegister above is the only source of
// handles and it has never issued one.
ULONG WINAPI ww_EventUnregister(REGHANDLE RegHandle) {
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hAdvapi32, EventUnregister)) {
    return system_fn(RegHandle);
  }
  return ERROR_INVALID_HANDLE;
}

} // extern "C"
