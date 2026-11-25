/* Copyright 2026 Alex313031. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* A table of Win32 functions by address, the way SQLite's Windows layer keeps
 * one:
 *
 *     { "GetTickCount64", (SYSCALL)GetTickCount64, 0 },
 *
 * In C the address of a dllimport function in a static initializer cannot be
 * loaded from the import slot at run time, so the compiler references the
 * function by name -- _GetTickCount64@0 -- and leaves the linker to find a
 * body. That is a different symbol from the __imp_ slot every ordinary call
 * goes through, and winwrap has to answer both: if it defines only the slot,
 * the name is satisfied from kernel32's import library, whose member for it
 * defines the slot a second time, and the link fails. Node 24.22.3 did,
 * on the line above.
 *
 * This file is C and not C++ on purpose. C++ may initialise such a table
 * dynamically, through the slot, and then nothing here would be tested.
 *
 * If winwrap ever stops defining the names, this file is what stops linking.
 * The functions below are for the unit test to call, so that it also checks
 * the names lead to the same place the slots do.
 */

#include <windows.h>

#include "plain_names_test.h"

#if defined(_MSC_VER)
 /* C4232: "address of dllimport 'x' is not static, identity not guaranteed".
  * Which is the situation this file exists to create. */
 #pragma warning(disable : 4232)
#endif

typedef void (*Syscall)(void);

static const struct {
  const char* name;
  Syscall     function;
} kTable[] = {
  { "GetTickCount64",            (Syscall)GetTickCount64 },
  { "GetCurrentProcessorNumber", (Syscall)GetCurrentProcessorNumber },
};

ULONGLONG winwrap_test_table_tick_count(void) {
  return ((ULONGLONG(WINAPI*)(void))kTable[0].function)();
}

DWORD winwrap_test_table_processor_number(void) {
  return ((DWORD(WINAPI*)(void))kTable[1].function)();
}
