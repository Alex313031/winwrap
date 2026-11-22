// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The locale family. kernel32, Vista, except ResolveLocaleName which is
// Windows 7.
//
// Almost all of these are the "Ex" half of a pair whose older form XP does
// have, and the difference is that the Ex form is addressed by locale *name*
// where the original takes an LCID. So the work is turning one into the other,
// and XP has no LocaleNameToLCID to do it with. LcidForName below is that
// conversion, and once a name has become an LCID the rest of each shim is the
// call XP already had:
//
//   GetLocaleInfoEx       GetDateFormatEx       GetNumberFormatEx
//   LCMapStringEx         GetTimeFormatEx       GetCurrencyFormatEx
//   CompareStringEx
//
// Three are not that shape. CompareStringOrdinal is new behaviour -- binary
// comparison, which CompareStringW cannot be asked for -- so it is written out
// here. LocaleNameToLCID and LCIDToLocaleName *are* the conversion, in each
// direction, so they are LcidForName and NameForLcid with a return convention
// around them. ResolveLocaleName is the two composed.
//
// Worth knowing why this file matters more than its size suggests: it has two
// sets of callers that arrive without being asked for. MSVC's own standard
// library reaches for CompareStringEx, LCMapStringEx and GetLocaleInfoEx, so
// any consumer that touches <string> or the locale facets imports them at load
// time whether it meant to or not. And ICU reaches for the other seven --
// locmap.cpp for the LCID mapping, windtfmt.cpp and winnmfmt.cpp for the host
// formatters -- which is how a Node build that never formats a date in a
// non-default locale still cannot start on XP without them.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  constexpr DWORD kSpinCount = 2000;

  PVOID g_lock_ptr = nullptr;

  CRITICAL_SECTION* EnsureLock() {
    if (PVOID existing = ::InterlockedCompareExchangePointer(&g_lock_ptr, nullptr, nullptr)) {
      return static_cast<CRITICAL_SECTION*>(existing);
    }
    auto* fresh = static_cast<CRITICAL_SECTION*>(
        ::HeapAlloc(::GetProcessHeap(), 0, sizeof(CRITICAL_SECTION)));
    if (!fresh || !::InitializeCriticalSectionAndSpinCount(fresh, kSpinCount)) {
      if (fresh) {
        ::HeapFree(::GetProcessHeap(), 0, fresh);
      }
      ::RaiseException(kStatusNoMemory, EXCEPTION_NONCONTINUABLE, 0, nullptr);
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

  using RtlUpcaseUnicodeCharFn = WCHAR(NTAPI*)(WCHAR);

  // Ordinal uppercasing, which is not linguistic uppercasing: it is a fixed
  // per-code-unit mapping with no locale in it.
  //
  // RtlUpcaseUnicodeChar is that mapping -- it is the table NT itself uses for
  // case-insensitive ordinal comparison, so matching it is the difference
  // between "close" and "the same answer". LCMapStringW with LOCALE_INVARIANT is
  // the NLS uppercase, which is near-identical but not the same function, and is
  // only the fallback for the impossible case of ntdll not resolving.
  //
  // ASCII is handled inline: it is the overwhelming majority of what gets
  // compared, the two mappings agree there, and it avoids a call per character.
  wchar_t UpcaseOrdinal(wchar_t c) {
    if (c < 0x80) {
      return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
    }

    const auto upcase =
        WW_TRYFUNC_AS(RtlUpcaseUnicodeCharFn, ::winwrap::Module::hNtDll, "RtlUpcaseUnicodeChar");
    if (upcase) {
      return upcase(c);
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
  LCID g_found_lcid            = 0;
  bool g_found                 = false;

  // One-entry memo. ICU asks for the same locale a dozen times in a row while
  // building one formatter, and each miss is an enumeration of every locale the
  // system has installed.
  wchar_t g_cached_name[LOCALE_NAME_MAX_LENGTH] = {};
  LCID g_cached_lcid                            = 0;
  bool g_has_cached                             = false;

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
  //
  // The alternative, which YY-Thunks takes, is a static table of every name and
  // LCID taken from the NLS reference. That resolves names this cannot --
  // anything whose Windows name is not language plus country, such as the
  // script-tagged zh-Hans or sr-Latn-RS -- at the cost of a few hundred lines of
  // data that has to be right and stay right. Asking the running system instead
  // cannot go stale and cannot disagree with it, and covers every locale the
  // machine actually has. Worth revisiting if a consumer needs the script-tagged
  // names, which neither Electron nor ICU's Windows formatters request.
  bool NameForLcid(LCID lcid, wchar_t* out, int out_chars) {
    wchar_t language[16] = {};
    if (::GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, language, ARRAYSIZE(language)) == 0) {
      return false;
    }

    wchar_t country[16] = {};
    bool have_country =
        ::GetLocaleInfoW(lcid, LOCALE_SISO3166CTRYNAME, country, ARRAYSIZE(country)) > 0 &&
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
      return TRUE; // Keep going.
    }
    if (!NamesEqual(name, g_wanted_name)) {
      return TRUE;
    }
    g_found_lcid = lcid;
    g_found      = true;
    return FALSE; // Stop.
  }

  // LOCALE_NAME_SYSTEM_DEFAULT, spelled out because the SDK hides it below 0x0600.
  constexpr wchar_t kSystemDefaultName[] = L"!x-sys-default-locale";

  bool LcidForName(LPCWSTR name, LCID* out) {
    if (name == nullptr) {
      *out = LOCALE_USER_DEFAULT; // LOCALE_NAME_USER_DEFAULT is a null pointer.
      return true;
    }
    if (name[0] == L'\0') {
      *out = LOCALE_INVARIANT; // LOCALE_NAME_INVARIANT is the empty string.
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
    g_found       = false;
    g_found_lcid  = 0;
    ::EnumSystemLocalesW(EnumLocaleProc, LCID_SUPPORTED);

    bool found = g_found;
    if (found) {
      *out = g_found_lcid;

      size_t i = 0;
      for (; name[i] && i < ARRAYSIZE(g_cached_name) - 1; ++i) {
        g_cached_name[i] = name[i];
      }
      g_cached_name[i] = L'\0';
      g_cached_lcid    = g_found_lcid;
      g_has_cached     = true;
    }
    g_wanted_name = nullptr;

    ::LeaveCriticalSection(lock);
    return found;
  }

  // The two-step buffer protocol LCIDToLocaleName and ResolveLocaleName share:
  // a zero count asks how much is needed, a short one fails, and the count
  // returned counts the terminating null. Written once because getting the
  // off-by-one different between the two would be a buffer overrun in whichever
  // one was wrong.
  int ReturnName(const wchar_t* name, LPWSTR out, int out_chars) {
    int needed = 1; // The terminator.
    for (const wchar_t* p = name; *p; ++p) {
      ++needed;
    }
    if (out_chars == 0) {
      return needed; // Measuring call; out may be null and is not touched.
    }
    if (out_chars < needed) {
      ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    for (int i = 0; i < needed; ++i) {
      out[i] = name[i];
    }
    return needed;
  }

  // Shared by the two entry points that take dwFlags. Both accept nothing but
  // LOCALE_ALLOW_NEUTRAL_NAMES, which is Windows 7 and which this answers
  // without doing anything special: NameForLcid builds the name out of the
  // language and the country, and omits the country when the locale has none,
  // so a neutral LCID already produces a neutral name. The flag's effect on a
  // real Windows is to permit that rather than to fail, which is the behaviour
  // here with or without it -- so it is accepted and then ignored, and that is
  // not a gap. Anything else is a flag this cannot honour.
  //
  // Accepting it matters: ICU's locmap.cpp passes it on every call, and
  // rejecting unknown flags without carving this one out would fail the only
  // caller in the tree.
  bool FlagsAreSupported(DWORD flags) {
    if ((flags & ~static_cast<DWORD>(LOCALE_ALLOW_NEUTRAL_NAMES)) != 0) {
      ::SetLastError(ERROR_INVALID_FLAGS);
      return false;
    }
    return true;
  }

  // An output buffer has to be there unless the call is only measuring.
  bool OutputIsUsable(LPWSTR out, int out_chars) {
    if (out_chars < 0 || (out_chars > 0 && out == nullptr)) {
      ::SetLastError(ERROR_INVALID_PARAMETER);
      return false;
    }
    return true;
  }

} // namespace

extern "C" {

int WINAPI ww_CompareStringOrdinal(LPCWCH lpString1,
                                   int cchCount1,
                                   LPCWCH lpString2,
                                   int cchCount2,
                                   BOOL bIgnoreCase) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CompareStringOrdinal)) {
    return system_fn(lpString1, cchCount1, lpString2, cchCount2, bIgnoreCase);
  }
  // The real function validates all three of these and returns 0. A count
  // below -1 is meaningless, and bIgnoreCase is specified as a boolean rather
  // than "anything non-zero" -- a caller passing 2 has a bug, and hiding it
  // would make winwrap more permissive than what it stands in for.
  if (!lpString1 || !lpString2 || cchCount1 < -1 || cchCount2 < -1 ||
      (bIgnoreCase != TRUE && bIgnoreCase != FALSE)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }

  // Ordinal means binary: compare code units, and let a prefix sort before the
  // string that extends it. Not CompareStringW with any flag -- that is always
  // linguistic, which is a different answer, and the reason this function was
  // added to Windows at all.
  size_t length1 = cchCount1 < 0 ? ::wcslen(lpString1) : static_cast<size_t>(cchCount1);
  size_t length2 = cchCount2 < 0 ? ::wcslen(lpString2) : static_cast<size_t>(cchCount2);
  size_t shared  = length1 < length2 ? length1 : length2;

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

int WINAPI ww_GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType, LPWSTR lpLCData, int cchData) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetLocaleInfoEx)) {
    return system_fn(lpLocaleName, LCType, lpLCData, cchData);
  }
  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  // Everything past the name is unchanged between the two, LOCALE_RETURN_NUMBER
  // included, so the rest is the original call.
  return ::GetLocaleInfoW(lcid, LCType, lpLCData, cchData);
}

