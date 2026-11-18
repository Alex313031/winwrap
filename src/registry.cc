// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Registry access. kernel32 forwards these; advapi32 implements them.
//
// RegGetValue is Vista. It is RegQueryValueEx plus the three things callers
// always had to do by hand afterwards, and getting any of them wrong is a
// bug that only shows up on bad data:
//
//   - a type filter, so a value of an unexpected type is an error rather than
//     bytes reinterpreted as whatever was expected;
//   - guaranteed termination for string types, which RegQueryValueEx does not
//     provide -- a REG_SZ written without its terminator comes back
//     unterminated, and the caller reads off the end;
//   - environment expansion for REG_EXPAND_SZ, unless asked not to.
//
// All three are built here on calls XP does have. The alternative considered
// was delegating to shlwapi's SHRegGetValueW, which XP SP2 has and which is
// the same function under another name -- the route YY-Thunks takes. It is
// rejected because it would make winwrap depend on a DLL it otherwise does
// not touch, to borrow logic short enough to own outright.
//
// Only the wide form is provided; nothing imports RegGetValueA, and an ANSI
// variant would be a conversion wrapper over this.

#include "dispatch.h"
#include "winwrap.h"

namespace {

// The RRF_ family is gated on NTDDI_VISTA, like the function itself.
#if !defined(RRF_RT_ANY)
 #define RRF_RT_REG_NONE      0x00000001
 #define RRF_RT_REG_SZ        0x00000002
 #define RRF_RT_REG_EXPAND_SZ 0x00000004
 #define RRF_RT_REG_BINARY    0x00000008
 #define RRF_RT_REG_DWORD     0x00000010
 #define RRF_RT_REG_MULTI_SZ  0x00000020
 #define RRF_RT_REG_QWORD     0x00000040
 #define RRF_RT_ANY           0x0000ffff
 #define RRF_NOEXPAND         0x10000000
 #define RRF_ZEROONFAILURE    0x20000000
#endif

  // Whether a stored type satisfies the caller's filter.
  bool TypeIsAllowed(DWORD type, DWORD flags) {
    DWORD bit = 0;
    switch (type) {
      case REG_NONE:
        bit = RRF_RT_REG_NONE;
        break;
      case REG_SZ:
        bit = RRF_RT_REG_SZ;
        break;
      case REG_EXPAND_SZ:
        bit = RRF_RT_REG_EXPAND_SZ;
        break;
      case REG_BINARY:
        bit = RRF_RT_REG_BINARY;
        break;
      case REG_DWORD:
        bit = RRF_RT_REG_DWORD;
        break;
      case REG_MULTI_SZ:
        bit = RRF_RT_REG_MULTI_SZ;
        break;
      case REG_QWORD:
        bit = RRF_RT_REG_QWORD;
        break;
      default:
        return false;
    }
    if ((flags & bit) != 0) {
      return true;
    }
    // An expandable string that is about to be expanded satisfies a request for
    // a plain string, because that is what the caller will receive.
    return type == REG_EXPAND_SZ && (flags & RRF_NOEXPAND) == 0 && (flags & RRF_RT_REG_SZ) != 0;
  }

  bool IsStringType(DWORD type) {
    return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ;
  }

  // How many terminating characters a string type needs beyond its content.
  // REG_MULTI_SZ ends with an empty string, so two.
  size_t TerminatorChars(DWORD type) {
    if (type == REG_MULTI_SZ) {
      return 2;
    }
    return IsStringType(type) ? 1 : 0;
  }

} // namespace

