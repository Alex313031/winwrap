# Function list

Every Win32 function winwrap substitutes, and the oldest Windows that exports
it natively. winwrap is only needed below that version — on anything newer the
system's implementation is the better one, and the point of the table is to say
exactly where the line falls.

Since 0.3.0 the library acts on this column itself: each function asks for the
real one and only runs its own code if the answer is no, so the same binary is
correct from XP to 11 and no build flag selects between them. Two groups resolve
all-or-nothing rather than one function at a time, because their arguments mean
different things to each implementation — the SRWLOCK and CONDITION_VARIABLE
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
| `SetProcessDPIAware` | user32 | Vista | Reports success: XP has no DPI virtualisation to opt out of, so the state already holds |
| `RegGetValueW` | advapi32 | Vista | Built on `RegQueryValueExW`: type filter, guaranteed termination, environment expansion |
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
| `CancelIoEx` | kernel32 | Vista | Best effort via `CancelIo`: same-thread only, and cannot target one request |
| `CancelSynchronousIo` | kernel32 | Vista | Always fails with `ERROR_NOT_FOUND`; cross-thread by definition, impossible on XP |
| `GetQueuedCompletionStatusEx` | kernel32 | Vista | `GetQueuedCompletionStatus` in a loop; `fAlertable` ignored, no alertable form exists |
| `SetFileCompletionNotificationModes` | kernel32 | Vista | Always fails with `ERROR_INVALID_FUNCTION`; no XP equivalent. See the libuv note below |
| `GetTickCount64` | kernel32 | Vista | Built on the 32-bit counter plus a wrap extension |
| `InitializeCriticalSectionEx` | kernel32 | Vista | Forwards to `InitializeCriticalSectionAndSpinCount`, flag folded into the spin count |
| `InitOnceExecuteOnce` | kernel32 | Vista | Tagged-pointer state machine. libuv's `uv_once()` is built directly on this |
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
[INCOMPLETE_FUNCS.md](INCOMPLETE_FUNCS.md) — read it before relying on
`CancelIoEx`, `CancelSynchronousIo`, the `Fls*` family's scope, or
`GetQueuedCompletionStatusEx` with `fAlertable`.

## Not yet implemented

Nothing, for Windows XP. The 29 functions a Chromium-based binary statically
imports and XP SP3 does not export are all present above, measured against a
real XP SP3 x86 `kernel32.dll` with Electron Legacy's
`tools/check_win_imports.py`, which lives in that repo rather than this one.

That is a statement about one binary's needs, not about Win32. A different
consumer will import functions this list has never heard of, and adding one is
a file, a declaration, a redirect and a `.def` line. The groundwork that made
the hard ones tractable -- the parking lot behind SRWLOCK and condition
variables, the ntdll resolution behind the file and pipe queries -- is already
here to be reused.
