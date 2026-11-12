# Function list

Every Win32 function winwrap substitutes, and the oldest Windows that exports
it natively. winwrap is only needed below that version — on anything newer the
system's implementation is the better one, and the point of the table is to say
exactly where the line falls.

## Implemented

| Function | DLL | Native since | Notes |
|---|---|---|---|
| `ConvertThreadToFiberEx` | kernel32 | Vista | Forwards to `ConvertThreadToFiber`; `FIBER_FLAG_FLOAT_SWITCH` is accepted and ignored |
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

### FLS is per thread here, not per fiber

The one semantic difference. A program that converts threads to fibers and
expects each fiber to see its own FLS values will not get that. Neither
Chromium nor the CRT does this, and faking it would mean intercepting the whole
fiber API.

### SetFileCompletionNotificationModes needs a libuv patch

Reporting success would be far worse than failing. libuv would set
`UV_HANDLE_SYNC_BYPASS_IOCP` and stop expecting a completion packet for a
synchronous read, while XP keeps posting one, so every such read is processed
twice. Failing is the honest answer.

libuv's UDP path already tolerates it, skipping the optimisation when the error
is `ERROR_INVALID_FUNCTION` -- which is why that is the code returned. Its TCP
path does not: `uv__tcp_set_socket()` returns the error and fails socket
creation outright, so it needs a patch to behave like the UDP path.

### The SRWLOCK family is all-or-nothing

Do not redirect part of it. Vista exports everything except the two
`TryAcquire` variants, and redirecting only those would point a
`CRITICAL_SECTION`-based `TryAcquire` at a lock whose `Ptr` field the real Vista
implementation owns, reading its internals as if they were our pointer. Either
the whole family is winwrap's or all of it is the system's.

That is why a Vista target is better served by teaching the caller that the
`TryAcquire` variants can be absent than by linking winwrap for them.

## Not yet implemented

What is still missing for Windows XP, measured against a real XP SP3 x86
`kernel32.dll`. These are all static imports, so every one has to exist before a
Chromium-based binary will load at all — there is no partial progress to
observe.

| Group | Functions |
|---|---|
| File information | `GetFileInformationByHandleEx`, `SetFileInformationByHandle`, `GetFinalPathNameByHandleW`, `ReOpenFile` |
| Named pipes | `GetNamedPipeClientProcessId`, `GetNamedPipeServerProcessId` |
| Misc | `GetCurrentProcessorNumber`, `GetErrorMode`, `CreateSymbolicLinkW`, `NeedCurrentDirectoryForExePathW`, `RegisterApplicationRestart` |

The `Fls*` set and `InitializeCriticalSectionEx`, both implemented above, are
pulled in by the MSVC static CRT itself rather than by any application code. An
XP target needs them whatever the program does -- including `winwrap.dll`,
which links that same CRT and so cannot load on XP without redirecting its own
calls into itself.
