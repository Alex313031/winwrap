// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CompareStringOrdinal and GetLocaleInfoEx. kernel32, Vista.
//
// Both are the "Ex" half of a pair whose older form XP does have, but they are
// not the same shape underneath. CompareStringOrdinal is new behaviour --
// binary comparison, which CompareStringW cannot be asked for -- so it is
// written out here. GetLocaleInfoEx is the old call addressed by locale *name*
// rather than LCID, so the work is turning one into the other, and XP has no
// LocaleNameToLCID to do it with.

#include <stdlib.h>  // wcstoul
#include <string.h>  // wcslen

#include "winwrap.h"

namespace {

constexpr DWORD kSpinCount = 2000;

PVOID g_lock_ptr = nullptr;

CRITICAL_SECTION* EnsureLock() {
  if (PVOID existing =
          ::InterlockedCompareExchangePointer(&g_lock_ptr, nullptr, nullptr)) {
    return static_cast<CRITICAL_SECTION*>(existing);
  }
  auto* fresh = static_cast<CRITICAL_SECTION*>(
      ::HeapAlloc(::GetProcessHeap(), 0, sizeof(CRITICAL_SECTION)));
  if (!fresh || !::InitializeCriticalSectionAndSpinCount(fresh, kSpinCount)) {
    if (fresh) {
      ::HeapFree(::GetProcessHeap(), 0, fresh);
    }
    ::RaiseException(STATUS_NO_MEMORY, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return nullptr;
  }
  PVOID won = ::InterlockedCompareExchangePointer(&g_lock_ptr, fresh, nullptr);
  if (won == nullptr) {
    return fresh;
  }
  ::DeleteCriticalSection(fresh);
  ::HeapFree(::GetProcessHeap(), 0, fresh);
  return static_cast<CRITICAL_SECTION*>(won);
}

// Ordinal uppercasing, which is not the same as linguistic uppercasing: it is
// a fixed per-code-unit mapping with no locale in it. ASCII is inline because
// it is the overwhelming majority of what gets compared -- environment
// variable names, in the only caller here -- and LOCALE_INVARIANT keeps the
// rest off the user's locale.
wchar_t UpcaseOrdinal(wchar_t c) {
  if (c < 0x80) {
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
  }
  wchar_t upper = c;
  if (::LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, &c, 1, &upper, 1) != 1) {
    return c;
  }
  return upper;
}

// EnumSystemLocalesW's callback takes no context -- EnumSystemLocalesEx, the
// one that does, arrived with the function being replaced here -- so the
// search has to communicate through these, under the lock.
const wchar_t* g_wanted_name = nullptr;
LCID g_found_lcid = 0;
bool g_found = false;

// One-entry memo. ICU asks for the same locale a dozen times in a row while
// building one formatter, and each miss is an enumeration of every locale the
// system has installed.
wchar_t g_cached_name[LOCALE_NAME_MAX_LENGTH] = {};
LCID g_cached_lcid = 0;
bool g_has_cached = false;

bool NamesEqual(const wchar_t* a, const wchar_t* b) {
  for (;; ++a, ++b) {
    if (UpcaseOrdinal(*a) != UpcaseOrdinal(*b)) {
      return false;
    }
    if (!*a) {
      return true;
    }
  }
}

// Rebuilds a locale's BCP 47-ish name from the pieces XP does expose, which is
// how a name is matched without LocaleNameToLCID: "en" plus "US" is "en-US".
// Locales XP cannot express -- anything needing a script subtag, say sr-Latn-RS
// -- do not exist on XP to be matched in the first place.
bool NameForLcid(LCID lcid, wchar_t* out, int out_chars) {
  wchar_t language[16] = {};
  if (::GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, language,
                       ARRAYSIZE(language)) == 0) {
    return false;
  }

  wchar_t country[16] = {};
  bool have_country = ::GetLocaleInfoW(lcid, LOCALE_SISO3166CTRYNAME, country,
                                       ARRAYSIZE(country)) > 0 &&
                      country[0] != L'\0';

