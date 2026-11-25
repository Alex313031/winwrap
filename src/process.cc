// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Process-wide state: the error mode, and DPI awareness.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  // PROCESSINFOCLASS as winternl.h declares it names only a few members, and
  // this is not among them.
  constexpr ULONG kProcessDefaultHardErrorMode = 12;

  using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

  // psapi's two ways of naming a process's executable, both in XP. psapi is
  // loaded on first use: winwrap links kernel32 and advapi32 only, and psapi
  // need not be in the process already.
  using GetProcessImageFileNameWFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD);
  using GetModuleFileNameExWFn     = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);

  std::atomic<void*> g_psapi{nullptr};
  std::atomic<void*> g_image_file_name{nullptr};
  std::atomic<void*> g_module_file_name{nullptr};

  constexpr DWORD kProcessNameNative = 0x00000001; // PROCESS_NAME_NATIVE

  // The longest path NT has: 32767 characters and a terminator. The scratch
  // buffers are this size whatever the caller's is, so that a path too long
  // for the caller is reported as too long for the caller -- and a caller
  // that grows its buffer and tries again gets somewhere. Too big for the
  // stack, hence the heap.
  constexpr DWORD kMaxNativePath = 32768;

  // A zeroed array of wide characters from the process heap, freed on the way
  // out. Not the CRT's allocator, which winwrap otherwise has no use for.
  class WideBuffer {
   public:
    explicit WideBuffer(DWORD characters)
        : text_(static_cast<wchar_t*>(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                  characters * sizeof(wchar_t)))) {}
    ~WideBuffer() {
      if (text_) {
        ::HeapFree(::GetProcessHeap(), 0, text_);
      }
    }
    WideBuffer(const WideBuffer&)            = delete;
    WideBuffer& operator=(const WideBuffer&) = delete;

    wchar_t* get() const { return text_; }

   private:
    wchar_t* text_;
  };

  bool StartsWithNoCase(const wchar_t* text, const wchar_t* prefix, size_t prefix_length) {
    return ::CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, text,
                            static_cast<int>(prefix_length), prefix,
                            static_cast<int>(prefix_length)) == CSTR_EQUAL;
  }

  enum class Translation { kNoMatch, kDone, kTooLong };

  // One drive letter's worth of NativeToDosPath, below: given that `letter`
  // maps to `device`, rewrites `native` into `dos` if the mapping applies to
  // it. Separate from the loop that asks Windows for the mappings so that the
  // string handling stands by itself.
  //
  // A mapping comes in two shapes.
  //
  // A local volume maps to a bare device, \Device\HarddiskVolume1, and a path
  // on it is that device followed by the rest: the device is replaced by the
  // letter.
  //
  // A mapped network drive maps to the redirector's device, then a component
  // beginning with a semicolon that names the letter and the logon session,
  // then the server and share:
  //
  //     \Device\LanmanRedirector\;Z:000000000001a2b3\server\share
  //     \Device\VBoxMiniRdr\;Z:\VBoxSvr\share
  //
  // The kernel's name for a file reached through it has no such component --
  // \Device\VBoxMiniRdr\VBoxSvr\share\dir\x.exe -- so the two never match as
  // strings, and for some time this function simply failed for anything run
  // from a share. What the mapping does establish is that the device is a
  // network redirector, and for one of those everything after the device is
  // the server, the share and the path: a UNC name short of one backslash.
  // That is the form the real function returns for a program on a share --
  // \\server\share\dir\x.exe, not the drive letter -- so it is the form
  // returned here, and it is right for any share on that redirector, mapped
  // or not.
  Translation TranslateWithMapping(const wchar_t* native,
                                   wchar_t letter,
                                   const wchar_t* device,
                                   wchar_t* dos,
                                   DWORD dos_chars) {
    const size_t native_length = wcslen(native);
    const wchar_t* session     = wcsstr(device, L"\\;");
    const size_t device_length = session ? static_cast<size_t>(session - device) : wcslen(device);

    // The device name has to be the whole of the first components, not a
    // prefix of them: HarddiskVolume1 is not HarddiskVolume10.
    if (device_length == 0 || device_length >= native_length ||
        native[device_length] != L'\\' || !StartsWithNoCase(native, device, device_length)) {
      return Translation::kNoMatch;
    }
    const wchar_t* rest      = native + device_length; // Begins with a backslash.
    const size_t rest_length = native_length - device_length;

    if (session) {
      // "\" + "\server\share\dir\x.exe".
      if (1 + rest_length + 1 > dos_chars) {
        return Translation::kTooLong;
      }
      dos[0] = L'\\';
      memcpy(dos + 1, rest, (rest_length + 1) * sizeof(wchar_t));
      return Translation::kDone;
    }
    // "C:" + "\dir\x.exe".
    if (2 + rest_length + 1 > dos_chars) {
      return Translation::kTooLong;
    }
    dos[0] = letter;
    dos[1] = L':';
    memcpy(dos + 2, rest, (rest_length + 1) * sizeof(wchar_t));
    return Translation::kDone;
  }

  // Rewrites `native`, a path such as \Device\HarddiskVolume1\Windows\x.exe,
  // as the path a Win32 program expects, into `dos`. The kernel knows a file
  // by its device; which letter that device wears is a mapping kept per
  // session, and QueryDosDeviceW reads it back one letter at a time. False if
  // no mapping accounts for the path's device.
  bool NativeToDosPath(const wchar_t* native, wchar_t* dos, DWORD dos_chars) {
    wchar_t drives[512] = {};
    const DWORD length  = ::GetLogicalDriveStringsW(ARRAYSIZE(drives) - 1, drives);
    if (length == 0 || length >= ARRAYSIZE(drives)) {
      return false;
    }

    // "C:\", then a terminator, then the next, ending with an empty string.
    for (const wchar_t* drive = drives; *drive != L'\0'; drive += wcslen(drive) + 1) {
      const wchar_t letter[3]  = {drive[0], L':', L'\0'};
      wchar_t device[MAX_PATH] = {};
      if (::QueryDosDeviceW(letter, device, ARRAYSIZE(device)) == 0) {
        continue;
      }
      switch (TranslateWithMapping(native, letter[0], device, dos, dos_chars)) {
        case Translation::kDone:
          return true;
        case Translation::kTooLong:
          return false;
        case Translation::kNoMatch:
          break;
      }
    }
    return false;
  }

} // namespace

