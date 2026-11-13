// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINWRAP_WINWRAP_VERSION_H_
#define WINWRAP_WINWRAP_VERSION_H_

#ifndef UNICODE
 #define UNICODE
#endif

#ifndef _UNICODE
 #define _UNICODE
#endif

// Macro to convert to string
#if !defined(_STRINGIZER_)
 #define _STRINGIZER_
 #define _STRINGIZER(in) #in
 #define STRINGIZE(in)   _STRINGIZER(in)
  // Wide-string variant: L ## "x" -> L"x". Two levels so the argument expands
 // before the L## paste widens the resulting narrow literal.
 #define _WIDEN(in) L##in
 #define WIDEN(in)  _WIDEN(in)
#endif // !defined(_STRINGIZER_)

// Builds L"major.minor.build" from a binary's own numbers; each version.h
// defines its VERSION_STRING with this.
#ifndef _VERSION
 // Run stringizer above
 #define _VERSION(major, minor, build) WIDEN(STRINGIZE(major.minor.build))
#endif // _VERSION

// Adhere to semver -> semver.org
#define WINWRAP_MAJOR_VERSION 0
#define WINWRAP_MINOR_VERSION 2
#define WINWRAP_BUILD_VERSION 4

#define WINWRAP_VERSION_STRING \
  _VERSION(WINWRAP_MAJOR_VERSION, WINWRAP_MINOR_VERSION, WINWRAP_BUILD_VERSION) // e.g. L"0.2.3"

// Base name as a BARE token (not a "..." literal) so STRINGIZE can fold it
// into the composite literals below.
#define INTERNAL_TOKENS winwrap

#define FILE_DESCRIPT L"Legacy Win32 Support Library"   // File description
#define INTERNAL_NAME WIDEN(STRINGIZE(INTERNAL_TOKENS)) // L"winwrap"
#ifdef WINWRAP_DLL
 #define ORIG_FILENAME WIDEN(STRINGIZE(INTERNAL_TOKENS.dll)) // L"winwrap.dll"
#else
 #define ORIG_FILENAME WIDEN(STRINGIZE(INTERNAL_TOKENS.lib)) // L"winwrap.lib"
#endif
#define PRODUCT_NAME INTERNAL_NAME // Product name

#ifndef VS_FF_RELEASE
 #define VS_FF_RELEASE 0x00000000L
#endif

// Vendor strings shared by every binary's VERSIONINFO
#define COMMENTS        L"https://github.com/Alex313031/electron-legacy" // Project GitHub URL
#define COMPANYNAME     L"Alex313031"                                    // My developer name
#define TRADEMARKS      L"BSD-3"                                         // License
#define LEGAL_COPYRIGHT L"(c) 2026 Alex313031" // ASCII on purpose: see winwrap.rc

#endif // WINWRAP_WINWRAP_VERSION_H_
