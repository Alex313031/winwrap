// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The Vista thread pool's timer and wait objects, on XP's timer queues.
//
// Unlike most of this library these are not thin wrappers: a TP_TIMER and a
// TP_WAIT are objects with lifetimes, and XP has nothing to hand them to. So
// winwrap allocates its own and drives them with the older API that is the
// Vista pool's ancestor -- CreateTimerQueueTimer for a timer,
// RegisterWaitForSingleObject for a wait. Both of those already run callbacks
// on a system-managed pool of threads, which is the part that would otherwise
// have to be written from scratch.
//
// Where these come from is worth knowing, because nothing in a consumer's
// source asks for them. They are the static Concurrency Runtime's:
// libconcrt.lib references eight of the nine here -- WaitForThreadpoolWaitCallbacks
// is the timer drain's counterpart, added so the pair is complete rather than
// because anything imported it -- and libcpmt.lib pulls libconcrt in
// because MSVC implements std::mutex, std::condition_variable and std::thread
// on ConcRT primitives. So any C++ program using the static CRT and a standard
// mutex imports the Vista thread pool whether it meant to or not -- the same
// shape of problem as <string> dragging in CompareStringEx, noted in
// framework.h.
//
// === The handles do not cross module boundaries ===
//
// A PTP_TIMER from here is a pointer to winwrap's own structure, not to
// anything the OS knows about. Two consequences, and the second is the one
// that bites:
//
//   - a handle made by this copy of winwrap must be closed by the same copy.
//     Two modules that each link winwrap statically have two sets of these
//     functions and two heaps, and passing an object between them is a wild
//     pointer. A process where one shared winwrap.dll serves everybody does
//     not have this problem, which is the better arrangement where there is a
//     choice.
//   - the PTP_CALLBACK_INSTANCE handed to a callback is also winwrap's, and is
//     not a real one. It is the object, which is enough for
//     FreeLibraryWhenCallbackReturns to find the object and then, within it,
//     the frame belonging to the calling thread. Passing it to any *system*
//     function that takes an instance would be read as a real pointer -- but
//     there are no such functions before Vista, so on the systems this code
//     runs on the only possible recipient is winwrap's own shim. Each object
//     still carries a signature so that shim can reject anything else rather
//     than write through a pointer it has misread.
//
//     Worth noting that one instance per object is itself a difference: the
//     real pool makes one per callback invocation. Nothing winwrap implements
//     can tell the difference, because what the instance is used for is kept
//     per invocation anyway -- see CallbackFrame.
//
// YY-Thunks reaches the same place by the same route and carries the same
// warning against using its handles across modules.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // Enough to recognise one of ours, and to tell the two kinds apart. Not a
  // security measure -- a caller that passes a random pointer can still be
  // unlucky -- but it turns the overwhelmingly likely mistake, a real
  // TP_CALLBACK_INSTANCE from some other source, into a no-op.
  constexpr DWORD kTimerSignature = 0x57577054; // "WWpT"
  constexpr DWORD kWaitSignature  = 0x57577057; // "WWpW"

  constexpr DWORD kSpinCount = 2000;

  // One of these per callback *invocation*, living on the callback thread's own
  // stack and linked into the object for as long as that callback runs.
  //
  // A fixed field in the object cannot do this job, and that is not a
  // hypothetical: a periodic timer armed with a period shorter than its
  // callback takes gets a second callback dispatched while the first is still
  // running, which the real TP_TIMER also permits. Then there are two current
  // callback threads and two independent FreeLibrary requests, and a single
  // slot for either is wrong in a way that shows up as a hang or an unfreed
  // module rather than as a visible error.
  //
  // A list costs no allocation and is exact for any number of them, because a
  // frame is only ever reachable while the stack frame holding it is alive.
  struct CallbackFrame {
    DWORD thread;
    // Set by FreeLibraryWhenCallbackReturns, acted on once this callback has
    // returned. Per frame, because the real function's unit is one library per
    // callback, not per object.
    HMODULE free_on_return;
    CallbackFrame* next;
  };

  // The part both object kinds share, first in each of them so a Common* cast
  // is valid for either. FreeLibraryWhenCallbackReturns and the reaper are the
  // things that need to work without knowing which they have.
  struct Common {
    DWORD signature;
    PVOID context;
    // Callbacks running right now, newest first. Empty means idle, and that is
    // the only thing that may signal the event below.
    CallbackFrame* frames;
    // Manual-reset, signalled whenever no callback is running. This is what
    // WaitForThreadpoolTimerCallbacks waits on; the legacy API has no "wait
    // for callbacks" of its own short of destroying the timer.
    HANDLE idle;
    CRITICAL_SECTION lock;
  };

  struct TimerObject {
    Common common;
    PTP_TIMER_CALLBACK callback;
    // The CreateTimerQueueTimer handle, or null while disarmed. The default
    // queue is used, so the queue handle is always null.
    HANDLE queue_timer;
    // What the handle above was last armed with, and whether a one-shot arming
    // has since fired. Together they answer the one question the legacy API
    // will not: whether ChangeTimerQueueTimer can still move this timer. It
    // cannot move a one-shot that has expired -- the call is documented to
    // leave such a timer alone -- so a spent one is replaced, not changed.
    DWORD period;
    bool spent;
  };

  struct WaitObject {
    Common common;
    PTP_WAIT_CALLBACK callback;
    HANDLE wait;   // RegisterWaitForSingleObject handle, or null.
    HANDLE object; // What is being waited on, kept only for re-arming.
  };

  bool InitCommon(Common* common, DWORD signature, PVOID context) {
    common->signature = signature;
    common->context   = context;
    common->frames    = nullptr;
    // Signalled: nothing is running yet.
    common->idle = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (common->idle == nullptr) {
      return false;
    }
    if (!::InitializeCriticalSectionAndSpinCount(&common->lock, kSpinCount)) {
      ::CloseHandle(common->idle);
      common->idle = nullptr;
      return false;
    }
    return true;
  }

  // Only ever called once the idle event has been waited on.
  void DestroyCommon(Common* common) {
    // Through the lock once before deleting it. The idle event is signalled by
    // the last callback while it still holds this lock, so a waiter can wake
    // with that callback a few instructions short of leaving it; passing
    // through here is what lets it finish. After this nothing else can be
    // inside: no callback is running, and none can start.
    ::EnterCriticalSection(&common->lock);
    // Signature first, so a use-after-free through a stale handle is rejected
    // by the checks above rather than following freed pointers.
    common->signature = 0;
    ::LeaveCriticalSection(&common->lock);
    ::DeleteCriticalSection(&common->lock);
    if (common->idle != nullptr) {
      ::CloseHandle(common->idle);
    }
  }

  // Runs the caller's callback with the bookkeeping either side of it. Shared
  // because the ordering is the delicate part and is identical for both kinds:
  // link this invocation's frame, run, unlink it, and apply whatever
  // FreeLibrary the callback asked for.
  //
  // Nothing here frees the object. A close that arrives from inside a callback
  // hands the teardown to the reaper below instead, which is the only context
  // that can wait for the dispatcher to let go -- see ww_CloseThreadpoolTimer.
  template <typename Object, typename Invoke>
  void RunCallback(Object* object, Invoke invoke) {
    Common* common = &object->common;

    CallbackFrame frame = {::GetCurrentThreadId(), nullptr, nullptr};

    ::EnterCriticalSection(&common->lock);
    frame.next     = common->frames;
    common->frames = &frame;
    ::ResetEvent(common->idle);
    ::LeaveCriticalSection(&common->lock);

    invoke();

    ::EnterCriticalSection(&common->lock);
    // Unlinked by address rather than popped: callbacks do not finish in the
    // order they started, so this frame need not be at the head.
    for (CallbackFrame** link = &common->frames; *link != nullptr; link = &(*link)->next) {
      if (*link == &frame) {
        *link = frame.next;
        break;
      }
    }
    // Only the last one out may say the object is idle, and it says so before
    // letting go of the lock. Signalling after the unlock looks safer and is
    // not: another callback can link its frame and reset the event in that
    // gap, and the late SetEvent then declares the object idle with a callback
    // inside it. Under the lock, the event and the list cannot disagree.
    //
    // The cost is that a waiter can wake while this thread still owns the
    // lock, which matters to exactly one kind of waiter -- one about to delete
    // it. DestroyCommon passes through the lock first for that reason.
    if (common->frames == nullptr) {
      ::SetEvent(common->idle);
    }
    const HMODULE to_free = frame.free_on_return;
    ::LeaveCriticalSection(&common->lock);

    // Nothing past this point may touch the object: it has been declared idle
    // and may already be gone. FreeLibrary can run arbitrary DllMain code, so
    // it goes outside the lock, and to_free is a local copy.
    if (to_free != nullptr) {
      ::FreeLibrary(to_free);
    }
  }

  void CALLBACK TimerTrampoline(PVOID parameter, BOOLEAN /*timer_or_wait_fired*/) {
    auto* timer = static_cast<TimerObject*>(parameter);
    // A one-shot arming is used up the moment it fires. Recorded before the
    // callback runs, since re-arming from inside the callback is the common
    // case and is exactly the caller that needs to know.
    ::EnterCriticalSection(&timer->common.lock);
    if (timer->period == 0) {
      timer->spent = true;
    }
    ::LeaveCriticalSection(&timer->common.lock);
    RunCallback(timer, [timer] {
      timer->callback(reinterpret_cast<PTP_CALLBACK_INSTANCE>(&timer->common),
                      timer->common.context, reinterpret_cast<PTP_TIMER>(timer));
    });
  }

  void CALLBACK WaitTrampoline(PVOID parameter, BOOLEAN timer_or_wait_fired) {
    auto* wait = static_cast<WaitObject*>(parameter);
    // The legacy callback says only "timed out or not"; the Vista one is handed
    // a wait result. Those are the only two outcomes a single-object wait can
    // have, so the translation is exact.
    const TP_WAIT_RESULT result = timer_or_wait_fired ? WAIT_TIMEOUT : WAIT_OBJECT_0;
    RunCallback(wait, [wait, result] {
      wait->callback(reinterpret_cast<PTP_CALLBACK_INSTANCE>(&wait->common),
                     wait->common.context, reinterpret_cast<PTP_WAIT>(wait), result);
    });
  }

  // A FILETIME due time as milliseconds from now.
  //
  // The sign is the mode, as it is for the Nt timer calls underneath: negative
  // is relative in 100ns units, positive is an absolute UTC file time. The
  // older API takes neither, only a millisecond delay, so an absolute time has
  // to be differenced against the clock here.
  DWORD DueTimeToMs(const FILETIME* due) {
    LARGE_INTEGER value;
    value.LowPart  = due->dwLowDateTime;
    value.HighPart = static_cast<LONG>(due->dwHighDateTime);

    LONGLONG hundred_ns = value.QuadPart;
    if (hundred_ns < 0) {
      hundred_ns = -hundred_ns;
    } else {
      FILETIME now_ft = {};
      ::GetSystemTimeAsFileTime(&now_ft);
      LARGE_INTEGER now;
      now.LowPart  = now_ft.dwLowDateTime;
      now.HighPart = static_cast<LONG>(now_ft.dwHighDateTime);
      hundred_ns -= now.QuadPart;
      if (hundred_ns <= 0) {
        return 0; // Already due; fire as soon as the pool can.
      }
    }

    const LONGLONG ms = hundred_ns / 10000;
    // INFINITE as a period would mean "never" to the legacy API, which is not
    // what an enormous due time means, so stop one short of it.
    if (ms >= static_cast<LONGLONG>(INFINITE)) {
      return INFINITE - 1;
    }
    return static_cast<DWORD>(ms);
  }

  // Disarming is two steps on purpose, and the split is not stylistic.
  //
  // Cancelling has to be synchronous to be worth anything: DeleteTimerQueueTimer
  // with a null completion event returns while a callback may already be queued,
  // so a timer "cancelled" that way can still deliver one afterwards -- which
  // for SetThreadpoolTimer(NULL) is precisely the thing the caller asked not to
  // happen. INVALID_HANDLE_VALUE makes it wait for that callback instead.
  //
  // But it must not wait while holding the object's lock, because the
  // trampoline takes that same lock on entry: the deleter would be waiting for
  // a callback that is waiting for the lock the deleter holds. So the handle is
  // taken out of the object under the lock, and the waiting happens after it is
  // dropped.
  //
  // And it must not wait at all when called from inside its own callback, which
  // the documented API permits -- a one-shot rescheduling itself does exactly
  // that. There the wait would be for the caller's own thread to finish, which
  // never comes. The asynchronous form is only safe there for a timer with
  // nothing queued behind the running callback, which is a spent one-shot; see
  // CancelTimer for what a live timer gets instead.
  HANDLE DetachTimerLocked(TimerObject* timer) {
    HANDLE handle      = timer->queue_timer;
    timer->queue_timer = nullptr;
    return handle;
  }

  void FinishTimerDelete(HANDLE handle, bool from_callback) {
    if (handle == nullptr) {
      return;
    }
    ::DeleteTimerQueueTimer(nullptr, handle, from_callback ? nullptr : INVALID_HANDLE_VALUE);
  }

  HANDLE DetachWaitLocked(WaitObject* wait) {
    HANDLE handle = wait->wait;
    wait->wait    = nullptr;
    wait->object  = nullptr;
    return handle;
  }

  void FinishWaitUnregister(HANDLE handle, bool from_callback) {
    if (handle == nullptr) {
      return;
    }
    ::UnregisterWaitEx(handle, from_callback ? nullptr : INVALID_HANDLE_VALUE);
  }

  // As far out as the legacy API can be asked to wait: INFINITE itself is not
  // a due time it accepts as "never".
  constexpr DWORD kParkedDueMs = INFINITE - 1;

  bool OnCallbackThreadLocked(const Common* common);

  // Stops a timer: no further callbacks are started once this returns, except
  // from inside the timer's own callback, where one already queued may still
  // run.
  //
  // That exception is why the in-callback case does not delete anything. A
  // non-blocking DeleteTimerQueueTimer gives the handle up, and the handle is
  // the only thing a later close can block on to learn that the dispatcher has
  // let go -- a callback that is queued but has not started has linked no
  // frame, so the idle event cannot see it. Lose the handle here and that
  // close frees the object under it. So a live timer is parked instead: pushed
  // as far into the future as the API allows and made one-shot, with the
  // handle left in place for whoever tears the object down, from a thread that
  // is allowed to wait.
  //
  // A spent one-shot has nothing behind it and nothing to park -- changing an
  // expired one-shot does nothing at all -- so it takes the asynchronous
  // delete, which is safe precisely because it has already fired.
  void CancelTimer(TimerObject* timer) {
    ::EnterCriticalSection(&timer->common.lock);
    const bool from_callback = OnCallbackThreadLocked(&timer->common);
    if (from_callback && timer->queue_timer != nullptr && !timer->spent &&
        ::ChangeTimerQueueTimer(nullptr, timer->queue_timer, kParkedDueMs, 0)) {
      timer->period = 0;
      ::LeaveCriticalSection(&timer->common.lock);
      return;
    }
    HANDLE stale = DetachTimerLocked(timer);
    ::LeaveCriticalSection(&timer->common.lock);
    FinishTimerDelete(stale, from_callback);
  }

  // Whether this thread is inside one of the object's callbacks. Called with
  // the lock held by every caller, because it decides between a blocking
  // teardown and an asynchronous one, and because the list it walks is only
  // stable under that lock.
  bool OnCallbackThreadLocked(const Common* common) {
    const DWORD self = ::GetCurrentThreadId();
    for (const CallbackFrame* frame = common->frames; frame != nullptr; frame = frame->next) {
      if (frame->thread == self) {
        return true;
      }
    }
    return false;
  }

  // Waits for running callbacks to finish, unless this thread is one of them.
  //
  // The real functions deadlock when called from their own callback; refusing
  // to is a deliberate difference. Nothing is gained by reproducing a hang,
  // and a caller asking "are the callbacks done" from inside one has its answer
  // already: all but this one.
  //
  // The lock is dropped before waiting, since a callback needs it to finish.
  // Whatever changes in between is harmless: a callback that ended there has
  // signalled the event, so the wait returns at once.
  void WaitForIdle(Common* common) {
    ::EnterCriticalSection(&common->lock);
    const bool own = OnCallbackThreadLocked(common);
    ::LeaveCriticalSection(&common->lock);
    if (own) {
      return;
    }
    ::WaitForSingleObject(common->idle, INFINITE);
  }

  // Teardown for the one case that cannot tear itself down: a close that comes
  // from inside the object's own callback.
  //
  // The problem is not the callback that is running, it is the one that might
  // be queued behind it. DeleteTimerQueueTimer and UnregisterWaitEx only
  // promise that no callback is pending once they have *waited*, and waiting is
  // exactly what a callback thread cannot do about itself. Freeing without that
  // promise is a use-after-free for a periodic timer, whose next period can
  // already be queued; freeing with it is a deadlock.
  //
  // So the handle and the object go to a pool thread, which is under no such
  // restriction: it blocks until the dispatcher has let go -- including on the
  // callback that asked for the close, which by then has returned -- and only
  // then destroys anything. WT_EXECUTELONGFUNCTION because blocking is the
  // whole job.
  struct Reaper {
    void* object;  // The TimerObject or WaitObject; Common is its first member.
    HANDLE handle; // The queue timer or the wait registration.
    bool is_timer;
  };

  DWORD WINAPI ReaperProc(LPVOID parameter) {
    auto* reaper = static_cast<Reaper*>(parameter);
    auto* common = static_cast<Common*>(reaper->object); // Common is first.

    // Two waits, because they answer different questions and neither implies
    // the other.
    //
    // The blocking teardown is the only thing that promises the dispatcher
    // will not start another callback -- a queued one has not linked a frame
    // yet, so no amount of looking at the object would reveal it.
    if (reaper->handle != nullptr) {
      if (reaper->is_timer) {
        ::DeleteTimerQueueTimer(nullptr, reaper->handle, INVALID_HANDLE_VALUE);
      } else {
        ::UnregisterWaitEx(reaper->handle, INVALID_HANDLE_VALUE);
      }
    }

    // And the idle event is what says the callback that asked for the close
    // has returned. Needed on its own account, not just as insurance: a
    // one-shot that re-armed nothing and closed itself was spent, so its
    // handle went the asynchronous way and there is none above -- and then
    // the teardown would wait for nothing at all while its own caller is
    // still running.
    ::WaitForSingleObject(common->idle, INFINITE);

    DestroyCommon(common);
    ::HeapFree(::GetProcessHeap(), 0, reaper->object);
    ::HeapFree(::GetProcessHeap(), 0, reaper);
    return 0;
  }

  // False if the work item could not be queued, which leaves the caller to do
  // the best it can rather than losing the object.
  bool Reap(void* object, HANDLE handle, bool is_timer) {
    auto* reaper = static_cast<Reaper*>(
        ::HeapAlloc(::GetProcessHeap(), 0, sizeof(Reaper)));
    if (reaper == nullptr) {
      return false;
    }
    reaper->object   = object;
    reaper->handle   = handle;
    reaper->is_timer = is_timer;
    if (!::QueueUserWorkItem(ReaperProc, reaper, WT_EXECUTELONGFUNCTION)) {
      ::HeapFree(::GetProcessHeap(), 0, reaper);
      return false;
    }
    return true;
  }

} // namespace