extern "C" {

// The process error mode. kernel32, Vista.
//
// XP can set this and never offered a way to read it back, which is the whole
// of what Vista added. The value is in the process information the kernel
// already keeps, so this asks for it directly.
//
// The one subtlety is that NT and Win32 disagree about the sense of the low
// bit. NT's ProcessDefaultHardErrorMode sets bit 0 when hard errors *are*
// displayed; Win32's SEM_FAILCRITICALERRORS sets the same bit to say they are
// *not*. The two are inverses, so the bit is flipped on the way out. Getting
// that backwards would be invisible in testing and exactly wrong in
// production, which is the kind of detail worth taking from someone who has
// already been through it -- YY-Thunks does the same inversion.
UINT WINAPI ww_GetErrorMode(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetErrorMode)) {
    return system_fn();
  }
  const auto query = WW_TRYFUNC_AS(NtQueryInformationProcessFn, ::winwrap::Module::hNtDll,
                                   "NtQueryInformationProcess");

  if (query) {
    DWORD hard_error_mode = 0;
    NTSTATUS status = query(::GetCurrentProcess(), kProcessDefaultHardErrorMode, &hard_error_mode,
                            sizeof(hard_error_mode), nullptr);
    if (status >= 0) {
      return (hard_error_mode & 1) ? (hard_error_mode & ~1u) : (hard_error_mode | 1u);
    }
  }

  // Fallback for the case that cannot happen, ntdll being absent: SetErrorMode
  // returns the previous mode, so setting and restoring reads it. Deliberately
  // second, because it is not atomic -- another thread that calls SetErrorMode
  // or raises a hard error in between sees the mode momentarily cleared.
  UINT previous = ::SetErrorMode(0);
  ::SetErrorMode(previous);
  return previous;
}

