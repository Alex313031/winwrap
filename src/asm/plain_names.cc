// Copyright 2026 Alex313031. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Every redirected function under its own name -- `_GetTickCount64@0` beside
// the `__imp__GetTickCount64@0` that static_redirects.cc defines -- for the
// callers that reference a function by name and not through its import slot.
// static_redirects.cc explains who those are, why it matters, and why this is
// a separate object from the slots.
//
// The table is that file's. Including it with the macro below turns each
// WINWRAP_REDIRECT line into a jump to the shim and leaves the slots and the
// anchor out, so the list of functions exists once.
//
// This is the Clang and GCC form. MSVC has no file-scope assembly, and its
// builds assemble plain_names_<arch>.asm, beside this file, in place of it, as they
// assemble forwarders_<arch>.asm in place of static_redirects.cc.

#define WINWRAP_PLAIN_NAMES_ONLY
#include "../static_redirects.cc"