extern "C" {

PTP_TIMER WINAPI ww_CreateThreadpoolTimer(PTP_TIMER_CALLBACK pfnti,
                                          PVOID pv,
                                          PTP_CALLBACK_ENVIRON pcbe) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CreateThreadpoolTimer)) {
    return system_fn(pfnti, pv, pcbe);
  }
  if (pfnti == nullptr) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }

  // pcbe is ignored, and that is the one place this group is loose rather than
  // strict. The environment selects a private pool, a cleanup group, a
  // callback priority and a few other knobs, none of which the legacy timer
  // queues have; the default process pool is all there is. Refusing a non-null
  // environment would be the choice this library makes elsewhere -- see
  // LCMapStringEx's sort handle -- but it is the wrong one here. An
  // environment changes where the callback runs, not what it computes, and the
  // caller is the Concurrency Runtime, which would meet a null return by
  // failing outright rather than by falling back. Ignoring a scheduling hint
  // costs less than that.
  (void)pcbe;

  auto* timer = static_cast<TimerObject*>(
      ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TimerObject)));
  if (timer == nullptr) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
  if (!InitCommon(&timer->common, kTimerSignature, pv)) {
    ::HeapFree(::GetProcessHeap(), 0, timer);
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
  timer->callback    = pfnti;
  timer->queue_timer = nullptr;
  timer->period      = 0;
  timer->spent       = false;
  return reinterpret_cast<PTP_TIMER>(timer);
}