int WINAPI ww_LCMapStringEx(LPCWSTR lpLocaleName,
                            DWORD dwMapFlags,
                            LPCWSTR lpSrcStr,
                            int cchSrc,
                            LPWSTR lpDestStr,
                            int cchDest,
                            LPNLSVERSIONINFO lpVersionInformation,
                            LPVOID lpReserved,
                            LPARAM sortHandle) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, LCMapStringEx)) {
    return system_fn(lpLocaleName, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest,
                     lpVersionInformation, lpReserved, sortHandle);
  }

  // The three parameters with nothing on XP to map onto, refused rather than
  // ignored. lpVersionInformation selects a specific sort version, sortHandle
  // names a sort handle -- which is what LCMAP_SORTHANDLE makes lpLocaleName
  // mean as well -- and neither exists before Vista. lpReserved is specified
  // as always null. A caller passing any of them asked for something this
  // cannot do, and quietly doing the unversioned mapping instead would answer
  // a question it did not ask.
  if (lpVersionInformation != nullptr || lpReserved != nullptr || sortHandle != 0) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }

  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  // Everything past the name is unchanged between the two -- the flags, the
  // counts and the -1-means-null-terminated convention all carry over -- so
  // the rest is the original call.
  return ::LCMapStringW(lcid, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest);
}