// Declare the process DPI aware. user32, Vista.
//
// Returns success, and that is not a polite fiction: XP has no DPI
// virtualisation to opt out of. From Vista onward a process that has not said
// this gets lied to -- GetSystemMetrics and the rest report scaled values and
// the window is stretched by the compositor -- and SetProcessDPIAware turns
// that off. XP never does any of it. Every process already sees real pixels
// and real metrics, so the state the caller is asking for is the state it is
// already in, and TRUE is the accurate answer rather than a convenient one.
//
// This is the opposite judgement to CreateSymbolicLinkW or the named pipe
// process IDs, where success would have been a lie about something that had
// not happened. The distinction is whether the postcondition holds, not
// whether any work was done.
BOOL WINAPI ww_SetProcessDPIAware(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hUser32, SetProcessDPIAware)) {
    return system_fn();
  }
  return TRUE;
}

// Force a memory barrier on every processor running this process. kernel32,
// Vista.
//
// The point of the function is to let one thread pay for synchronisation so
// that many others need not: readers publish with plain stores, and a writer
// that must see them all calls this instead of every reader taking a fence.
// It works by interrupting each processor, since taking an interrupt drains
// the store buffer.
//
// XP has no call for it, but it has one that interrupts every processor as a
// side effect. Taking write access away from a page the process has mapped
// means no processor may go on using a cached translation that still permits
// writes, so the kernel sends each one running the process an interprocessor
// interrupt to flush it. That is the trick the Concurrency Runtime -- which is where
// this import comes from -- carries as its own fallback, crediting the CLR
// with years of relying on it; YY-Thunks has it too.
//
// The interrupt is only sent if there is a live, writable translation to
// revoke, and the usual form of the trick leaves that to luck: it protects a
// page that is already read-only, and relies on the page being in the working
// set. This one makes it so on every call -- restore write access, write to
// the page, which puts a writable translation in this processor's cache and
// the page in the working set, then take the access away again. The lock is
// what makes that sequence safe: a second thread arriving between the two
// protection changes would otherwise write to a page just made read-only.
//
// A processor not running one of this process's threads is not interrupted
// and need not be. It reached that state through a context switch, and a
// context switch is itself a barrier.
VOID WINAPI ww_FlushProcessWriteBuffers(void) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, FlushProcessWriteBuffers)) {
    system_fn();
    return;
  }

  static std::atomic<void*> g_page{nullptr};
  static SRWLOCK g_lock = {};

  void* page = g_page.load(std::memory_order_acquire);
  if (page == nullptr) {
    // One page, for the life of the process. If it cannot be had there is no
    // way to report it -- the function returns nothing -- and the next call
    // tries again.
    void* fresh = ::VirtualAlloc(nullptr, 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (fresh == nullptr) {
      return;
    }
    if (g_page.compare_exchange_strong(page, fresh, std::memory_order_acq_rel)) {
      page = fresh;
    } else {
      ::VirtualFree(fresh, 0, MEM_RELEASE); // Another thread got there first.
    }
  }

  ww_AcquireSRWLockExclusive(&g_lock);
  DWORD previous = 0;
  if (::VirtualProtect(page, 1, PAGE_READWRITE, &previous)) {
    ::InterlockedIncrement(static_cast<LONG volatile*>(page));
    ::VirtualProtect(page, 1, PAGE_READONLY, &previous);
  }
  ww_ReleaseSRWLockExclusive(&g_lock);
}

