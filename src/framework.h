#ifndef WINWRAP_FRAMEWORK_H_
#define WINWRAP_FRAMEWORK_H_

#ifndef UNICODE
 #define UNICODE
#endif

#ifndef _UNICODE
 #define _UNICODE
#endif

/* Including SDKDDKVer.h defines the highest available Windows platform.
   If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
   set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h. */

#include "winsdkver.h"

#ifndef _WIN32_WINNT
 #define _WIN32_WINNT 0x0501 // Windows XP
#endif

#ifndef WINVER
 #define WINVER 0x0501 // Same as _WIN32_WINNT above
#endif

#if _WIN32_WINNT < 0x0601 // If we are less than Windows 7, use old ATL for safety
 #ifndef _ATL_XP_TARGETING
  #define _ATL_XP_TARGETING // For using XP-compatible ATL/MFC functions
 #endif
#endif

#include "sdkddkver.h"

// clang-format off
#ifdef _MSC_VER
 #define WIN32_NO_STATUS
#endif
#include <windows.h> // Main Windows header
#include <winnt.h>
#ifdef _MSC_VER
 #undef WIN32_NO_STATUS
#endif
#include <winternl.h>
#include <ntstatus.h> // Full NTSTATUS codes (e.g. STATUS_SUCCESS)
// clang-format on

#include <stdlib.h> // wcstoul

#include <cstdint> // uint32_t, uint64_t
#include <cwchar>  // wcslen, wcsncmp
// NOT <string>. It was here and unused -- nothing in winwrap names
// std::string or std::wstring outside a comment -- and MSVC's <string> drags
// in the locale machinery, which imports CompareStringEx, LCMapStringEx and
// GetLocaleInfoEx. Those are Vista+, so an unused include was adding
// load-time requirements to every consumer on the oldest target this library
// exists to support. winwrap shims all three now, but it should not be
// creating the need for its own shims.

// STATUS_NO_MEMORY as RaiseException wants it.
//
// RaiseException takes a DWORD, and ntstatus.h types the STATUS_* codes as
// NTSTATUS -- a signed LONG, negative for every failure code, since they all
// carry severity in the top two bits. Passing one straight through is a
// signed/unsigned conversion, which MSVC reports as C4245 at every call site.
//
// A cast rather than a different code: the EXCEPTION_* family has no spelling
// of "out of memory", and this is the value the lazy-initialisation paths want
// when they cannot allocate a lock and have no way to report it. Named here,
// next to the include that decides the type, so the reason lives in one place
// rather than as four unexplained casts across three files.
//
// Worth knowing why it warns only in some configurations: winnt.h defines the
// same macro as a DWORD, and framework.h asks for WIN32_NO_STATUS above so
// that ntstatus.h's definitions are the ones in force.
constexpr DWORD kStatusNoMemory = static_cast<DWORD>(STATUS_NO_MEMORY);

#endif // WINWRAP_FRAMEWORK_H_