int WINAPI ww_CompareStringEx(LPCWSTR lpLocaleName,
                              DWORD dwCmpFlags,
                              LPCWCH lpString1,
                              int cchCount1,
                              LPCWCH lpString2,
                              int cchCount2,
                              LPNLSVERSIONINFO lpVersionInformation,
                              LPVOID lpReserved,
                              LPARAM lParam) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, CompareStringEx)) {
    return system_fn(lpLocaleName, dwCmpFlags, lpString1, cchCount1, lpString2, cchCount2,
                     lpVersionInformation, lpReserved, lParam);
  }

  // As in ww_LCMapStringEx above: the three parameters the Ex form added have
  // nothing on XP to map onto, so they are refused rather than ignored.
  if (lpVersionInformation != nullptr || lpReserved != nullptr || lParam != 0) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }

  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }

  // dwCmpFlags is passed through unfiltered on purpose. Most of it is shared
  // with CompareStringW, but three flags are not: LINGUISTIC_IGNORECASE and
  // LINGUISTIC_IGNOREDIACRITIC arrived with this function, and
  // SORT_DIGITSASNUMBERS is Windows 7. XP's CompareStringW rejects an unknown
  // flag with ERROR_INVALID_FLAGS, which is the right answer and better than
  // anything this could invent -- masking them off would silently return a
  // comparison the caller did not ask for, and comparisons feed sorting and
  // equality tests where a wrong answer propagates quietly.
  return ::CompareStringW(lcid, dwCmpFlags, lpString1, cchCount1, lpString2, cchCount2);
}

