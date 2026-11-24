// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GetDynamicTimeZoneInformation. kernel32, Vista.
//
// DYNAMIC_TIME_ZONE_INFORMATION is TIME_ZONE_INFORMATION with two fields on
// the end, and GetTimeZoneInformation fills the shared part on XP exactly as
// the new function does. The two additions are what need finding.
//
// TimeZoneKeyName is the name of the zone's subkey under
// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time Zones -- "Pacific
// Standard Time", in English whatever the display language. Vista records it
// when the zone is chosen. XP does not; it records only the display strings.
// So the key is found by looking: first for the subkey whose "Std" value is
// the standard name in force, which is how XP's own Date and Time applet
// identifies the current zone, and failing that for one whose "TZI" rule is
// the rule in force. The second is a weaker match, since several zones share
// a rule and the first of them wins, but a zone with the right offsets and
// transitions is a better answer than none. If neither finds anything the
// name is left empty, which Windows itself does for a zone set by hand.
//
// This is the field ICU wants, and the reason node imports the function:
// wintz.cpp maps the key name to an IANA zone, and that becomes the process's
// default time zone.
//
// DynamicDaylightTimeDisabled reports the "Automatically adjust clock for
// daylight saving changes" box being cleared. Vista stores that under a value
// of the same name; XP stores it as DisableAutoDaylightTimeSet in the same
// key, and that is what is read. XP has no dynamic, year-by-year rules to
// disable, so the box is the whole of what the field can mean there -- and it
// is what ICU uses the field for, to decide the user wants a fixed offset.

#include "dispatch.h"
#include "winwrap.h"

namespace {

  constexpr wchar_t kZonesKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones";
  constexpr wchar_t kCurrentKey[] = L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation";

  // The "TZI" value of a zone's key, as documented for TIME_ZONE_INFORMATION.
  struct RegTzi {
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    SYSTEMTIME StandardDate;
    SYSTEMTIME DaylightDate;
  };

  // KEY_WOW64_64KEY. HKLM\SOFTWARE is one of the trees a 32-bit process on
  // 64-bit Windows sees a private copy of, and the zone list that matters is
  // the system's own -- the one the Date and Time applet, a native program,
  // chose the current zone from. Without this a 32-bit winwrap on XP x64
  // would search whatever is under Wow6432Node instead. 32-bit XP has no
  // second view and ignores the flag; the SDK hides the name below 0x0502.
  constexpr REGSAM kNativeView = 0x0100;

  constexpr size_t kNameLength = 32; // StandardName's, and "Std"'s to match.

  // Whether two zone names are the same, where either may fill its 32
  // characters without a terminator.
  bool SameName(const wchar_t* a, const wchar_t* b) {
    for (size_t i = 0; i < kNameLength; ++i) {
      if (a[i] != b[i]) {
        return false;
      }
      if (a[i] == L'\0') {
        break;
      }
    }
    return true;
  }

  bool SameRule(const RegTzi& rule, const TIME_ZONE_INFORMATION& zone) {
    return rule.Bias == zone.Bias && rule.StandardBias == zone.StandardBias &&
           rule.DaylightBias == zone.DaylightBias &&
           memcmp(&rule.StandardDate, &zone.StandardDate, sizeof(SYSTEMTIME)) == 0 &&
           memcmp(&rule.DaylightDate, &zone.DaylightDate, sizeof(SYSTEMTIME)) == 0;
  }

