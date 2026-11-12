# Function list

Every Win32 function winwrap substitutes, and the oldest Windows that exports
it natively. winwrap is only needed below that version — on anything newer the
system's implementation is the better one, and the point of the table is to say
exactly where the line falls.

## Implemented

| Function | DLL | Native since | Notes |
|---|---|---|---|
| `GetTickCount64` | kernel32 | Vista | Built on the 32-bit counter plus a wrap extension |
| `InitOnceExecuteOnce` | kernel32 | Vista | Tagged-pointer state machine. libuv's `uv_once()` is built directly on this |
| `InitializeSRWLock` | kernel32 | Vista | SRWLOCK on a lazily allocated `CRITICAL_SECTION` in the lock's `Ptr` field |
| `AcquireSRWLockExclusive` | kernel32 | Vista | |
| `ReleaseSRWLockExclusive` | kernel32 | Vista | |
| `AcquireSRWLockShared` | kernel32 | Vista | Shared acquisition is exclusive here: correct, but not concurrent |
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
| Condition variables | `InitializeConditionVariable`, `SleepConditionVariableCS`, `SleepConditionVariableSRW`, `WakeConditionVariable`, `WakeAllConditionVariable` |
| File information | `GetFileInformationByHandleEx`, `SetFileInformationByHandle`, `GetFinalPathNameByHandleW`, `ReOpenFile` |
| I/O | `CancelIoEx`, `CancelSynchronousIo`, `GetQueuedCompletionStatusEx`, `SetFileCompletionNotificationModes` |
| Fibers | `ConvertThreadToFiberEx`, `IsThreadAFiber` |
| Locale and string | `CompareStringOrdinal`, `GetLocaleInfoEx` |
| Named pipes | `GetNamedPipeClientProcessId`, `GetNamedPipeServerProcessId` |
| Critical sections | `InitializeCriticalSectionEx` |
| Misc | `GetCurrentProcessorNumber`, `GetErrorMode`, `CreateSymbolicLinkW`, `NeedCurrentDirectoryForExePathW`, `RegisterApplicationRestart` |

`InitializeCriticalSectionEx`, like the `Fls*` set above it, is pulled in by the
MSVC static CRT itself rather than by any application code, so an XP target
needs it no matter what the program does.