// The name-to-LCID direction, which is LcidForName with a return convention.
// kernel32, Vista.
LCID WINAPI ww_LocaleNameToLCID(LPCWSTR lpName, DWORD dwFlags) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, LocaleNameToLCID)) {
    return system_fn(lpName, dwFlags);
  }
  if (!FlagsAreSupported(dwFlags)) {
    return 0;
  }
  LCID lcid = 0;
  if (!LcidForName(lpName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return lcid;
}

// The LCID-to-name direction. kernel32, Vista.
int WINAPI ww_LCIDToLocaleName(LCID Locale, LPWSTR lpName, int cchName, DWORD dwFlags) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, LCIDToLocaleName)) {
    return system_fn(Locale, lpName, cchName, dwFlags);
  }
  if (!FlagsAreSupported(dwFlags) || !OutputIsUsable(lpName, cchName)) {
    return 0;
  }

  // LOCALE_INVARIANT's name is the empty string, which is the inverse of the
  // mapping LcidForName applies to LOCALE_NAME_INVARIANT. Answered here rather
  // than left to NameForLcid, which would ask GetLocaleInfoW for the invariant
  // locale's ISO 639 name and get something that is not what the pair agreed.
  if (Locale == LOCALE_INVARIANT) {
    return ReturnName(L"", lpName, cchName);
  }

  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (!NameForLcid(Locale, name, ARRAYSIZE(name))) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return ReturnName(name, lpName, cchName);
}

