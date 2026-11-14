# Incomplete functions

Most of what [FUNCTION_LIST.md](FUNCTION_LIST.md) lists is a full
implementation: the same inputs produce the same outputs and the same errors as
the function it stands in for, and a caller cannot tell the difference. These
are the ones where that is not true.

They are here because the gap matters more than the function does. A caller
that knows `CancelIoEx` only cancels its own thread's I/O can work with that; a
caller that assumes otherwise has a bug it will never find by reading its own
code. Each entry says what the function does instead and what it costs, so the
answer is in writing rather than in somebody's memory.

Nothing here is a stub that returns a plausible value. Where a function cannot
do its job it fails, with the error code its callers already test for, which
is not the same thing as failing with any error.

Three kinds, in rough order of how much they can bite:

---

## Does less than it says

The function works, but not in every case the real one covers. These are the
ones to read before relying on something.

### `CancelIoEx`: same thread only, and cannot target one request

Built on XP's `CancelIo`, which cancels every pending request the **calling
thread** issued on a handle. The real function cancels any thread's, and can
cancel a single `OVERLAPPED`. So a cross-thread cancellation silently cancels
nothing, and a request-specific cancellation takes its siblings with it.

Returns `TRUE` where `CancelIo` succeeded and `ERROR_NOT_FOUND` where there was
nothing to cancel, because that is the pair libuv's pipe code tests for.

**Known consequence, worth recognising rather than rediscovering:** libuv's
`uv__pipe_interrupt_read()` spins on `CancelSynchronousIo` waiting for a
thread-pool thread blocked in `ReadFile` on a non-overlapped pipe to
acknowledge a cancellation that never arrives. The loop ends only when the read
completes by itself. It is a busy wait with `SwitchToThread()` rather than a
true deadlock, and it is reached only for pipes that could not be opened
overlapped, but on XP it is a hang in everything but name. Queuing an APC does
not help: a thread inside a synchronous `ReadFile` is not in an alertable wait.

### `GetQueuedCompletionStatusEx`: `fAlertable` is approximated

The dequeue loop is faithful: it returns as soon as the port runs dry rather
than waiting for `ulCount` packets, and it distinguishes all four ways
`GetQueuedCompletionStatus` overloads its return value, so failed I/O and
hand-posted wake-up packets are not lost.

`fAlertable` has nothing to build on, because `GetQueuedCompletionStatus` is
never alertable. It is approximated by polling the port and then giving up the
rest of a 5 ms slice in `SleepEx`, which is alertable. An APC that runs ends the
call with `WAIT_IO_COMPLETION`, as the real one does. The cost is that a packet
arriving mid-slice waits out the remainder, so the alertable path trades a few
milliseconds of latency for being able to deliver APCs at all. Both of
Electron's callers pass `FALSE` and never reach it.

### `GetFinalPathNameByHandleW`: two of the four name formats

`VOLUME_NAME_DOS` and `VOLUME_NAME_NT` only, via `NtQueryObject` and a
device-to-drive map. `VOLUME_NAME_GUID` and `VOLUME_NAME_NONE` are not
implemented.

### `GetFileInformationByHandleEx`: `FileIdInfo` is synthesised

Most classes map onto `NtQueryInformationFile`, and the enumeration classes
onto `NtQueryDirectoryFile`. `FileIdInfo` has no XP equivalent, so it is
assembled from the volume serial number and the 64-bit file index. That is
unique and stable in the same way the real 128-bit file ID is, but it is not
the same value, so it must not be persisted and compared against one produced
by a real `GetFileInformationByHandleEx`.

### `GetLocaleInfoEx`: locale names that are language plus country

The name is resolved to an LCID by enumerating the locales the machine has and
rebuilding each one's name from the pieces XP exposes, so `en` plus `US` matches
`en-US`. Names that are not language-plus-country do not match: script-tagged
ones such as `zh-Hans` or `sr-Latn-RS`.

The alternative is a static table of every name and LCID from the NLS
reference, which is what YY-Thunks does. Asking the running system cannot go
stale, cannot disagree with it, and covers exactly the locales the machine
really has, at the price of these names. Worth revisiting if a consumer needs
them; neither Electron nor ICU's Windows formatters request them.

### `Fls*`: per thread, not per fiber

Fiber-local storage built on thread-local storage. The callback half is
faithful: `FlsAlloc`'s callback runs for every non-null value at thread exit,
driven by a PE TLS callback, and `FlsFree` runs it for every thread still
holding a value. The MSVC CRT depends on that to release per-thread state.

What differs is the scope the name promises. A program that converts threads to
fibers and expects each fiber to see its own slot gets one slot per thread
instead. Faking the real behaviour would mean intercepting the whole fiber API.

### `GetTickCount64`: needs to be called at least once per wrap

`GetTickCount` is 32-bit milliseconds and wraps about every 49.7 days;
`GetTickCount64` does not, and callers depend on that, since a comparison
across a wrap jumps backwards by 49 days: in Chromium and libuv, timers that
never fire or fire immediately.

