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

Returns `TRUE` wherever `CancelIo` succeeded, which **includes the case where
there was nothing to cancel** — `CancelIo` reports success for both and gives
no way to tell them apart. The real `CancelIoEx` answers `FALSE` with
`ERROR_NOT_FOUND` when it finds no matching request. Callers that only need to
know nothing is left pending cannot act on the difference, and libuv's pipe
code accepts either (`r || GetLastError() == ERROR_NOT_FOUND`), which is why
this is not worth a probe to tighten. A `CancelIo` that genuinely fails passes
its own error through rather than being flattened into `ERROR_NOT_FOUND`.

`CancelSynchronousIo` is the one that always answers `ERROR_NOT_FOUND`, since
it can never do anything at all.

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

**Not winwrap's, but it lands on the same callers, and it is worse than a
different spelling: on Vista an APC-ended alertable wait is indistinguishable
from a completion.**

Windows 7 and later return `FALSE` with `WAIT_IO_COMPLETION`, which is what the
documentation says and what the emulation above does — so XP agrees with 11.
Vista's own implementation, measured on a port confirmed empty immediately
beforehand, returns **`TRUE`** and sets no error; the error left afterwards was
the previous call's `WAIT_TIMEOUT`.

Both of them also leave `1` in `lpNumberOfEntriesRemoved`, which is worth
stating only to dismiss it: an output parameter of a call that failed means
nothing, so Windows 10 writing to it there is unremarkable. On Vista the call
*succeeded*, and on the success path that count is authoritative — which is
what makes this dangerous rather than merely different. A caller is told there
is one completion to process when there was none.

What that entry contains, measured on Vista with the buffer zeroed first:

```
entries removed: 1
first entry:     key 0, 0 bytes, overlapped null
APC ran:         yes
```

So the APC really did run — the wait ended for the right reason and only the
reporting is wrong — and the phantom entry is entirely zero rather than
fabricated or stale. That narrows the hazard considerably, because a zeroed
entry with a null `lpOverlapped` is indistinguishable from a hand-posted
`PostQueuedCompletionStatus` wake-up. A caller that dispatches on
`lpOverlapped` therefore treats it as "something woke me, go round again",
which is accidentally the correct response. A caller that dereferences
`lpOverlapped` without checking, or that attaches meaning to completion key 0,
is the one that breaks. The unit test asserts the null pointer specifically,
since a non-null one would be categorically worse.

winwrap forwards to Vista's implementation rather than substituting, so a
winwrap-linked binary sees this there.

The defence, if a caller cannot be sure it handles a null `lpOverlapped`, is
not to pass `fAlertable = TRUE` on Vista at all — use a separate alertable
wait if APCs have to be delivered. Both of Electron's callers pass `FALSE`, so
nothing in that tree is exposed.

The unit test reports this rather than failing on it, so a Vista run stays
legible: it is not winwrap's defect, and a permanently red Vista run teaches
people to stop reading the output. `TestCompletionPort` in
`test/winwrap_unittest.cc` is where the shape above was measured.

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

### `LCMapStringEx`: no sort handle, and no sort version

The locale name is resolved the same way `GetLocaleInfoEx` does, so it inherits
that entry's limit exactly -- script-tagged names do not match -- and the
mapping itself is then XP's `LCMapStringW`, which is the same function
underneath.

What it will not do is the three parameters the `Ex` form added.
`lpVersionInformation` selects a specific sort version and `sortHandle` names a
sort handle, neither of which exists before Vista, and `LCMAP_SORTHANDLE` makes
`lpLocaleName` a sort handle too. Passing any of them fails with
`ERROR_INVALID_PARAMETER` rather than quietly doing the unversioned mapping,
which would answer a question the caller did not ask. `lpReserved` is specified
as always null and is checked for the same reason.

### `CompareStringEx`: no sort version, and three flags XP rejects

Resolved the same way `GetLocaleInfoEx` is, so it inherits that entry's limit
on script-tagged locale names, and the comparison itself is XP's
`CompareStringW`.

