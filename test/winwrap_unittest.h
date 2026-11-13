#ifndef WINWRAP_WINWRAP_UNITTEST_H_
#define WINWRAP_WINWRAP_UNITTEST_H_

#ifndef UNICODE
 #define UNICODE
#endif

#ifndef _UNICODE
 #define _UNICODE
#endif

// clang-format off
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include <winwrap.h>
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