VOID WINAPI ww_SetThreadpoolTimer(PTP_TIMER pti,
                                  PFILETIME pftDueTime,
                                  DWORD msPeriod,
                                  DWORD msWindowLength) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, SetThreadpoolTimer)) {
    system_fn(pti, pftDueTime, msPeriod, msWindowLength);
    return;
  }
  auto* timer = reinterpret_cast<TimerObject*>(pti);
  if (timer == nullptr || timer->common.signature != kTimerSignature) {
    return; // Not one of ours; the real function has no error to report either.
  }

  // msWindowLength is a coalescing tolerance: it lets Windows slide the firing
  // to join other timers and save power. There is nothing to ask for it with
  // before Vista, so it is dropped. Firing on time is always within tolerance,
  // which is why this is a hint lost rather than behaviour changed.
  (void)msWindowLength;

  if (pftDueTime == nullptr) {
    // The documented way to stop a timer. Pending callbacks are cancelled;
    // a running one is not interrupted, exactly as the real function leaves it.
    CancelTimer(timer);
    return;
  }

  const DWORD due_ms = DueTimeToMs(pftDueTime);

  ::EnterCriticalSection(&timer->common.lock);
  const bool from_callback = OnCallbackThreadLocked(&timer->common);

  if (timer->queue_timer != nullptr) {
    // Already armed: move it rather than tearing it down, which keeps any
    // callback currently running untouched and needs no teardown at all. Not
    // for a one-shot that has fired, though -- the legacy call leaves an
    // expired one-shot exactly as it was, so the re-arm would be lost.
    if (!timer->spent &&
        ::ChangeTimerQueueTimer(nullptr, timer->queue_timer, due_ms, msPeriod)) {
      timer->period = msPeriod;
      ::LeaveCriticalSection(&timer->common.lock);
      return;
    }
    // Spent, or the change was refused, so start again from a fresh timer.
    HANDLE stale = DetachTimerLocked(timer);
    ::LeaveCriticalSection(&timer->common.lock);
    FinishTimerDelete(stale, from_callback);
    ::EnterCriticalSection(&timer->common.lock);
  }

  // WT_EXECUTEDEFAULT: a pool thread, which is what the Vista pool would use.
  // No WT_EXECUTEONLYONCE even for a one-shot -- a zero period already means
  // once, and the flag would additionally forbid re-arming this handle.
  HANDLE created = nullptr;
  if (::CreateTimerQueueTimer(&created, nullptr, TimerTrampoline, timer, due_ms, msPeriod,
                              WT_EXECUTEDEFAULT)) {
    timer->queue_timer = created;
    timer->period      = msPeriod;
    timer->spent       = false;
  }
  ::LeaveCriticalSection(&timer->common.lock);
}