`lpVersionInformation`, `lpReserved` and `lParam` are refused with
`ERROR_INVALID_PARAMETER`, as in `LCMapStringEx`. `dwCmpFlags` is passed
through unfiltered, which means `LINGUISTIC_IGNORECASE`,
`LINGUISTIC_IGNOREDIACRITIC` and `SORT_DIGITSASNUMBERS` fail with
`ERROR_INVALID_FLAGS` from `CompareStringW` rather than being quietly dropped.
That is deliberate: comparisons feed sorting and equality, so a silently
different answer propagates further than a visible failure.

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

### `CreateWaitableTimerExW`: no high-resolution timers

`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` fails with `ERROR_INVALID_PARAMETER`.
That is not a gap particular to winwrap: the flag arrived in Windows 10 1803,
and Vista through 1709 answer the same way, so callers that use it already
handle the refusal.

Nothing else about this function or its siblings `CreateEventExW` and
`CreateSemaphoreExW` is reduced. The access mask is honoured — the handle from
the XP function is duplicated down to it — with one exception that grants no
more than the real call would: `MAXIMUM_ALLOWED` returns the full-access handle
as it stands.

### `QueryThreadCycleTime`: clock ticks, expressed as cycles

Vista charges a thread the time stamp counter's reading at every context
switch. XP keeps no such count; the only per-thread accounting it has is the
one behind `GetThreadTimes`, which charges a whole clock tick to whichever
thread is running when the timer interrupt arrives. The shim returns that —
kernel and user time together — multiplied up to cycles at the processor's
nominal rate, the `~MHz` value the HAL leaves in the registry.

So the unit is right and a caller dividing by the counter's frequency gets
seconds, which is what V8 does with it. The resolution is not: the value moves
in steps of 10 or 15.6 ms, and a thread that habitually yields before the tick
can run a great deal while appearing to use nothing. Anything timing short
sections of code with this will read zero.

### `GetDynamicTimeZoneInformation`: the key name is searched for

XP does not record which registry key the current zone came from, so
`TimeZoneKeyName` is found by matching: first the key whose `Std` string is the
standard name in force, then, failing that, the first key whose rule — biases
and transition dates — is the one in force. The second can name a different
zone that happens to keep the same time, and if neither matches the name is
empty. That last case is legitimate on Windows too, for a zone set by hand, and
ICU handles it.

`DynamicDaylightTimeDisabled` is XP's `DisableAutoDaylightTimeSet`, the
"automatically adjust for daylight saving" box. XP has no year-by-year rules,
so there is nothing else for the field to describe.

### `SHGetKnownFolderPath`: only the folders XP has

Works for the folder IDs with a `CSIDL` equivalent, which is every folder XP
knows about. IDs for folders Vista introduced — Downloads, SavedGames, Links,
LocalAppDataLow, Public, the libraries — fail with
`HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)`, as the real function does for an ID
the system lacks. None is redirected to a near equivalent. `KF_FLAG_ALIAS_ONLY`
fails with `E_INVALIDARG`, and `hToken` is passed through to
`SHGetFolderPathW`, which honours it for the per-user folders.

This is one of the two places winwrap **loads libraries** (the network
interface pair below is the other): shell32 for the folder lookup
and ole32 for `CoTaskMemAlloc`, by full path from the system directory, kept
for the life of the process. Consumers delay-load shell32, and because the
redirect means no delay thunk exists for this function, nothing else would
load it. It should not be called from `DllMain`, which is equally true of the
function it stands in for.

### `ConvertInterfaceIndexToLuid`, `ConvertInterfaceLuidToNameW`: a made-up LUID

XP identifies a network interface by its index and has no `NET_LUID` to look
up, so `ConvertInterfaceIndexToLuid` builds one: the interface type from
`GetIfEntry`, and the index itself where the LUID's own number would go. It is
well formed, different for every interface, and as stable as the index.

`ConvertInterfaceLuidToNameW` then formats it as Windows would — `ethernet_2`,
`loopback_1`, `iftype200_7` — but the number is the index, so the name is not
what a later Windows would call the same adapter (`ethernet_32769`, say).
Windows assigns that number per machine, so nothing portable could depend on
it; within one XP session the names are consistent, which is what
`uv_if_indextoname()` needs.

