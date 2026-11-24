# Function list

Every Win32 function winwrap substitutes, and the oldest Windows that exports
it natively. winwrap is only needed below that version; on anything newer the
system's implementation is the better one, and the point of the table is to say
exactly where the line falls.

Since 0.3.0 the library acts on this column itself: each function asks for the
real one and only runs its own code if the answer is no, so the same binary is
correct from XP to 11 and no build flag selects between them. Two groups resolve
all-or-nothing rather than one function at a time, because their arguments mean
different things to each implementation: the SRWLOCK and CONDITION_VARIABLE
functions together, which therefore only defer from Windows 7 (Vista has no
`TryAcquireSRWLock*`, and no `RtlTryAcquireSRWLock*` under it either), and the
`Fls*` four.

## Implemented

| Function | DLL | Native since | Notes |
|---|---|---|---|
| `CreateSymbolicLinkW` | kernel32 | Vista | Always fails with `ERROR_NOT_SUPPORTED`; XP has no symbolic links to create |
| `NeedCurrentDirectoryForExePathW` | kernel32 | Server 2003 | Implemented in full; the rule is `NoDefaultCurrentDirectoryInExePath` plus a backslash test |
| `GetFileInformationByHandleEx` | kernel32 | Vista | `NtQueryInformationFile`, plus `NtQueryDirectoryFile` for the enumeration classes and a synthesised `FileIdInfo` |
| `SetFileInformationByHandle` | kernel32 | Vista | `NtSetInformationFile`; the rename class translates its DOS path to an NT one |
| `GetFinalPathNameByHandleW` | kernel32 | Vista | `NtQueryObject` plus a device-to-drive map. `VOLUME_NAME_DOS` and `VOLUME_NAME_NT` only |
| `ReOpenFile` | kernel32 | Server 2003 | `NtCreateFile` relative to the original handle |
| `GetNamedPipeClientProcessId` | kernel32 | Vista | Asks npfs through the control code the real function uses; fails where the kernel has no answer |
| `GetNamedPipeServerProcessId` | kernel32 | Vista | Always fails on XP: npfs records neither party and no inference is safe |
| `GetCurrentProcessorNumber` | kernel32 | Server 2003 | From the CPUID initial APIC ID; a hint, as the real one is too |
| `GetLogicalProcessorInformation` | kernel32 | XP SP3 | Hands over to the system where it exists. Where it does not, builds the result from `GetSystemInfo`: the processor count is exact, every processor is reported as its own core with no SMT, and no caches are reported. The only entry here whose floor is a service pack rather than a Windows version -- it is what stops a binary loading on SP2 |
| `SetProcessDPIAware` | user32 | Vista | Reports success: XP has no DPI virtualisation to opt out of, so the state already holds |
| `RegGetValueW` | advapi32 | Vista | Built on `RegQueryValueExW`: type filter, guaranteed termination, environment expansion |
| `EventSetInformation` | advapi32 | 8 | Always `ERROR_NOT_SUPPORTED`; XP has no manifest-based provider to describe, and Vista and 7 have the rest of the family but not this |
| `EventWriteTransfer` | advapi32 | Vista | Always `ERROR_INVALID_HANDLE`; XP's ETW has no manifest-based provider model to write into |
| `GetErrorMode` | kernel32 | Vista | Read from the process information NT already keeps, with the low bit inverted |
| `RegisterApplicationRestart` | kernel32 | Vista | Always `E_NOTIMPL`; XP has no Restart Manager |
| `UnregisterApplicationRestart` | kernel32 | Vista | Always `E_NOTIMPL`, to agree with its other half |
| `ConvertThreadToFiberEx` | kernel32 | Vista | Defers to the real one where it exists; otherwise `ConvertThreadToFiber`, dropping `FIBER_FLAG_FLOAT_SWITCH` |
| `CreateFiberEx` | kernel32 | XP (flag: Vista) | Present on XP but refuses `FIBER_FLAG_FLOAT_SWITCH`; retried without it rather than failing |
| `IsThreadAFiber` | kernel32 | Vista | Tests the TEB field `GetCurrentFiber()` reads against the 0x1E00 non-fiber sentinel |
| `InitializeConditionVariable` | kernel32 | Vista | No state: the variable's address is the parking-lot key, so the word stays zero |
| `SleepConditionVariableCS` | kernel32 | Vista | Queues before releasing the lock, which is what makes the wake-up unlosable |
| `SleepConditionVariableSRW` | kernel32 | Vista | Honours `CONDITION_VARIABLE_LOCKMODE_SHARED`; re-acquires in the same mode |
| `WakeConditionVariable` | kernel32 | Vista | |
| `WakeAllConditionVariable` | kernel32 | Vista | |
| `CompareStringOrdinal` | kernel32 | Vista | Written out; binary comparison is not something `CompareStringW` can be asked for |
| `GetLocaleInfoEx` | kernel32 | Vista | Resolves the locale name to an LCID by enumeration, then `GetLocaleInfoW` |
| `LCMapStringEx` | kernel32 | Vista | The same name-to-LCID resolution, then `LCMapStringW`. Refuses a sort handle or a sort version, which XP has neither of |
| `CompareStringEx` | kernel32 | Vista | The same name-to-LCID resolution, then `CompareStringW`. MSVC's own standard library imports this, so any C++ consumer needs it |
| `LocaleNameToLCID` | kernel32 | Vista | The name-to-LCID half of this family, which is the `LcidForName` the three above already needed. Language and country only -- not the script- or sort-suffixed names, see INCOMPLETE_FUNCS.md |
| `LCIDToLocaleName` | kernel32 | Vista | The other half, rebuilt from `LOCALE_SISO639LANGNAME` plus `LOCALE_SISO3166CTRYNAME`, with the same limit. Accepts `LOCALE_ALLOW_NEUTRAL_NAMES`, which it satisfies by construction; on Vista, whose own function rejects the flag, the call is retried without it, and `LocaleNameToLCID` does the same |
| `ResolveLocaleName` | kernel32 | **7** | The two above composed, so it normalises a name the system has. It does not walk up a tag it does not have -- see INCOMPLETE_FUNCS.md |
| `GetDateFormatEx` | kernel32 | Vista | The same name-to-LCID resolution, then `GetDateFormatW`. Requires a null `lpCalendar`, which is all the real function accepts -- it is documented as reserved. `DATE_USE_ALT_CALENDAR` is what actually selects a calendar, and XP has it |
| `GetTimeFormatEx` | kernel32 | Vista | The same resolution, then `GetTimeFormatW`; every other parameter means the same to both |
| `GetNumberFormatEx` | kernel32 | Vista | The same resolution, then `GetNumberFormatW` |
| `GetCurrencyFormatEx` | kernel32 | Vista | The same resolution, then `GetCurrencyFormatW`. ICU reaches for this pair through winnmfmt.cpp |
| `CancelIoEx` | kernel32 | Vista | Best effort via `CancelIo`: same-thread only, and cannot target one request |
| `CancelSynchronousIo` | kernel32 | Vista | Always fails with `ERROR_NOT_FOUND`; cross-thread by definition, impossible on XP |
| `GetQueuedCompletionStatusEx` | kernel32 | Vista | `GetQueuedCompletionStatus` in a loop; `fAlertable` ignored, no alertable form exists |
| `SetFileCompletionNotificationModes` | kernel32 | Vista | Always fails with `ERROR_INVALID_FUNCTION`; no XP equivalent. See the libuv note below |
| `GetTickCount64` | kernel32 | Vista | Built on the 32-bit counter plus a wrap extension |
| `InitializeCriticalSectionEx` | kernel32 | Vista | Forwards to `InitializeCriticalSectionAndSpinCount`, flag folded into the spin count |
| `CreateEventExW` | kernel32 | Vista | `CreateEventW` with the two flags translated, then the handle duplicated down to the requested access mask. An existing named object that refuses full access is opened with the requested mask instead |
| `CreateSemaphoreExW` | kernel32 | Vista | `CreateSemaphoreW`, same access-mask treatment. The reserved flags word must be zero |
| `CreateWaitableTimerExW` | kernel32 | Vista | `CreateWaitableTimerW`, same access-mask treatment. `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` is refused with `ERROR_INVALID_PARAMETER`, as on every Windows before 10 1803 -- see INCOMPLETE_FUNCS.md |
| `FlushProcessWriteBuffers` | kernel32 | Vista | Provokes the interprocessor interrupt by revoking write access to a page it has just written to. The MSVC Concurrency Runtime is the caller |
| `QueryThreadCycleTime` | kernel32 | Vista | `GetThreadTimes`, kernel plus user, scaled to cycles at the processor's nominal rate. Clock-tick resolution -- see INCOMPLETE_FUNCS.md |
| `GetDynamicTimeZoneInformation` | kernel32 | Vista | `GetTimeZoneInformation`, plus the zone's registry key name found by search and the automatic-DST setting read from where XP keeps it. ICU derives the default time zone from the key name |
| `SHGetKnownFolderPath` | shell32 | Vista | The 44 folder IDs that have a CSIDL (46 on x64), through `SHGetFolderPathW`. Folders Vista introduced fail with `ERROR_FILE_NOT_FOUND`. Loads shell32 and ole32 -- see INCOMPLETE_FUNCS.md |
| `ConvertInterfaceIndexToLuid` | iphlpapi | Vista | `GetIfEntry` for the interface's type; the index stands in for the LUID's own number, XP having no LUIDs. Loads iphlpapi -- see INCOMPLETE_FUNCS.md |
| `ConvertInterfaceLuidToNameW` | iphlpapi | Vista | Formats `<type>_<number>` the way Windows does, `iftype<N>_<number>` for a type without a name. With the one above, this is libuv's `uv_if_indextoname()` |
| `InitOnceExecuteOnce` | kernel32 | Vista | Tagged-pointer state machine with ntdll's state numbering; the callback's context is stored and handed to later callers. Waiters sleep in the parking lot. libuv's `uv_once()` is built directly on this |
| `InitOnceBeginInitialize` | kernel32 | Vista | The same state word, so it mixes freely with the other two. Synchronous, `INIT_ONCE_ASYNC` and `INIT_ONCE_CHECK_ONLY` are all supported |
| `InitOnceComplete` | kernel32 | Vista | Stores the context and wakes the waiters; `INIT_ONCE_INIT_FAILED` hands the `INIT_ONCE` back for another attempt. The losing asynchronous completion fails, as on Windows |
| `CreateThreadpoolTimer` | kernel32 | Vista | Allocates winwrap's own `TP_TIMER`; the callback pool is `CreateTimerQueueTimer`'s. Handles do not cross modules -- see INCOMPLETE_FUNCS.md |
| `SetThreadpoolTimer` | kernel32 | Vista | `CreateTimerQueueTimer` or `ChangeTimerQueueTimer`. A null due time cancels, and does so synchronously so no queued callback slips through |
| `WaitForThreadpoolTimerCallbacks` | kernel32 | Vista | Waits on an idle event the trampoline maintains, since the legacy API has no wait of its own short of deleting the timer |
| `CloseThreadpoolTimer` | kernel32 | Vista | Waits for a running callback rather than racing it. Called from inside one, it hands the teardown to a work item, which is the only context able to wait for the dispatcher |
| `CreateThreadpoolWait` | kernel32 | Vista | Allocates winwrap's own `TP_WAIT`; same module caveat as the timer |
| `SetThreadpoolWait` | kernel32 | Vista | `RegisterWaitForSingleObject` with `WT_EXECUTEONLYONCE`, which is the Vista object's one-shot contract rather than a simplification |
| `WaitForThreadpoolWaitCallbacks` | kernel32 | Vista | The wait's counterpart to the timer's, on the same idle event |
| `CloseThreadpoolWait` | kernel32 | Vista | `UnregisterWaitEx`, waiting for the callback unless called from it, which takes the same work item as the timer |
| `FreeLibraryWhenCallbackReturns` | kernel32 | Vista | Records the module against the running callback, and the trampoline unloads it once that callback returns -- the deferral is what makes the function worth having. Per callback, so two concurrent ones do not overwrite each other |
| `InitializeSRWLock` | kernel32 | Vista | Real reader/writer lock; whole state is the one pointer, waiters park by address |
| `AcquireSRWLockExclusive` | kernel32 | Vista | |
| `ReleaseSRWLockExclusive` | kernel32 | Vista | |
| `AcquireSRWLockShared` | kernel32 | Vista | Genuinely concurrent with other readers; writers are preferred so they cannot starve |
| `ReleaseSRWLockShared` | kernel32 | Vista | |
| `TryAcquireSRWLockExclusive` | kernel32 | **Windows 7** | Vista has the rest of the family but not this |
| `TryAcquireSRWLockShared` | kernel32 | **Windows 7** | Likewise |
| `FlsAlloc` | kernel32 | Server 2003 / Vista | FLS on TLS, with thread-exit callbacks driven by a PE TLS callback |
| `FlsFree` | kernel32 | Server 2003 / Vista | Runs the callback for every thread holding a value, as Windows does per fiber |
| `FlsGetValue` | kernel32 | Server 2003 / Vista | |
| `FlsSetValue` | kernel32 | Server 2003 / Vista | |
| `GetCurrentProcessExplicitAppUserModelID` | shell32 | Windows 7 | Reports no ID set; Vista has no AppUserModelID concept to report |
| `SetCurrentProcessExplicitAppUserModelID` | shell32 | Windows 7 | Accepts and discards, but rejects a null ID like the real one |