VOID WINAPI ww_WaitForThreadpoolTimerCallbacks(PTP_TIMER pti, BOOL fCancelPendingCallbacks) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, WaitForThreadpoolTimerCallbacks)) {
    system_fn(pti, fCancelPendingCallbacks);
    return;
  }
  auto* timer = reinterpret_cast<TimerObject*>(pti);
  if (timer == nullptr || timer->common.signature != kTimerSignature) {
    return;
  }

  if (fCancelPendingCallbacks) {
    CancelTimer(timer);
  }
  WaitForIdle(&timer->common);
}

VOID WINAPI ww_CloseThreadpoolTimer(PTP_TIMER pti) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CloseThreadpoolTimer)) {
    system_fn(pti);
    return;
  }
  auto* timer = reinterpret_cast<TimerObject*>(pti);
  if (timer == nullptr || timer->common.signature != kTimerSignature) {
    return;
  }

  ::EnterCriticalSection(&timer->common.lock);
  const bool in_own_callback = OnCallbackThreadLocked(&timer->common);
  HANDLE stale               = DetachTimerLocked(timer);
  ::LeaveCriticalSection(&timer->common.lock);

  if (in_own_callback) {
    // Closing from inside the callback is legal, and neither freeing here nor
    // waiting here is possible -- see Reap, which does both from somewhere it
    // is allowed to block.
    if (Reap(timer, stale, true)) {
      return;
    }
    // The work item could not be queued, so this is the last chance to let the
    // dispatcher go. Deliberately not freeing the object: a leak of one small
    // allocation beats handing a periodic timer's next callback a dangling
    // pointer, and this is reached only when the process is out of memory.
    FinishTimerDelete(stale, true);
    return;
  }
  FinishTimerDelete(stale, false);

  // Not our callback, so there may be one running on another thread. Waiting
  // is stricter than the real function, which documents a race the caller is
  // expected to have closed with WaitForThreadpoolTimerCallbacks first. Doing
  // it anyway costs a correct caller nothing and turns an incorrect one's
  // use-after-free into a short wait.
  WaitForIdle(&timer->common);

  DestroyCommon(&timer->common);
  ::HeapFree(::GetProcessHeap(), 0, timer);
}