  int written = 0;
  for (const wchar_t* p = language; *p && written < out_chars - 1; ++p) {
    out[written++] = *p;
  }
  if (have_country && written < out_chars - 1) {
    out[written++] = L'-';
    for (const wchar_t* p = country; *p && written < out_chars - 1; ++p) {
      out[written++] = *p;
    }
  }
  out[written] = L'\0';
  return true;
}

BOOL CALLBACK EnumLocaleProc(LPWSTR lpLocaleString) {
  // The callback is handed the LCID as an eight-digit hex string.
  LCID lcid = static_cast<LCID>(::wcstoul(lpLocaleString, nullptr, 16));

  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (!NameForLcid(lcid, name, ARRAYSIZE(name))) {
    return TRUE;  // Keep going.
  }
  if (!NamesEqual(name, g_wanted_name)) {
    return TRUE;
  }
  g_found_lcid = lcid;
  g_found = true;
  return FALSE;  // Stop.
}

// LOCALE_NAME_SYSTEM_DEFAULT, spelled out because the SDK hides it below 0x0600.
constexpr wchar_t kSystemDefaultName[] = L"!x-sys-default-locale";

bool LcidForName(LPCWSTR name, LCID* out) {
  if (name == nullptr) {
    *out = LOCALE_USER_DEFAULT;  // LOCALE_NAME_USER_DEFAULT is a null pointer.
    return true;
  }
  if (name[0] == L'\0') {
    *out = LOCALE_INVARIANT;  // LOCALE_NAME_INVARIANT is the empty string.
    return true;
  }
  if (NamesEqual(name, kSystemDefaultName)) {
    *out = LOCALE_SYSTEM_DEFAULT;
    return true;
  }

  CRITICAL_SECTION* lock = EnsureLock();
  ::EnterCriticalSection(lock);

  if (g_has_cached && NamesEqual(g_cached_name, name)) {
    *out = g_cached_lcid;
    ::LeaveCriticalSection(lock);
    return true;
  }

  g_wanted_name = name;
  g_found = false;
  g_found_lcid = 0;
  ::EnumSystemLocalesW(EnumLocaleProc, LCID_SUPPORTED);

  bool found = g_found;
  if (found) {
    *out = g_found_lcid;

    size_t i = 0;
    for (; name[i] && i < ARRAYSIZE(g_cached_name) - 1; ++i) {
      g_cached_name[i] = name[i];
    }
    g_cached_name[i] = L'\0';
    g_cached_lcid = g_found_lcid;
    g_has_cached = true;
  }
  g_wanted_name = nullptr;

  ::LeaveCriticalSection(lock);
  return found;
}

}  // namespace

extern "C" {

int WINAPI winwrap_CompareStringOrdinal(LPCWCH lpString1,
                                        int cchCount1,
                                        LPCWCH lpString2,
                                        int cchCount2,
                                        BOOL bIgnoreCase) {
  if (!lpString1 || !lpString2) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }

  // Ordinal means binary: compare code units, and let a prefix sort before the
  // string that extends it. Not CompareStringW with any flag -- that is always
  // linguistic, which is a different answer, and the reason this function was
  // added to Windows at all.
  size_t length1 = cchCount1 < 0 ? ::wcslen(lpString1)
                                 : static_cast<size_t>(cchCount1);
  size_t length2 = cchCount2 < 0 ? ::wcslen(lpString2)
                                 : static_cast<size_t>(cchCount2);
  size_t shared = length1 < length2 ? length1 : length2;

  for (size_t i = 0; i < shared; ++i) {
    wchar_t c1 = lpString1[i];
    wchar_t c2 = lpString2[i];
    if (c1 != c2 && bIgnoreCase) {
      c1 = UpcaseOrdinal(c1);
      c2 = UpcaseOrdinal(c2);
    }
    if (c1 != c2) {
      return c1 < c2 ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
  }

  if (length1 == length2) {
    return CSTR_EQUAL;
  }
  return length1 < length2 ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

int WINAPI winwrap_GetLocaleInfoEx(LPCWSTR lpLocaleName,
                                   LCTYPE LCType,
                                   LPWSTR lpLCData,
                                   int cchData) {
  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  // Everything past the name is unchanged between the two, LOCALE_RETURN_NUMBER
  // included, so the rest is the original call.
  return ::GetLocaleInfoW(lcid, LCType, lpLCData, cchData);
}

}  // extern "C"
