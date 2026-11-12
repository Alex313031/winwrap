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

#include <stdio.h>
// clang-format on

// inline, not an anonymous-namespace global: a second test file would
// otherwise get its own counter and main() would report success while another
// translation unit printed failures.
inline int g_failures = 0;

namespace {

  #define CHECK(cond, ...)                              \
    do {                                                \
      if (!(cond)) {                                    \
        ++g_failures;                                   \
        ::printf("    FAIL line %d: ", __LINE__);       \
        ::printf(__VA_ARGS__);                          \
        ::printf("\n");                                 \
      }                                                 \
    } while (0)
} // namespace

#endif // WINWRAP_WINWRAP_UNITTEST_H_