PTP_WAIT WINAPI ww_CreateThreadpoolWait(PTP_WAIT_CALLBACK pfnwa,
                                        PVOID pv,
                                        PTP_CALLBACK_ENVIRON pcbe) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CreateThreadpoolWait)) {
    return system_fn(pfnwa, pv, pcbe);
  }
  if (pfnwa == nullptr) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  (void)pcbe; // As in ww_CreateThreadpoolTimer.

  auto* wait = static_cast<WaitObject*>(
      ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WaitObject)));
  if (wait == nullptr) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
  if (!InitCommon(&wait->common, kWaitSignature, pv)) {
    ::HeapFree(::GetProcessHeap(), 0, wait);
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
  wait->callback = pfnwa;
  wait->wait     = nullptr;
  wait->object   = nullptr;
  return reinterpret_cast<PTP_WAIT>(wait);
}

VOID WINAPI ww_SetThreadpoolWait(PTP_WAIT pwa, HANDLE h, PFILETIME pftTimeout) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, SetThreadpoolWait)) {
    system_fn(pwa, h, pftTimeout);
    return;
  }
  auto* wait = reinterpret_cast<WaitObject*>(pwa);
  if (wait == nullptr || wait->common.signature != kWaitSignature) {
    return;
  }

  ::EnterCriticalSection(&wait->common.lock);
  const bool from_callback = OnCallbackThreadLocked(&wait->common);

  // Whatever was registered before is replaced, armed or not. A null handle is
  // the documented way to cancel. Unregistered outside the lock for the same
  // reason a timer is deleted outside it -- UnregisterWaitEx waits for the
  // callback, and the callback wants this lock.
  HANDLE stale = DetachWaitLocked(wait);
  ::LeaveCriticalSection(&wait->common.lock);
  FinishWaitUnregister(stale, from_callback);

  if (h == nullptr || h == INVALID_HANDLE_VALUE) {
    return;
  }
  ::EnterCriticalSection(&wait->common.lock);

  // A null timeout is an indefinite wait; otherwise the same sign convention
  // as a timer's due time.
  const DWORD timeout_ms = pftTimeout == nullptr ? INFINITE : DueTimeToMs(pftTimeout);

  // WT_EXECUTEONLYONCE, which is the Vista object's contract rather than a
  // simplification: a TP_WAIT fires once and has to be re-armed with another
  // SetThreadpoolWait. Without the flag the legacy wait would rearm itself and
  // deliver callbacks the caller never asked for.
  HANDLE registered = nullptr;
  if (::RegisterWaitForSingleObject(&registered, h, WaitTrampoline, wait, timeout_ms,
                                    WT_EXECUTEONLYONCE)) {
    wait->wait   = registered;
    wait->object = h;
  }
  ::LeaveCriticalSection(&wait->common.lock);
}

