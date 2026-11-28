#ifndef WINWRAP_WINWRAP_UNITTEST_H_
#define WINWRAP_WINWRAP_UNITTEST_H_

// clang-format off
#ifndef UNICODE
 #define UNICODE
#endif

#ifndef _UNICODE
 #define _UNICODE
#endif

// WIN32_NO_STATUS around <windows.h>, as framework.h does it. winwrap.h below
// reaches framework.h, which includes <ntstatus.h> for the full STATUS_* set;
// if winnt.h has already defined its own subset of those, ntstatus.h redefines
// every one -- twenty C4005 warnings per build. framework.h cannot prevent
// that from where it sits, because by then <windows.h> is behind its include
// guard. So the first include of it, which is this one, has to ask.
//
// <windows.h> still comes first rather than leaving it to winwrap.h: this
// file builds against the current SDK, and framework.h would otherwise set
// _WIN32_WINNT to XP before the SDK headers were read.
#ifdef _MSC_VER
 #define WIN32_NO_STATUS
#endif
#include <windows.h> // Main Windows header, comes first
#ifdef _MSC_VER
 #undef WIN32_NO_STATUS
#endif

// _getch, for holding the window open when this was double-clicked. A CRT
// function rather than ReadConsoleInput so there is no console mode to save
// and restore, and no risk of leaving the console in a different state than it
// was found in.
#include <conio.h>
// SymGetSearchPathW and SymSetSearchPathW. Only the declarations are wanted:
// the rest of dbghelp is reached through GetProcAddress in
// TestSymbolSearchPath, so no build needs dbghelp.lib for this.
// TASKDIALOGCONFIG and the TDN_ and TDM_ constants. Declarations only: the
// function resolves to winwrap, and no build links comctl32 for it.
#include <commctrl.h>
#include <dbghelp.h>
// EventWriteTransfer and EVENT_DESCRIPTOR, likewise not in windows.h. Gated on
// WINVER >= 0x0600, which this file is built at -- it deliberately does not
// take winwrap's own lowered target, since it has to be able to call the Win32
// names to test them.
#include <evntprov.h>
// NET_LUID, for the iphlpapi pair.
#include <ifdef.h>
// The AppUserModelID pair lives in shlobj.h, and CoTaskMemFree in objbase.h;
// neither arrives with windows.h.
#include <objbase.h>
// ConvertStringSecurityDescriptorToSecurityDescriptorW, for the wait-only
// event in TestSyncObjects. advapi32, which the registry tests already link.
#include <sddl.h>
// Shell objects
#include <shlobj.h>
// clang-format on

// C Std Libs
#include <cstdint>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
// memcmp, for comparing the two RegGetValueW results byte for byte.
#include <string.h>

// C++ Std Libs
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// clang-format off
// winwrap's headers, comes last.
#include <winwrap.h>
#include <winwrap_public.h>
// clang-format on

// A view, not a std::wstring. libc++ uses the allocated representation during
// constant evaluation -- the short-string buffer is a union, which constexpr
// cannot read -- so a constexpr std::wstring is rejected however short it is,
// and a non-constexpr one at namespace scope needs an exit-time destructor.
inline constexpr std::wstring_view kWinWrapVer = WINWRAP_VERSION_STRING;

// inline, not an anonymous-namespace global: a second test file would
// otherwise get its own counter and main() would report success while another
// translation unit printed failures.
inline int g_failures = 0;

// What %#x used to do. Two reasons it is a function returning a string rather
// than std::hex inline: a manipulator stays in effect, so one hex value would
// silently turn every later number on the stream into hex, and the cast keeps
// a negative HRESULT from being sign-extended -- 0x80070057 streamed as a
// long long would otherwise read 0xffffffff80070057.
template <typename T>
inline std::wstring Hex(T value) {
  static_assert(std::is_integral_v<T>, "Hex() formats integers");
  std::wostringstream stream;
  stream << L"0x" << std::hex << static_cast<std::make_unsigned_t<T>>(value);
  return stream.str();
}

// A string with everything outside printable ASCII spelled as \uXXXX, for
// anything that gets streamed to the console.
//
// std::wcout converts through the console's code page, which cannot represent
// most of what the locale tests deal in. A GitHub Actions log reported the
// uppercased form of "stra\u00dfe" as "STRA?x.E" -- unreadable, and worse,
// indistinguishable from a real defect, since the one thing a reader needs to
// know is which character came back. Escaping is also the only form that
// survives being pasted somewhere else.
inline std::wstring Printable(const wchar_t* text) {
  std::wostringstream stream;
  for (const wchar_t* p = text; p && *p; ++p) {
    if (*p >= 0x20 && *p < 0x7F) {
      stream << *p;
    } else {
      stream << L"\\u" << std::hex << std::setfill(L'0') << std::setw(4)
             << static_cast<unsigned int>(*p) << std::dec << std::setfill(L' ');
    }
  }
  return stream.str();
}

namespace {

// The message is a stream expression, not a format string:
//   CHECK(ok, L"expected " << expected << L", got " << actual);
//
// Parenthesise anything compound in it. The expression is spliced into a
// chain of <<, which binds tighter than comparison, ternary and logical
// operators, so `a == b` would bind as `(stream << a) == b`.
#define CHECK(cond, message)                                                        \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      ++g_failures;                                                                 \
      std::wcout << L"    FAIL line " << __LINE__ << L": " << message << std::endl; \
    }                                                                               \
  } while (0)
} // namespace

#endif // WINWRAP_WINWRAP_UNITTEST_H_