### Not everything here succeeds

Most of the above is a full implementation and a caller cannot tell the
difference. Some of it cannot be, either because XP has nothing to build on or
because the honest answer is an error. Those are listed separately, with what
each one does instead and what it costs, in
[INCOMPLETE_FUNCS.md](INCOMPLETE_FUNCS.md); read it before relying on
`CancelIoEx`, `CancelSynchronousIo`, the `Fls*` family's scope, or
`GetQueuedCompletionStatusEx` with `fAlertable`.

## Not yet implemented

Nothing, for Windows XP. The 29 functions a Chromium-based binary statically
imports and XP SP3 does not export are all present above, measured against a
real XP SP3 x86 `kernel32.dll` with Electron Legacy's
`tools/check_win_imports.py`, which lives in that repo rather than this one.

Note the floor that was measured against: XP **SP3**. SP2 is a shorter
list but not an empty one -- `GetLogicalProcessorInformation` above is
the first of it, and the method for finding the rest is the same
`check_imports.py` run against an SP2 `kernel32.dll`.

That is a statement about one binary's needs, not about Win32. A different
consumer will import functions this list has never heard of, and adding one is
a file, a declaration, a redirect and a `.def` line. The groundwork that made
the hard ones tractable -- the parking lot behind SRWLOCK and condition
variables, the ntdll resolution behind the file and pipe queries -- is already
here to be reused.
