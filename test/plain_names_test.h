/* Copyright 2026 Alex313031. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef WINWRAP_PLAIN_NAMES_TEST_H_
#define WINWRAP_PLAIN_NAMES_TEST_H_

/* What plain_names_test.c offers the unit test: two Win32 functions called
 * through a table of their addresses rather than through their import slots.
 * See that file for why it is C. Needs <windows.h> first. */

#ifdef __cplusplus
extern "C" {
#endif

ULONGLONG winwrap_test_table_tick_count(void);
DWORD     winwrap_test_table_processor_number(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WINWRAP_PLAIN_NAMES_TEST_H_ */