A LUID from here means something only to the other function in the pair. None
of the rest of the Vista interface API is supplied — `ConvertInterfaceLuidToIndex`,
`ConvertInterfaceNameToLuidW`, `GetIfEntry2` and so on — so there is nothing
else to hand one to.

Like `SHGetKnownFolderPath`, the pair loads its DLL, iphlpapi, from the system
directory on first use, because consumers delay-load it.

### `SymGetSearchPathW`, `SymSetSearchPathW`: through the ANSI code page

Where the loaded dbghelp lacks the wide functions — XP's 5.1 does — these call
the ANSI ones and convert. A directory name with a character outside the
machine's ANSI code page does not survive the trip: that entry of the path
will not match anything, and the rest are unaffected.

`SymGetSearchPathW` fails with `ERROR_INSUFFICIENT_BUFFER` and an empty string
when the path does not fit, rather than returning part of an entry.

They call into whichever `dbghelp.dll` is **already in the process**, asking
for it by bare name, which loads one only if there is none. winwrap keeps a
reference to it from then on, so that copy stays loaded for the life of the
process even if the program frees its own. A symbol session lives in the module
`SymInitialize` was called in, so a program that ships its own dbghelp beside
the executable has to be answered by that copy; System32's would know nothing
of the session. A newer redistributable dbghelp has the wide functions itself,
and then winwrap only forwards.

Nothing else in dbghelp is supplied. A caller of `SymFromAddrW`,
`SymGetLineFromAddrW64` and the other wide functions XP lacks still needs a
newer dbghelp.dll shipped beside it.

### `GetThreadId`: needs `THREAD_QUERY_INFORMATION`

The result is exact; it comes from the same kernel query the real function
makes. What differs is the access the handle must carry. From Vista on,
`THREAD_QUERY_LIMITED_INFORMATION` is enough. XP has no such right, so there
the handle needs `THREAD_QUERY_INFORMATION`, and one opened with only the
limited right — which XP's `OpenThread` would refuse to grant anyway — fails
with `ERROR_ACCESS_DENIED`.

### `GetSystemTimePreciseAsFileTime`: precise only where the system is

Windows 8 added an interpolated clock, and this function reads it. Before
that there is none, so on XP, Vista and 7 the shim returns
`GetSystemTimeAsFileTime`: the correct time, in steps of up to 15.6 ms. Two
calls in quick succession return the same value far more often than they
would on Windows 8.

It is deliberately not improved with `QueryPerformanceCounter`. That would
give more digits from a second clock that runs at a slightly different rate,
and the result would drift against file times and against
`GetSystemTimeAsFileTime`, both of which callers compare it with.

### `QueryFullProcessImageNameW`: needs more access, and a drive letter

The name comes from the kernel, through psapi's `GetProcessImageFileNameW`,
as the real function's does. Two things differ.

The handle needs `PROCESS_QUERY_INFORMATION`. The real function is satisfied
with `PROCESS_QUERY_LIMITED_INFORMATION`, a right XP does not have, so a
process that could be named on Vista with a narrowly opened handle may answer
`ERROR_ACCESS_DENIED` here.

Without `PROCESS_NAME_NATIVE` the kernel's device path is translated to a
drive letter by looking for the letter mapped to that device. An executable
on a volume with no letter — run from a network share, or from a volume
mounted only as a folder — has none to find. The shim then falls back to
`GetModuleFileNameExW`, which reads the path from the process's own memory
and so needs `PROCESS_VM_READ` as well; if that fails too the call fails with
psapi's own error, `ERROR_ACCESS_DENIED` for a handle without that right.
`PROCESS_NAME_NATIVE` is unaffected.

psapi.dll is loaded from the system directory on first use.

### `GetCurrentProcessorNumber`: a hint

Derived from the CPUID initial APIC ID. It can be stale the moment it is
returned, because the thread may be rescheduled onto another processor, but
that is equally true of the real function, which is why callers use it to index
a per-CPU cache rather than to make a decision.

### `ResolveLocaleName`: normalises, but does not fall back

Built from `LocaleNameToLCID` and `LCIDToLocaleName`, so a name goes to an LCID
and comes back as whatever the installed locale calls itself. That normalises
what can be normalised -- case, and a name whose pieces are in the right order:
`EN-us` resolves to `en-US`.

