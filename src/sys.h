// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINWRAP_SYS_H_
#define WINWRAP_SYS_H_

#include "framework.h"

// extern "C", so the symbol is the name: a namespaced or mangled C++ name is
// not something a .def can export portably, since the mangling differs between
// x86 and x64. Unprefixed, unlike the shims, because these are winwrap's own
// API rather than stand-ins for a Win32 function -- there is no other name for
// them to be exported under. The symbol has to match the export for the import
// library to satisfy a caller, so the definition is unprefixed too.
extern "C" {

// Gets the real NT version of the host system using RtlGetNtVersionNumbers;
// returns the value in the same format that _WIN32_WINNT takes, so for example
// on XP this would return (when displayed with std::hex) 0x0501.It can also
// also be static_cast to a DWORD, since DWORD is an unsigned 32 bit value too
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

} // extern "C"

#endif // WINWRAP_SYS_H_