extern "C" {

// The implementation XP actually runs, split out of ww_RegGetValueW so that a
// test can reach it on a machine that has the real function.
//
// Without the split there is nowhere to exercise this. ww_RegGetValueW hands
// over to advapi32 wherever advapi32 has RegGetValueW, so on Vista and later a
// test calling the public name compares the system function with itself and
// cannot fail; on XP there is no system function to compare against. Neither
// arrangement says whether this code is right.
//
// Not exported from winwrap.dll -- it is winwrap's own business, not API. The
// unit test reaches it in the static form only, where linking the archive
// makes every symbol in it available.
LSTATUS APIENTRY winwrap_RegGetValueFallbackW(HKEY hkey,
                                              LPCWSTR lpSubKey,
                                              LPCWSTR lpValue,
                                              DWORD dwFlags,
                                              LPDWORD pdwType,
                                              PVOID pvData,
                                              LPDWORD pcbData) {
  // A buffer with no size is unusable, and a filter that permits no type at
  // all can never succeed.
  if ((pvData != nullptr && pcbData == nullptr) || (dwFlags & RRF_RT_ANY) == 0) {
    return ERROR_INVALID_PARAMETER;
  }

  const DWORD caller_size = pcbData ? *pcbData : 0;

  HKEY key    = hkey;
  bool opened = false;
  if (lpSubKey != nullptr && lpSubKey[0] != L'\0') {
    LSTATUS status = ::RegOpenKeyExW(hkey, lpSubKey, 0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) {
      return status;
    }
    opened = true;
  }

  LSTATUS status    = ERROR_SUCCESS;
  DWORD type        = REG_NONE;
  DWORD stored_size = 0;

  // Ask for the type and size first. The filter has to be applied before any
  // data is handed back, and the expansion path needs its own buffer anyway.
  status = ::RegQueryValueExW(key, lpValue, nullptr, &type, nullptr, &stored_size);
  if (status == ERROR_SUCCESS && !TypeIsAllowed(type, dwFlags)) {
    status = ERROR_UNSUPPORTED_TYPE;
  }

  if (status == ERROR_SUCCESS) {
    const bool expanding = type == REG_EXPAND_SZ && (dwFlags & RRF_NOEXPAND) == 0;

    if (expanding) {
      // Read it privately, expand it, then measure. The expanded form can be
      // any length, so the caller's buffer cannot be used for the raw value.
      const size_t raw_chars = stored_size / sizeof(WCHAR) + 1;
      auto* raw              = static_cast<WCHAR*>(
          ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, raw_chars * sizeof(WCHAR)));
      if (raw == nullptr) {
        status = ERROR_NOT_ENOUGH_MEMORY;
      } else {
        DWORD size = stored_size;
        status =
            ::RegQueryValueExW(key, lpValue, nullptr, &type, reinterpret_cast<LPBYTE>(raw), &size);
        if (status == ERROR_SUCCESS) {
          // Returns the length including the terminator, or 0 on failure.
          DWORD expanded_chars = ::ExpandEnvironmentStringsW(raw, nullptr, 0);
          if (expanded_chars == 0) {
            status = ::GetLastError();
          } else {
            const DWORD expanded = expanded_chars * static_cast<DWORD>(sizeof(WCHAR));

            // The larger of the expanded form and what is stored, which is
            // what the real RegGetValueW reports. Measured rather than
            // reasoned out: for %SystemRoot%\explorer.exe it answers 52 bytes
            // -- the stored string with its terminator -- where the expanded
            // result is 48, because %SystemRoot% is longer than C:\Windows.
            // Reporting the expanded size alone was more precise and did not
            // match, which the unit test's comparison against advapi32 caught
            // the first time it ran.
            //
            // Taking the maximum rather than simply the stored size, because
            // one observation cannot distinguish "always reports the stored
            // size" from "reports whichever is bigger", and only the latter is
            // safe: %PATH% expands to far more than it occupies, and a caller
            // told the stored size would allocate too little. The maximum
            // agrees with the measurement and cannot under-report.
            const DWORD needed = expanded > stored_size ? expanded : stored_size;

            if (pvData == nullptr) {
              if (pcbData) {
                *pcbData = needed;
              }
            } else if (caller_size < needed) {
              if (pcbData) {
                *pcbData = needed;
              }
              status = ERROR_MORE_DATA;
            } else {
              ::ExpandEnvironmentStringsW(raw, static_cast<LPWSTR>(pvData),
                                          caller_size / sizeof(WCHAR));
              if (pcbData) {
                *pcbData = needed;
              }
            }
            // Expansion makes it an ordinary string, which is what the
            // caller is told it received.
            type = REG_SZ;
          }
        }
        ::HeapFree(::GetProcessHeap(), 0, raw);
      }
    } else {
      // The stored size may omit the terminator a string type is promised,
      // so the answer can be larger than what is on disk.
      const size_t wanted    = TerminatorChars(type);
      const DWORD terminator = static_cast<DWORD>(wanted * sizeof(WCHAR));

      // What a query has to quote, and deliberately the worst case: nothing
      // has read the data at this point, so whether it carries a terminator
      // of its own is not yet known, and the promise of a terminated string
      // has already been made. The real RegGetValueW quotes the same, which a
      // caller sees as a size query answering two bytes more than the value
      // it then reads.
      const DWORD needed = stored_size + terminator;

      if (pvData == nullptr) {
        if (pcbData) {
          *pcbData = needed;
        }
      } else if (caller_size < stored_size) {
        // Not even room for what is stored. Compared against stored_size and
        // not against needed, because a buffer that exactly fits an already
        // terminated value is enough and the real RegGetValueW accepts it.
        if (pcbData) {
          *pcbData = needed;
        }
        status = ERROR_MORE_DATA;
      } else {
        DWORD size = caller_size;
        status =
            ::RegQueryValueExW(key, lpValue, nullptr, &type, static_cast<LPBYTE>(pvData), &size);
        if (status == ERROR_SUCCESS && terminator != 0) {
          // Terminate whatever came back, whether or not the stored data
          // bothered to -- but add only what is missing, and count only what
          // was added. Appending unconditionally over-reports by a character
          // on the ordinary case of an already terminated value, and the
          // reported size is what a caller measures a string by, so that
          // lands as an off-by-one in the caller rather than here. The real
          // RegGetValueW reports what it produced.
          auto* text         = static_cast<WCHAR*>(pvData);
          const size_t chars = size / sizeof(WCHAR);
          size_t present     = 0;
          while (present < wanted && chars > present && text[chars - 1 - present] == L'\0') {
            ++present;
          }

          const size_t missing = wanted - present;
          if (missing != 0) {
            if (size + missing * sizeof(WCHAR) > caller_size) {
              // Room for the data but not for the terminator it lacks.
              if (pcbData) {
                *pcbData = needed;
              }
              status = ERROR_MORE_DATA;
            } else {
              for (size_t i = 0; i < missing; ++i) {
                text[chars + i] = L'\0';
              }
              size += static_cast<DWORD>(missing * sizeof(WCHAR));
            }
          }
        }
        if (status == ERROR_SUCCESS && pcbData) {
          *pcbData = size;
        }
      }
    }
  }

  if (status != ERROR_SUCCESS && (dwFlags & RRF_ZEROONFAILURE) != 0 && pvData != nullptr &&
      caller_size != 0) {
    ::SecureZeroMemory(pvData, caller_size);
  }

  if (status == ERROR_SUCCESS && pdwType != nullptr) {
    *pdwType = type;
  }

  if (opened) {
    ::RegCloseKey(key);
  }
  return status;
}

LSTATUS APIENTRY ww_RegGetValueW(HKEY hkey,
                                 LPCWSTR lpSubKey,
                                 LPCWSTR lpValue,
                                 DWORD dwFlags,
                                 LPDWORD pdwType,
                                 PVOID pvData,
                                 LPDWORD pcbData) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hAdvapi32, RegGetValueW)) {
    return system_fn(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
  }
  return winwrap_RegGetValueFallbackW(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
}

} // extern "C"