VOID WINAPI ww_WaitForThreadpoolWaitCallbacks(PTP_WAIT pwa, BOOL fCancelPendingCallbacks) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, WaitForThreadpoolWaitCallbacks)) {
    system_fn(pwa, fCancelPendingCallbacks);
    return;
  }
  auto* wait = reinterpret_cast<WaitObject*>(pwa);
  if (wait == nullptr || wait->common.signature != kWaitSignature) {
    return;
  }

  if (fCancelPendingCallbacks) {
    ::EnterCriticalSection(&wait->common.lock);
    const bool from_callback = OnCallbackThreadLocked(&wait->common);
    HANDLE stale             = DetachWaitLocked(wait);
    ::LeaveCriticalSection(&wait->common.lock);
    FinishWaitUnregister(stale, from_callback);
  }
  WaitForIdle(&wait->common);
}

VOID WINAPI ww_CloseThreadpoolWait(PTP_WAIT pwa) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CloseThreadpoolWait)) {
    system_fn(pwa);
    return;
  }
  auto* wait = reinterpret_cast<WaitObject*>(pwa);
  if (wait == nullptr || wait->common.signature != kWaitSignature) {
    return;
  }

  ::EnterCriticalSection(&wait->common.lock);
  const bool in_own_callback = OnCallbackThreadLocked(&wait->common);
  HANDLE stale               = DetachWaitLocked(wait);
  ::LeaveCriticalSection(&wait->common.lock);

  if (in_own_callback) {
    // As in ww_CloseThreadpoolTimer. A one-shot wait cannot have another
    // callback queued behind it, so this is less load-bearing here than it is
    // there -- but it is the same situation and the same answer, and keeping
    // one of the two hand-rolled would be the version that rots.
    if (Reap(wait, stale, false)) {
      return;
    }
    FinishWaitUnregister(stale, true);
    return;
  }
  FinishWaitUnregister(stale, false);

  WaitForIdle(&wait->common);

  DestroyCommon(&wait->common);
  ::HeapFree(::GetProcessHeap(), 0, wait);
}