The half that is missing is the one the name promises. The real function walks
up a tag until something matches, so `en-XY` on a machine with no such locale
resolves to `en`, and a script-tagged or three-letter tag resolves to its
nearest relative. This resolves a name the system has and fails on one it does
not.

Returning a wrong neighbour would be worse than failing. A caller handed the
wrong locale formats dates and numbers in it with no sign that anything went
amiss, where a zero is visible at the call site. ICU, the only caller in the
tree, reads a zero as `U_UNSUPPORTED_ERROR` and carries on with its own data --
which is a better outcome than a plausible-looking wrong locale, and one a
partial answer would have denied it.

The same applies to `LocaleNameToLCID` and `LCIDToLocaleName`, which resolve
only the names `NameForLcid` can rebuild out of `LOCALE_SISO639LANGNAME` and
`LOCALE_SISO3166CTRYNAME` -- language, or language plus country. Two kinds of
name are not that. One carries a script, such as `zh-Hans` or `sr-Latn-RS`. The
other is an alternate sort order, which Windows spells with a suffix:
`es-ES_tradnl`, `de-DE_phoneb`, `ja-JP_radstr`. Neither can be produced from a
language and a country, so both fail to resolve. See the note in `locale.cc`
for why asking the running system beats carrying a table of every name, and
what it costs.

The sort names cost more than a failure, and this is the one place in this
family where a wrong answer is possible rather than a refusal. An
alternate-sort locale shares its language and country with its default
sibling, so the two rebuild to the same name here: **0x040A is `es-ES_tradnl`
and 0x0C0A is `es-ES`**, and `LOCALE_SISO639LANGNAME` plus
`LOCALE_SISO3166CTRYNAME` answers `es` and `ES` for both. A lookup of `es-ES`
lands on whichever the enumeration reaches first, and Windows always means
0x0C0A. Picking the other one is not a near miss: traditional Spanish
collation treats *ch* and *ll* as single letters, so `CompareStringEx` would
order Spanish text differently from every other Windows program on the
machine. A static table is the only fix, and it is the strongest argument for
carrying one -- but no caller in the tree asks for a sort name, and the
collision needs a machine with both siblings installed to appear at all.

`LOCALE_ALLOW_NEUTRAL_NAMES` is accepted rather than rejected as an unknown
flag, and then ignored, which is not a gap: the flag's effect on a real Windows
is to permit a name with no region instead of failing, and that is what this
does with or without it, because a locale with no country simply produces no
country. ICU passes the flag on every call, so refusing it would have failed
the only caller there is.

That is winwrap's fallback, which runs on XP. Vista has its own
`LCIDToLocaleName` and `LocaleNameToLCID`, which winwrap forwards to, and they
predate the flag -- it arrived in Windows 7 -- and reject any call carrying it
with `ERROR_INVALID_PARAMETER`. winwrap retries such a call without the flag,
so it succeeds on Vista as it does everywhere else. For a specific locale the
answer is the same one. For a neutral locale it is whatever Vista returns when
not asked to allow neutral names, which may be the default specific locale
(`en-US` for `en`) rather than the neutral name the flag was asking for.

### Thread pool timers and waits: winwrap's objects, not the OS's

The firing works, and the unit test checks it does: one-shot, periodic,
cancellation, the wait result for a signal against a timeout, and that a
cancelled timer or wait delivers nothing afterwards. What is partial is
everything around the callback.

**The handles are winwrap's allocations.** A `PTP_TIMER` or `PTP_WAIT` from
here points at a winwrap structure, not at anything the kernel knows about, so
it must be closed by the copy of winwrap that made it. Two modules that each
link winwrap statically have two sets of these functions and two heaps, and
passing an object between them is a wild pointer. One shared `winwrap.dll`
serving the whole process does not have this problem, and is the better
arrangement wherever there is a choice. YY-Thunks carries the same warning for
the same reason.