// Resolve a name to the closest name the system actually has. kernel32,
// Windows 7.
//
// Built from the two above: a name in, an LCID, and the system's own name for
// that LCID back out. That normalises what can be normalised -- case, and a
// name whose pieces are in the right order -- because the round trip through
// EnumSystemLocalesW ends at whatever the installed locale calls itself.
//
// What it does not do is the fallback half of "resolve". The real function
// walks up a tag: "en-XY" with no such locale installed resolves to "en", and
// a script-tagged or three-letter tag resolves to its nearest relative. This
// resolves a name the system has and fails on one it does not. That is a
// narrower function than the name suggests, and worth being plain about rather
// than returning a wrong neighbour -- a caller handed the wrong locale formats
// dates in it silently, where a failure is visible. ICU, the only caller in
// the tree, treats a zero as U_UNSUPPORTED_ERROR and carries on with its own
// data, which is the outcome a partial answer would have denied it.
int WINAPI ww_ResolveLocaleName(LPCWSTR lpNameToResolve, LPWSTR lpLocaleName, int cchLocaleName) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, ResolveLocaleName)) {
    return system_fn(lpNameToResolve, lpLocaleName, cchLocaleName);
  }
  if (!OutputIsUsable(lpLocaleName, cchLocaleName)) {
    return 0;
  }

  LCID lcid = 0;
  if (!LcidForName(lpNameToResolve, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (lcid == LOCALE_INVARIANT) {
    return ReturnName(L"", lpLocaleName, cchLocaleName);
  }

  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (!NameForLcid(lcid, name, ARRAYSIZE(name))) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return ReturnName(name, lpLocaleName, cchLocaleName);
}

// --- The four formatters ---------------------------------------------------
//
// All kernel32, all Vista, and all the same shape: resolve the name to an LCID
// and make the call XP already has. Every parameter past the name means the
// same thing to both forms, including the convention that a null output buffer
// with a zero count asks for the required length, so none of them is touched
// on the way through.

int WINAPI ww_GetDateFormatEx(LPCWSTR lpLocaleName,
                              DWORD dwFlags,
                              const SYSTEMTIME* lpDate,
                              LPCWSTR lpFormat,
                              LPWSTR lpDateStr,
                              int cchDate,
                              LPCWSTR lpCalendar) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetDateFormatEx)) {
    return system_fn(lpLocaleName, dwFlags, lpDate, lpFormat, lpDateStr, cchDate, lpCalendar);
  }

  // The one parameter the Ex form added, and the only one of these four with
  // anything to refuse. It names a calendar for this call alone, which XP's
  // GetDateFormatW has no parameter for -- a calendar there is a property of
  // the locale, set through LOCALE_ICALENDARTYPE, not something a formatting
  // call can override. Refused rather than ignored, as this file already does
  // for LCMapStringEx's sort handle: a caller that asked for the Hijri
  // calendar and silently got the Gregorian one has a wrong date, not a
  // slightly different one.
  if (lpCalendar != nullptr) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }

  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return ::GetDateFormatW(lcid, dwFlags, lpDate, lpFormat, lpDateStr, cchDate);
}

int WINAPI ww_GetTimeFormatEx(LPCWSTR lpLocaleName,
                              DWORD dwFlags,
                              const SYSTEMTIME* lpTime,
                              LPCWSTR lpFormat,
                              LPWSTR lpTimeStr,
                              int cchTime) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetTimeFormatEx)) {
    return system_fn(lpLocaleName, dwFlags, lpTime, lpFormat, lpTimeStr, cchTime);
  }
  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return ::GetTimeFormatW(lcid, dwFlags, lpTime, lpFormat, lpTimeStr, cchTime);
}

int WINAPI ww_GetNumberFormatEx(LPCWSTR lpLocaleName,
                                DWORD dwFlags,
                                LPCWSTR lpValue,
                                const NUMBERFMTW* lpFormat,
                                LPWSTR lpNumberStr,
                                int cchNumber) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetNumberFormatEx)) {
    return system_fn(lpLocaleName, dwFlags, lpValue, lpFormat, lpNumberStr, cchNumber);
  }
  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return ::GetNumberFormatW(lcid, dwFlags, lpValue, lpFormat, lpNumberStr, cchNumber);
}

int WINAPI ww_GetCurrencyFormatEx(LPCWSTR lpLocaleName,
                                  DWORD dwFlags,
                                  LPCWSTR lpValue,
                                  const CURRENCYFMTW* lpFormat,
                                  LPWSTR lpCurrencyStr,
                                  int cchCurrency) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn = WW_TRYFUNC(::winwrap::Module::hKernel32, GetCurrencyFormatEx)) {
    return system_fn(lpLocaleName, dwFlags, lpValue, lpFormat, lpCurrencyStr, cchCurrency);
  }
  LCID lcid = 0;
  if (!LcidForName(lpLocaleName, &lcid)) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return ::GetCurrencyFormatW(lcid, dwFlags, lpValue, lpFormat, lpCurrencyStr, cchCurrency);
}

} // extern "C"