  // Writes the registry key name of `zone` into `key_name`, which holds 128
  // characters and arrives zeroed. Leaves it empty if no key matches.
  void FindKeyName(const TIME_ZONE_INFORMATION& zone, wchar_t* key_name) {
    HKEY zones = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kZonesKey, 0, KEY_READ | kNativeView, &zones) !=
        ERROR_SUCCESS) {
      return;
    }

    wchar_t by_rule[128] = {};
    bool found           = false;

    for (DWORD index = 0; !found; ++index) {
      wchar_t name[128] = {};
      DWORD name_length = ARRAYSIZE(name);
      const LONG status = ::RegEnumKeyExW(zones, index, name, &name_length, nullptr, nullptr,
                                          nullptr, nullptr);
      if (status == ERROR_MORE_DATA) {
        continue; // A name too long to be a TimeZoneKeyName; not a candidate.
      }
      if (status != ERROR_SUCCESS) {
        // ERROR_NO_MORE_ITEMS, normally. Anything else ends the search just
        // the same: an error such as ERROR_KEY_DELETED is returned for every
        // index, and going on would mean asking four billion times.
        break;
      }

      HKEY entry = nullptr;
      if (::RegOpenKeyExW(zones, name, 0, KEY_QUERY_VALUE | kNativeView, &entry) !=
          ERROR_SUCCESS) {
        continue;
      }

      // One spare character, zeroed, so a value that is not terminated in the
      // registry still is here.
      wchar_t standard[kNameLength + 1] = {};
      DWORD size                        = kNameLength * sizeof(wchar_t);
      DWORD type                        = 0;
      if (::RegQueryValueExW(entry, L"Std", nullptr, &type, reinterpret_cast<LPBYTE>(standard),
                             &size) == ERROR_SUCCESS &&
          type == REG_SZ && standard[0] != L'\0' && SameName(standard, zone.StandardName)) {
        memcpy(key_name, name, sizeof(name));
        found = true;
      } else if (by_rule[0] == L'\0') {
        RegTzi rule = {};
        size        = sizeof(rule);
        if (::RegQueryValueExW(entry, L"TZI", nullptr, &type, reinterpret_cast<LPBYTE>(&rule),
                               &size) == ERROR_SUCCESS &&
            size == sizeof(rule) && SameRule(rule, zone)) {
          memcpy(by_rule, name, sizeof(name));
        }
      }
      ::RegCloseKey(entry);
    }
    ::RegCloseKey(zones);

    if (!found && by_rule[0] != L'\0') {
      memcpy(key_name, by_rule, sizeof(by_rule));
    }
  }

  bool AutomaticDaylightTimeDisabled() {
    HKEY current = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCurrentKey, 0, KEY_QUERY_VALUE, &current) !=
        ERROR_SUCCESS) {
      return false;
    }
    DWORD disabled = 0;
    DWORD size     = sizeof(disabled);
    DWORD type     = 0;
    const bool set = ::RegQueryValueExW(current, L"DisableAutoDaylightTimeSet", nullptr, &type,
                                        reinterpret_cast<LPBYTE>(&disabled), &size) ==
                         ERROR_SUCCESS &&
                     type == REG_DWORD && disabled != 0;
    ::RegCloseKey(current);
    return set;
  }

} // namespace

extern "C" DWORD WINAPI
ww_GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION pTimeZoneInformation) {
  // Hand straight over where Windows has this itself.
  if (const auto system_fn =
          WW_TRYFUNC(::winwrap::Module::hKernel32, GetDynamicTimeZoneInformation)) {
    return system_fn(pTimeZoneInformation);
  }
  if (!pTimeZoneInformation) {
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return TIME_ZONE_ID_INVALID;
  }

  TIME_ZONE_INFORMATION zone = {};
  const DWORD result         = ::GetTimeZoneInformation(&zone);
  if (result == TIME_ZONE_ID_INVALID) {
    return result;
  }

  // The new structure begins with the old one, field for field.
  static_assert(offsetof(DYNAMIC_TIME_ZONE_INFORMATION, TimeZoneKeyName) ==
                    sizeof(TIME_ZONE_INFORMATION),
                "DYNAMIC_TIME_ZONE_INFORMATION no longer extends TIME_ZONE_INFORMATION");
  memset(pTimeZoneInformation, 0, sizeof(*pTimeZoneInformation));
  memcpy(pTimeZoneInformation, &zone, sizeof(zone));

  FindKeyName(zone, pTimeZoneInformation->TimeZoneKeyName);
  pTimeZoneInformation->DynamicDaylightTimeDisabled = AutomaticDaylightTimeDisabled();
  return result;
}