**The `PTP_CALLBACK_INSTANCE` is not a real one.** It is the object itself, so
that `FreeLibraryWhenCallbackReturns` can find which callback it belongs to
without a thread-local. The instance is therefore only meaningful to winwrap's
own shims. Nothing else before Vista takes one, so there is nowhere wrong for
it to go today -- but the rest of that family is not implemented, and a
consumer that starts calling `CallbackMayRunLong`,
`SetEventWhenCallbackReturns`, `LeaveCriticalSectionWhenCallbackReturns` or
`DisassociateCurrentThreadFromCallback` needs those written before it can pass
this instance to them. Each object carries a signature so a shim can reject a
pointer that is not one of ours rather than write through it.

**`PTP_CALLBACK_ENVIRON` is ignored.** The environment selects a private pool,
a cleanup group, a callback priority and a few other things the legacy timer
queues have no equivalent for; the default process pool is all there is.
Refusing a non-null environment is what this library does elsewhere for a
parameter it cannot honour -- see `LCMapStringEx`'s sort handle -- and it is
the wrong choice here. An environment changes where a callback runs, not what
it computes, and the caller is the Concurrency Runtime, which would meet a
null return by failing outright rather than falling back. The visible
consequence is that a cleanup group will not track these objects, so a
consumer relying on `CloseThreadpoolCleanupGroupMembers` to tear them down has
to close them itself.

**`msWindowLength` is ignored.** It is a coalescing tolerance, letting Windows
slide a firing to join other timers and save power. There is nothing to ask
for it with before Vista. Firing on time is always within tolerance, so this
is a hint lost rather than behaviour changed.

**Two places this is deliberately *not* the same as Windows**, both of them
narrowing a documented hazard rather than widening it:

- `CloseThreadpoolTimer` and `CloseThreadpoolWait` wait for a callback already
  running on another thread. The real functions document a race the caller is
  expected to have closed by calling `WaitForThreadpoolTimerCallbacks` first.
  Waiting costs a correct caller nothing and turns an incorrect one's
  use-after-free into a short pause.
- `WaitForThreadpoolTimerCallbacks` and `WaitForThreadpoolWaitCallbacks` called
  from inside their own callback return instead of deadlocking, as does closing
  from inside one. A caller asking "are the callbacks done" from inside one has
  its answer already: all but this one.

Cancelling *from* inside the callback is the one case that cannot be
synchronous, because a blocking teardown there would be waiting for the
calling thread to finish, which never comes. So one callback already queued
behind the caller may still run, as it may on Windows.

What matters is that the object outlives it, and a non-blocking
`DeleteTimerQueueTimer` cannot promise that: it gives up the handle, and the
handle is the only thing a later close can block on to learn the dispatcher has
let go. A queued callback that has not started is invisible to everything else.
So a live timer cancelled from its own callback is not deleted but *parked* --
pushed as far out as the legacy API allows, with the handle kept for whoever
tears the object down from a thread that may wait. A one-shot that has already
fired has nothing behind it and is simply deleted.

A close from inside a callback has the same problem and no later caller to
leave it to, so it queues a work item, which blocks until the dispatcher has
let go of both the object and the callback that asked for the close, and
destroys it there. The one path that cannot do even that is a failure to queue
the work item, which needs the process to be out of memory; it leaks the object
rather than risk handing a queued callback a dangling pointer.

The wait object has no equivalent of parking. Re-arming a wait and then
cancelling it within the same callback is the remaining case where a
registration is given up without a blocking unregister.

The same bookkeeping is what makes re-arming a fired one-shot work at all:
`ChangeTimerQueueTimer` leaves an expired one-shot untouched, so the shim
remembers that the arming was used up and replaces the timer instead.

A periodic timer is also why the per-callback bookkeeping is a list rather than
a field. Two callbacks of the same object can run at once, so "the callback
thread" and "the library to free when the callback returns" are per invocation,
not per object. Getting that wrong is not a visible error: it is
`WaitForThreadpoolTimerCallbacks` returning while a callback is still running,
a close from the second callback mistaking itself for an outside caller and
blocking on its own completion, or one of two `FreeLibraryWhenCallbackReturns`
requests being quietly dropped.

### `GetLogicalProcessorInformation`: the count, not the topology

The one entry here whose floor is a service pack rather than a Windows
version: XP SP3 and Server 2003 SP1 export this, so the fallback runs only on
SP2 and earlier, and only on 32-bit — the oldest x64 Windows is Server 2003
SP1 underneath and has it natively.

