// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Network interface identifiers: ConvertInterfaceIndexToLuid and
// ConvertInterfaceLuidToNameW. iphlpapi, Vista.
//
// Vista gave every network interface a NET_LUID, a 64-bit value that stays
// put while interface indexes are reassigned, and a name derived from it --
// "ethernet_32769", "loopback_0". These two functions are the path from an
// index to that name, which is all libuv's uv_if_indextoname() is.
//
// XP has indexes and nothing else, so there is no LUID to look up. One is
// made instead. A NET_LUID is two fields that matter: the interface type,
// which XP does know and GetIfEntry reports, and NetLuidIndex, a number
// unique among interfaces of that type. The index is unique among all
// interfaces, so it serves. The result is a LUID that is well formed,
// distinct per interface, and stable for as long as the index is -- which on
// XP is the only identity an interface has. YY-Thunks makes the same
// substitution.
//
// It follows that the names differ from what a later Windows would call the
// same adapter: "ethernet_2" here, "ethernet_32769" there. Nothing portable
// can depend on the number, since Windows itself assigns it per machine, but
// a LUID from here means something only to the other function in this file.
// INCOMPLETE_FUNCS.md has the entry.
//
// Like shell32.cc, and for the same reason, this loads its library rather
// than probing for it: consumers delay-import iphlpapi. See dispatch.h.

#include "dispatch.h"
#include "winwrap.h" // And through it <ifdef.h>, for NET_LUID.

namespace {

  // MIB_IFROW, as GetIfEntry fills it. Declared here rather than taken from
  // ifmib.h, which arrives by way of headers that want Winsock 2 in front of
  // them, while framework.h's <windows.h> has already brought in Winsock 1.
  constexpr size_t kMaxInterfaceNameLength  = 256; // MAX_INTERFACE_NAME_LEN
  constexpr size_t kMaxPhysicalAddress      = 8;   // MAXLEN_PHYSADDR
  constexpr size_t kMaxInterfaceDescription = 256; // MAXLEN_IFDESCR

  struct InterfaceRow {
    WCHAR wszName[kMaxInterfaceNameLength];
    DWORD dwIndex;
    DWORD dwType;
    DWORD dwMtu;
    DWORD dwSpeed;
    DWORD dwPhysAddrLen;
    UCHAR bPhysAddr[kMaxPhysicalAddress];
    DWORD dwAdminStatus;
    DWORD dwOperStatus;
    DWORD dwLastChange;
    DWORD dwInOctets;
    DWORD dwInUcastPkts;
    DWORD dwInNUcastPkts;
    DWORD dwInDiscards;
    DWORD dwInErrors;
    DWORD dwInUnknownProtos;
    DWORD dwOutOctets;
    DWORD dwOutUcastPkts;
    DWORD dwOutNUcastPkts;
    DWORD dwOutDiscards;
    DWORD dwOutErrors;
    DWORD dwOutQLen;
    DWORD dwDescrLen;
    UCHAR bDescr[kMaxInterfaceDescription];
  };
  static_assert(sizeof(InterfaceRow) == 860, "InterfaceRow does not match MIB_IFROW");

  using GetIfEntryFn = DWORD(WINAPI*)(InterfaceRow*);

  // The width of NET_LUID's NetLuidIndex field.
  constexpr unsigned kLuidIndexBits = 24;
  constexpr ULONG kLuidIndexMask    = (1ul << kLuidIndexBits) - 1;

  // The names Windows gives the interface types it has names for. Any other
  // type is "iftype" followed by its number.
  struct TypeName {
    DWORD type;
    const wchar_t* name;
  };
  const TypeName kTypeNames[] = {
      {1, L"other"},      // IF_TYPE_OTHER
      {6, L"ethernet"},   // IF_TYPE_ETHERNET_CSMACD
      {9, L"tokenring"},  // IF_TYPE_ISO88025_TOKENRING
      {23, L"ppp"},       // IF_TYPE_PPP
      {24, L"loopback"},  // IF_TYPE_SOFTWARE_LOOPBACK
      {37, L"atm"},       // IF_TYPE_ATM
      {71, L"wireless"},  // IF_TYPE_IEEE80211
      {131, L"tunnel"},   // IF_TYPE_TUNNEL
      {144, L"ieee1394"}, // IF_TYPE_IEEE1394
  };