The high half is kept here and extended whenever the low half is observed to go
backwards, which is correct as long as the function is called at least once per
wrap period. Any running browser calls it many times a second. A process that
slept for more than 49.7 days between two calls would miss a wrap. Every
implementation of this shim has the same caveat.

### `GetCurrentProcessorNumber`: a hint

Derived from the CPUID initial APIC ID. It can be stale the moment it is
returned, because the thread may be rescheduled onto another processor, but
that is equally true of the real function, which is why callers use it to index
a per-CPU cache rather than to make a decision.

### `SRWLOCK` and `CONDITION_VARIABLE`: a parking lot, not kernel objects

A real reader/writer lock: readers are genuinely concurrent, writers are
preferred so they cannot starve, and the whole state is the one pointer-sized
word the API gives it, with waiters queued by address in a parking lot of
per-thread auto-reset events. Not a `CRITICAL_SECTION` wearing the name.

What it is not is the kernel's implementation, so contended performance differs
and `WaitOnAddress`-style kernel keyed events are not involved.

One consequence worth knowing, because it is not about XP: these twelve
functions resolve to the system's versions **as a group or not at all**, and
the group is only complete from Windows 7. Vista exports
`AcquireSRWLock*`/`ReleaseSRWLock*` but not `TryAcquireSRWLock*`, and its ntdll
has no `RtlTryAcquireSRWLock*` underneath either, so on Vista the whole family
stays winwrap's. Mixing them is not a slower lock, it is memory corruption,
since each implementation reads that one word differently.

### `CreateFiberEx` and `ConvertThreadToFiberEx`: `FIBER_FLAG_FLOAT_SWITCH` is dropped

XP has `CreateFiberEx` but rejects `FIBER_FLAG_FLOAT_SWITCH` with
`ERROR_INVALID_PARAMETER`. The call is retried without the flag rather than
failing, so the fiber is created and its floating-point state is not switched
along with it. `ConvertThreadToFiberEx` does the same, falling back to
`ConvertThreadToFiber`.

This is the one that kept Electron from starting on XP at all: the 32-bit
`electron_main_win.cc` fiber dance asks for the flag, and a failure there fell
through to a stack overflow.

---

## Succeeds without doing anything

The function reports success because the state it would have established
already holds. A caller cannot tell, and there is nothing for it to be wrong
about.

| Function | What it does instead |
|---|---|
| `SetProcessDPIAware` | Returns `TRUE`. XP has no DPI virtualisation to opt out of, so the process is already in the state the call asks for. |
| `SetFileCompletionNotificationModes` with `FILE_SKIP_SET_EVENT_ON_HANDLE` alone | Returns `TRUE`. The flag only suppresses signalling the handle's own event; a caller that gets the signalling it asked to skip is not harmed by it. c-ares requests this flag by itself and treats failure as fatal to its whole event backend. |
| `SetCurrentProcessExplicitAppUserModelID` | Accepts and discards the ID, but rejects a null one with `E_INVALIDARG`, as the real function does. Vista has no AppUserModelID concept to store it in. |
| `GetCurrentProcessExplicitAppUserModelID` | Reports that no ID is set, and clears the out parameter. Callers free it unconditionally, so leaving it untouched would be a crash. |

---

## Cannot do it, and says so

No emulation is possible. Each returns the error its callers already test for.

| Function | Result | Why |
|---|---|---|
| `CreateSymbolicLinkW` | `ERROR_NOT_SUPPORTED` | XP has no symbolic links to create. |
| `CancelSynchronousIo` | `ERROR_NOT_FOUND` | Cross-thread by definition, and XP offers no way to reach another thread's synchronous I/O. See the `CancelIoEx` note above for what this costs libuv. |
| `RegisterApplicationRestart` | `E_NOTIMPL` | XP has no Restart Manager. |
| `UnregisterApplicationRestart` | `E_NOTIMPL` | To agree with its other half. |
| `GetNamedPipeServerProcessId` | Fails | XP's npfs records neither party to a connection, and no inference from the handle is safe. |
| `SetFileCompletionNotificationModes` with `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` | `ERROR_INVALID_FUNCTION` | The opposite of the harmless case above. Claiming success tells the caller a synchronously completed operation will *not* queue a packet, so it stops waiting for one and may free the `OVERLAPPED`, and then XP queues the packet anyway, into a structure that no longer exists. Reporting failure costs an optimisation; reporting success costs a wild pointer. libuv's UDP path treats exactly this code as "not supported here" and carries on; its TCP path does not, and needs a patch of its own. YY-Thunks arrived at the same split after a bug report (Chuyu-Team/YY-Thunks#130). |

`GetNamedPipeClientProcessId` is a near miss rather than a member of this list:
it asks npfs through the same control code the real function uses, and so works
where the kernel has an answer and fails where it does not.
