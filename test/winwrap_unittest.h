#ifndef WINWRAP_WINWRAP_UNITTEST_H_
#define WINWRAP_WINWRAP_UNITTEST_H_

#include <windows.h>

namespace {
  int g_failures = 0;

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