What is exact is the processor *count*. It comes from `GetSystemInfo`'s active
processor mask, which is the same number every other API reports, and the
`RelationProcessorCore` entries cover exactly that mask.

What is approximated is everything structural:

- every active processor is reported as its own core, with
  `ProcessorCore.Flags` zero. Telling a core from a hyperthread needs CPUID,
  so the choice is between understating how many logical processors share a
  core and overstating how many cores exist; understating is the safer error
- no `RelationCache` entries at all. Cache sizes and sharing cannot be
  discovered without CPUID, and inventing them would be worse than omitting
  them, because a caller cannot tell a guess from a measurement. One that
  finds no cache entries falls back to its own defaults, which is what a
  failed call would have made it do anyway
- one `RelationNumaNode` and one `RelationProcessorPackage`, both covering the
  whole mask. Anything old enough to lack this function is a single socket
  with no NUMA, so one entry each describes the machine

The alternative is what YY-Thunks does: read CPUID leaves 0, 1, 0x0B and 0x1F,
recover the APIC IDs and merge logical processors into real cores. That is
several hundred lines of x86-only inline assembly, and worth having if
something ever needs the topology rather than the count.

Worth saying why a shim exists for a function most callers on these systems
never reach: the MSVC CRT references it while guarding the call behind its own
version check, so the usual case is a program that imports it and never calls
it. The import alone stops the image loading on SP2.

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
| `EventRegister` | `ERROR_NOT_SUPPORTED` | A manifest provider registers with a session infrastructure XP does not have. The handle is set to zero, so a caller that ignores the result -- TraceLogging's registration macro permits that -- goes on to write and unregister with a value every sibling refuses. |
| `EventUnregister` | `ERROR_INVALID_HANDLE` | `EventRegister` is the only source of handles and on such a Windows it has never issued one. |
| `EventSetInformation` | `ERROR_NOT_SUPPORTED` | The same missing provider model, two Windows later: this arrived in Windows 8, so Vista and Windows 7 both have `EventRegister` and `EventWriteTransfer` and lack only this. Not `ERROR_INVALID_HANDLE`, because that reasoning does not carry here -- a Vista or 7 caller may be holding a perfectly good `REGHANDLE`, and calling it invalid would be a lie about the one thing it could check. What is true on all three is that the system cannot set provider information, and the real function already answers `ERROR_NOT_SUPPORTED` for an `InformationClass` it does not recognise. |
| `EventWriteTransfer` | `ERROR_INVALID_HANDLE` | XP's ETW is the classic provider model only -- `RegisterTraceGuids` and `TraceEvent`. A manifest provider is described by a manifest the system must already know about, and there is nothing on XP for its events to be written into. `ERROR_INVALID_HANDLE` rather than `ERROR_NOT_SUPPORTED` because the `REGHANDLE` really is invalid: `EventRegister` always fails there, so nothing can ever have produced a valid one, and that is the condition the real function reports this way. |
| `RegisterApplicationRestart` | `E_NOTIMPL` | XP has no Restart Manager. |
| `UnregisterApplicationRestart` | `E_NOTIMPL` | To agree with its other half. |
| `GetNamedPipeServerProcessId` | Fails | XP's npfs records neither party to a connection, and no inference from the handle is safe. |
| `SetFileCompletionNotificationModes` with `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` | `ERROR_INVALID_FUNCTION` | The opposite of the harmless case above. Claiming success tells the caller a synchronously completed operation will *not* queue a packet, so it stops waiting for one and may free the `OVERLAPPED`, and then XP queues the packet anyway, into a structure that no longer exists. Reporting failure costs an optimisation; reporting success costs a wild pointer. libuv's UDP path treats exactly this code as "not supported here" and carries on; its TCP path does not, and needs a patch of its own. YY-Thunks arrived at the same split after a bug report (Chuyu-Team/YY-Thunks#130). |

`GetNamedPipeClientProcessId` is a near miss rather than a member of this list:
it asks npfs through the same control code the real function uses, and so works
where the kernel has an answer and fails where it does not.