// The full path of a process's executable. kernel32, Vista.
//
// Vista added it for two reasons, and XP can honour the first. It names the
// file the way the kernel knows it, from the image the process was created
// from, rather than reading a string out of the target's own memory as
// GetModuleFileNameEx does -- a string the target can rewrite, and one that
// for the earliest system processes is not a Win32 path at all. psapi's
// GetProcessImageFileNameW has given that kernel name since XP, as a device
// path; PROCESS_NAME_NATIVE asks for exactly that, and without the flag the
// device is translated back to its drive letter.
//
// The second reason was access: the real function needs only
// PROCESS_QUERY_LIMITED_INFORMATION, a right Vista introduced so that a
// process could be named without being opened wide. XP has no such right and
// the handle has to carry PROCESS_QUERY_INFORMATION.
//
// A program on a network share comes back as a UNC path, \\server\share\...,
// as the real function returns it, provided some drive letter is mapped
// through the same redirector to identify the device as one. Where nothing
// accounts for the device -- a share reached with no drive mapped at all, a
// volume mounted only as a folder -- the translation has nothing to offer,
// and the path GetModuleFileNameExW reads from the process is used instead,
// which wants PROCESS_VM_READ as well. INCOMPLETE_FUNCS.md has the entry.
BOOL WINAPI ww_QueryFullProcessImageNameW(HANDLE hProcess,
                                          DWORD dwFlags,
                                          LPWSTR lpExeName,
                                          PDWORD lpdwSize) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, QueryFullProcessImageNameW)) {
    return system_fn(hProcess, dwFlags, lpExeName, lpdwSize);
  }
  if (!lpExeName || !lpdwSize || (dwFlags & ~kProcessNameNative) != 0) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }

  const HMODULE psapi  = ::winwrap::LoadSystemLibrary(g_psapi, L"psapi.dll");
  const auto get_image = reinterpret_cast<GetProcessImageFileNameWFn>(
      ::winwrap::ResolveFrom(g_image_file_name, psapi, "GetProcessImageFileNameW"));
  if (!get_image) {
    ::SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }

  WideBuffer native_buffer(kMaxNativePath);
  WideBuffer dos_buffer(kMaxNativePath);
  wchar_t* const native = native_buffer.get();
  wchar_t* const dos    = dos_buffer.get();
  if (!native || !dos) {
    ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
  }

  const DWORD native_length = get_image(hProcess, native, kMaxNativePath);
  if (native_length == 0 || native_length >= kMaxNativePath) {
    return FALSE; // With psapi's error: access denied, or not a process.
  }

  const wchar_t* result = native;
  if ((dwFlags & kProcessNameNative) == 0) {
    if (NativeToDosPath(native, dos, kMaxNativePath)) {
      result = dos;
    } else {
      const auto get_module = reinterpret_cast<GetModuleFileNameExWFn>(
          ::winwrap::ResolveFrom(g_module_file_name, psapi, "GetModuleFileNameExW"));
      if (!get_module) {
        ::SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
      }
      const DWORD copied = get_module(hProcess, nullptr, dos, kMaxNativePath);
      if (copied == 0) {
        // With psapi's error, which says why: ERROR_ACCESS_DENIED for a
        // handle without PROCESS_VM_READ, most often.
        return FALSE;
      }
      // It truncates without saying so, and a full buffer is how that shows.
      // Not reachable with a buffer the size of the longest path there is,
      // but the terminator is not something to take on trust.
      if (copied >= kMaxNativePath - 1) {
        ::SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
      }
      result = dos;
    }
  }

  // In: the buffer's size in characters. Out: the length written, without the
  // terminator. A buffer that cannot hold the terminator too is too small.
  const size_t length = wcslen(result);
  if (*lpdwSize <= length) {
    ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
  }
  memcpy(lpExeName, result, (length + 1) * sizeof(wchar_t));
  *lpdwSize = static_cast<DWORD>(length);
  return TRUE;
}

} // extern "C"