// Unload a library once the current callback returns. kernel32, Vista.
//
// The real function records the module against the callback instance and the
// pool calls FreeLibrary after the callback returns, which lets a DLL schedule
// work and then unload itself without pulling the ground out from under the
// running callback. The deferral is the whole point: calling FreeLibrary
// directly from inside a callback that lives in that DLL frees the code
// currently executing.
//
// So the instance names the object, and the module is recorded on the frame
// the calling thread linked into it -- one per running callback, not one per
// object. The trampoline calls FreeLibrary after that callback returns and
// after the lock is dropped. An instance that is not one of winwrap's is
// ignored -- there is nowhere to record it, and writing through a misread
// pointer would be worse than not honouring the request.
VOID WINAPI ww_FreeLibraryWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HMODULE mod) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, FreeLibraryWhenCallbackReturns)) {
    system_fn(pci, mod);
    return;
  }
  auto* common = reinterpret_cast<Common*>(pci);
  if (common == nullptr || mod == nullptr) {
    return;
  }
  if (common->signature != kTimerSignature && common->signature != kWaitSignature) {
    return;
  }

  ::EnterCriticalSection(&common->lock);
  // Against this thread's own frame, and only if it has one. Anywhere else
  // there is no "when the callback returns" to attach this to, and queueing it
  // against whatever runs next would unload the library at an unrelated
  // moment. Two callbacks of the same object running at once each get their
  // own answer, which is what the real function's per-callback unit means.
  const DWORD self = ::GetCurrentThreadId();
  for (CallbackFrame* frame = common->frames; frame != nullptr; frame = frame->next) {
    if (frame->thread == self) {
      frame->free_on_return = mod;
      break;
    }
  }
  ::LeaveCriticalSection(&common->lock);
}

} // extern "C"