  // Appends to a fixed buffer and remembers whether everything fitted.
  class NameBuilder {
   public:
    NameBuilder(wchar_t* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

    void Text(const wchar_t* text) {
      for (; *text != L'\0'; ++text) {
        Character(*text);
      }
    }

    void Number(ULONG64 value) {
      wchar_t digits[20]; // 2^64 has twenty.
      size_t count = 0;
      do {
        digits[count++] = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
      } while (value != 0);
      while (count != 0) {
        Character(digits[--count]);
      }
    }

    // Terminates the string. False if it, or anything before it, did not fit
    // -- in which case the buffer is left holding an empty string rather than
    // as much of the name as there was room for with nothing to end it. The
    // caller has been told the call failed, but one that reads the buffer
    // anyway should find a string there.
    bool Finish() {
      Character(L'\0');
      if (!fits_ && capacity_ != 0) {
        buffer_[0] = L'\0';
      }
      return fits_;
    }

   private:
    void Character(wchar_t character) {
      if (length_ < capacity_) {
        buffer_[length_++] = character;
      } else {
        fits_ = false;
      }
    }

    wchar_t* buffer_;
    size_t capacity_;
    size_t length_ = 0;
    bool fits_     = true;
  };

  std::atomic<void*> g_iphlpapi{nullptr};
  std::atomic<void*> g_index_to_luid{nullptr};
  std::atomic<void*> g_luid_to_name{nullptr};
  std::atomic<void*> g_get_if_entry{nullptr};

  HMODULE Iphlpapi() {
    return ::winwrap::LoadSystemLibrary(g_iphlpapi, L"iphlpapi.dll");
  }

} // namespace

extern "C" {

DWORD WINAPI ww_ConvertInterfaceIndexToLuid(ULONG InterfaceIndex, NET_LUID* InterfaceLuid) {
  const HMODULE iphlpapi = Iphlpapi();

  // Hand straight over where Windows has this itself.
  if (const auto system_fn = reinterpret_cast<decltype(&ww_ConvertInterfaceIndexToLuid)>(
          ::winwrap::ResolveFrom(g_index_to_luid, iphlpapi, "ConvertInterfaceIndexToLuid"))) {
    return system_fn(InterfaceIndex, InterfaceLuid);
  }
  if (!InterfaceLuid) {
    return ERROR_INVALID_PARAMETER;
  }
  // Zero on every failure, as documented.
  InterfaceLuid->Value = 0;

  const auto get_if_entry = reinterpret_cast<GetIfEntryFn>(
      ::winwrap::ResolveFrom(g_get_if_entry, iphlpapi, "GetIfEntry"));
  if (!get_if_entry) {
    return ERROR_PROC_NOT_FOUND;
  }

  // 860 bytes of which two fields are read. Static storage would spare the
  // stack and cost a lock; the stack is the cheaper of the two.
  InterfaceRow row = {};
  row.dwIndex      = InterfaceIndex;
  if (get_if_entry(&row) != NO_ERROR) {
    // Whatever GetIfEntry called it -- ERROR_INVALID_DATA, usually -- there is
    // no such interface, and this is how the real function says so.
    return ERROR_FILE_NOT_FOUND;
  }

  // See the top of the file. NetLuidIndex is 24 bits and an index is 32, so
  // the top byte goes in Reserved, which is 24 bits nobody else reads: a LUID
  // from here is only ever handed back to ww_ConvertInterfaceLuidToNameW,
  // which puts the two halves together again. Without that an index past
  // 0x00FFFFFF would lose its top byte and could share a name with another.
  InterfaceLuid->Info.Reserved     = InterfaceIndex >> kLuidIndexBits;
  InterfaceLuid->Info.NetLuidIndex = InterfaceIndex & kLuidIndexMask;
  InterfaceLuid->Info.IfType       = row.dwType;
  return NO_ERROR;
}

DWORD WINAPI ww_ConvertInterfaceLuidToNameW(const NET_LUID* InterfaceLuid,
                                            PWSTR InterfaceName,
                                            SIZE_T Length) {
  if (const auto system_fn = reinterpret_cast<decltype(&ww_ConvertInterfaceLuidToNameW)>(
          ::winwrap::ResolveFrom(g_luid_to_name, Iphlpapi(), "ConvertInterfaceLuidToNameW"))) {
    return system_fn(InterfaceLuid, InterfaceName, Length);
  }
  if (!InterfaceLuid || !InterfaceName) {
    return ERROR_INVALID_PARAMETER;
  }

  const DWORD type         = static_cast<DWORD>(InterfaceLuid->Info.IfType);
  const wchar_t* type_name = nullptr;
  for (const TypeName& entry : kTypeNames) {
    if (entry.type == type) {
      type_name = entry.name;
      break;
    }
  }

  NameBuilder name(InterfaceName, Length);
  if (type_name) {
    name.Text(type_name);
  } else {
    name.Text(L"iftype");
    name.Number(type);
  }
  name.Text(L"_");
  // The whole index, top byte included; see ww_ConvertInterfaceIndexToLuid.
  // Zero in Reserved, which is every LUID made from an index below 2^24 and
  // every LUID from anywhere else, leaves NetLuidIndex as it is.
  name.Number((static_cast<ULONG64>(InterfaceLuid->Info.Reserved) << kLuidIndexBits) |
              InterfaceLuid->Info.NetLuidIndex);
  return name.Finish() ? NO_ERROR : ERROR_NOT_ENOUGH_MEMORY;
}

} // extern "C"
