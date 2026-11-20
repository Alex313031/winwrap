// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINWRAP_WINWRAP_PUBLIC_H_
#define WINWRAP_WINWRAP_PUBLIC_H_

// Public definitions for extra functions that winwrap
// provides. This file should NOT contain any definitions
// for Win32 function thunks.
//
// This is the only header on a consumer's include path, and the only place
// these three are declared. extra.cc includes it directly and there is no
// private header beside it, so there is one signature per function rather than
// two to keep in step.
//
// Deliberately absent: DllGetVersion. It is exported by winwrap.dll, but
// declaring it here would put a name on a consumer's include path that half
// the system DLLs also export, and winwrap's is not the one a caller usually
// means. Reach it with GetProcAddress on the module in question.
//
// extern "C", so the symbol is the name: a namespaced or mangled C++ name is
// not something a .def can export portably, since the mangling differs between
// x86 and x64. Unprefixed, unlike the shims, because these are winwrap's own
// API rather than stand-ins for a Win32 function -- there is no other name for
// them to be exported under. The symbol has to match the export for the import
// library to satisfy a caller, so the definition is unprefixed too.

#ifndef __cplusplus
 #error This project requires a C++ compiler
#endif

#include <cstdint> // uint32_t

extern "C" {

// Gets the real NT version of the host system using RtlGetNtVersionNumbers;
// returns the value in the same format that _WIN32_WINNT takes, so for example
// on XP this would return (when displayed with std::hex) 0x0501. It can also
// be static_cast to a DWORD, since DWORD is an unsigned 32 bit value too.
// Returns 0 if the version cannot be determined, which no NT system should do.
// Compare with >=, so that an unknown version reads as "older than whatever I
// am asking about" and takes winwrap's own path, which is the safe direction.
uint32_t GetRealNTVersion();

// Gets if program is running on Wine, with optional string to get version.
//
// The string is const char* rather than std::string so that this header costs
// a consumer nothing: <string> would arrive in every translation unit that
// includes it, and an std::string crossing the winwrap.dll boundary would mean
// the caller and the library had to agree on a standard library. There is
// nothing to free -- the pointer is into ntdll's own data, as wine_get_version
// returns it, and stays valid as long as ntdll is loaded, which is for the
// life of the process.
bool IsRunningOnWine(const char** outWineVer = nullptr);

// Allows getting the winwrap version in a more easy to use format than
// DllGetVersion (which is only available for the shared .dll anyway).
//
// Returns WINWRAP_VERSION_STRING, e.g. L"0.3.8" -- a literal with static
// storage duration, so there is nothing to free and it outlives any caller.
// Every out parameter is optional: pass nullptr for the ones not wanted, or
// for all three to use the return value alone.
//
// Three components and not four, matching winwrap_version.h, which is what
// these are read from. `build` is that file's WINWRAP_BUILD_VERSION, which is
// semver's patch number under an older name; the fourth field DllGetVersion
// hands to MAKEDLLVERULL is a hard zero there precisely because winwrap has no
// fourth component for it to carry.
const wchar_t* GetWinWrapVersion(unsigned int* major, unsigned int* minor,
                                 unsigned int* build);

} // extern "C"

#endif // WINWRAP_WINWRAP_PUBLIC_H_
