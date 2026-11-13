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

  using RegGetValueWFn = LSTATUS(APIENTRY*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

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

LSTATUS APIENTRY winwrap_RegGetValueW(HKEY hkey,
                                      LPCWSTR lpSubKey,
                                      LPCWSTR lpValue,
                                      DWORD dwFlags,
                                      LPDWORD pdwType,
                                      PVOID pvData,
                                      LPDWORD pcbData) {
  static const auto real = []() -> RegGetValueWFn {
    HMODULE advapi32 = ::GetModuleHandleW(L"advapi32.dll");
    return advapi32 ? reinterpret_cast<RegGetValueWFn>(::GetProcAddress(advapi32, "RegGetValueW"))
                    : nullptr;
  }();
  if (real) {
    return real(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
  }

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
            const DWORD needed = expanded_chars * static_cast<DWORD>(sizeof(WCHAR));
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
      const DWORD terminator = static_cast<DWORD>(TerminatorChars(type) * sizeof(WCHAR));
      const DWORD needed     = stored_size + terminator;

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
        DWORD size = caller_size;
        status =
            ::RegQueryValueExW(key, lpValue, nullptr, &type, static_cast<LPBYTE>(pvData), &size);
        if (status == ERROR_SUCCESS) {
          if (terminator != 0) {
            // Terminate whatever came back, whether or not the stored data
            // bothered to. This is the guarantee the caller is relying on.
            auto* text           = static_cast<WCHAR*>(pvData);
            const size_t written = size / sizeof(WCHAR);
            for (size_t i = 0; i < TerminatorChars(type); ++i) {
              text[written + i] = L'\0';
            }
            size += terminator;
          }
          if (pcbData) {
            *pcbData = size;
          }
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

} // extern "C"
