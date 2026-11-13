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
#include <windows.h>
#include <winternl.h>
// clang-format on

#include <stdlib.h> // wcstoul
#include <string.h> // wcslen, wcsncmp

#endif // WINWRAP_FRAMEWORK_H_
