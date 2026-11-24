# Copyright 2026 Alex313031. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# NMAKE makefile for winwrap. For MSVC, deliberately -- not MinGW, which has
# GNUmakefile beside this one. GNU make prefers that name and NMAKE does not
# know it, so each tool reads its own.
#
#  nmake                   print usage and build nothing
#  nmake all               both flavours, and the tests for each
#  nmake static            only out\<arch>\static\winwrap.lib
#  nmake dll               only out\<arch>\dll\winwrap.dll
#  nmake test              only the unit tests for this flavour
#  nmake DEBUG=1 all       unoptimised, /MTd, with full debug info
#  nmake check-forwarders  verify the generated .asm is current
#  nmake clean             remove build artifacts
#
# Run it from a Visual Studio command prompt, so cl, link, lib, ml and rc are
# on PATH and the architecture is already chosen. "x86 Native Tools Command
# Prompt" gives a 32-bit winwrap, "x64 Native Tools" a 64-bit one. There is no
# cross-build switch here because the prompt already is one.
#
# --- Why this exists alongside BUILD.gn and winwrap.gyp ---------------------
#
# The GN and GYP builds serve Chromium-derived trees, and both want clang-cl,
# because src/static_redirects.cc names its symbols with Clang asm labels.
# This build serves the other consumer: somebody with a current Visual Studio
# who wants to target XP or Vista, with no Chromium in sight and no reason to
# install a second compiler.
#
# So it assembles src/forwarders_$(ARCH).asm instead -- the same table of
# __imp_<name> slots, written in MASM, which is the one assembler that can
# name a symbol like __imp__GetTickCount64@0. That is not a legal C
# identifier, which is why the C++ version needs an asm label and MSVC cannot
# compile it at all.
#
# static_redirects.cc is therefore absent from SOURCES below. A build takes one
# or the other and never both: two definitions of a slot is a duplicate symbol.
# tools/gen_forwarders.py generates the .asm from that same file, so there is
# one table to maintain rather than three.
#
# --- Consuming the result --------------------------------------------------
#
# Link winwrap.lib and force the redirect object in, which the static form
# needs because nothing in a consumer references winwrap by name:
#
#   x86   link ... winwrap.lib /INCLUDE:_winwrap_static_redirects
#   x64   link ... winwrap.lib /INCLUDE:winwrap_static_redirects
#
# Then set the subsystem floor, or the loader rejects the binary whatever its
# imports turn out to be:
#
#   x86   /SUBSYSTEM:WINDOWS,5.01     (or CONSOLE,5.01)
#   x64   /SUBSYSTEM:WINDOWS,5.02     as low as the linker goes for x64
#
# And link advapi32.lib, which winwrap itself needs -- see LIBS below. Most
# Windows programs already do, so this is easy to miss until something does
# not.
#
# Nothing else is needed. The include directory is src, and USING_WINWRAP is
# only a marker a consumer may test for -- the redirects need no header.

# The architecture define is not optional. Both forwarders_*.asm files guard
# their whole body on it, so without the right one they assemble to an empty
# object and every __imp_ slot silently goes missing -- the build succeeds and
# winwrap does nothing. See tools/gen_forwarders.py for why the files are
# guarded rather than selected.
#
# /safeseh on x86 only, and required rather than tidy: link.exe defaults to
# /SAFESEH:YES for x86, and a MASM object without the metadata fails the link
# with LNK2026, "module unsafe for SAFESEH image". x64 has no such table and
# ml64 does not take the flag.
!IF "$(VSCMD_ARG_TGT_ARCH)" == "x64" || "$(PLATFORM)" == "x64"
ARCH      = x64
ASM       = ml64
ASFLAGS   = /nologo /c /DWINWRAP_X64=1
ANCHOR    = winwrap_static_redirects
SUBSYS    = /SUBSYSTEM:WINDOWS,5.02
SUBSYSCON = /SUBSYSTEM:CONSOLE,5.02
!ELSE
ARCH      = x86
ASM       = ml
ASFLAGS   = /nologo /c /coff /safeseh /DWINWRAP_X86=1
ANCHOR    = _winwrap_static_redirects
SUBSYS    = /SUBSYSTEM:WINDOWS,5.01
SUBSYSCON = /SUBSYSTEM:CONSOLE,5.01
!ENDIF

# --- The three macros, which form they select, and where it goes -----------
#
# WINWRAP_DLL and WINWRAP_STATIC say which form is being built. Only the first
# is read, and only as version-resource metadata: winwrap_version.h picks
# ORIG_FILENAME from it, and winwrap.rc picks FILETYPE, so a DLL says
# winwrap.dll and VFT_DLL where a static library says winwrap.lib and
# VFT_STATIC_LIB. WINWRAP_STATIC is read nowhere and is documentary -- it is
# set anyway, because src/BUILD.gn sets it on its static_library target and these
# builds should not disagree about something a reader will compare.
#
# It has to reach the resource compiler, not just cl, which is the one place
# it does anything. Hence FORMDEF on both.
#
# USING_WINWRAP is a marker for consumers and is read nowhere inside winwrap.
# It is defined here as well because GN applies winwrap_public_config to the
# target itself and not only to its dependents, and winwrap.gyp does the same
# through 'defines' plus 'direct_dependent_settings'.
#
# Each form gets its own output directory, out\$(ARCH)\static and
# out\$(ARCH)\dll, because otherwise they collide on the name winwrap.lib --
# and collide destructively. The static form's winwrap.lib is the archive
# itself; the shared form's is the import library /IMPLIB writes beside the
# DLL. Those are entirely different things with the same name, so building one
# after the other would leave a consumer linking an import library that needs
# winwrap.dll at run time while believing it had the self-contained archive.
# Nothing would report it.
#
# The objects go under the same split, which also means the two forms cannot
# reuse each other's. Nothing in a .cc reads either macro today, so they would
# in fact be identical -- but that stops being true the moment one of these
# macros grows a second use.
!IFDEF SHARED
FORM    = dll
FORMDEF = /DWINWRAP_DLL
!ELSE
FORM    = static
FORMDEF = /DWINWRAP_STATIC
!ENDIF

OUTDIR = out\$(ARCH)\$(FORM)
OBJDIR = $(OUTDIR)\obj

# The only import library winwrap needs, and worth stating as a fact about the
# library rather than a detail of this build: its static-link surface is
# kernel32 plus advapi32 and nothing else. user32, shell32 and ntdll are all
# reached through GetProcAddress, so they produce no import at all.
#
# kernel32.lib is absent here because the CRT's own /DEFAULTLIB directive
# brings it in. advapi32 has no such directive, so it has to be named --
# registry.cc calls RegOpenKeyExW, RegQueryValueExW and RegCloseKey directly.
#
# The same applies to a consumer of the *static* library, which has no link
# step of its own here: it needs advapi32.lib on its own command line. Most
# Windows programs already link it, which is why this is easy to miss.
LIBS = advapi32.lib

# winwrap has to run on the oldest Windows it supports, so it compiles against
# that SDK surface rather than the current one. A guardrail, not a target
# setting: it stops winwrap reaching for a declaration the OS it stands in for
# does not have, which is the mistake it exists to repair. A consumer keeps
# building against whatever SDK it likes.
#
# NTDDI_VERSION as well as _WIN32_WINNT, because plenty of SDK headers gate on
# that one instead -- OVERLAPPED_ENTRY stayed visible at _WIN32_WINNT=0x0501
# until NTDDI came down too. And WINVER, which some older headers prefer.
TARGETDEFS = /D_WIN32_WINNT=0x0501 /DNTDDI_VERSION=0x05010300 /DWINVER=0x0501

# /EHs-c- and /GR- because winwrap needs neither exceptions nor RTTI, and both
# would pull CRT machinery into a library whose whole purpose is to have no
# surprising dependencies. /Zc:__cplusplus so __cplusplus reports the standard
# in force rather than 199711L, which is what MSVC says without it.
CXXFLAGS = /nologo /c /W4 /std:c++17 /Zc:__cplusplus /GR- /EHs-c- \
	$(FORMDEF) /DUSING_WINWRAP /DUNICODE /D_UNICODE \
	$(TARGETDEFS) /I src /Fo$(OBJDIR)\ /Fd$(OBJDIR)\winwrap.pdb

# Tested by value, not with !IFDEF, so the recursive invocations below can
# forward it unconditionally. `DEBUG=` on a command line *defines* the macro as
# empty, which !IFDEF reads as true -- so forwarding an unset DEBUG would turn
# every release build into a debug one.
DEBUG = 0

# Split out so the test below compiles with the same optimisation level and,
# more to the point, the same CRT. A /MT library linked into a /MTd executable
# is two CRTs in one image, which the linker reports as a pile of duplicate
# symbols if you are lucky.
!IF "$(DEBUG)" != "0"
OPTFLAGS = /Od /Zi /D_DEBUG
CRTFLAG  = /MTd
!ELSE
OPTFLAGS = /O2 /Oi /Zi /DNDEBUG
CRTFLAG  = /MT
!ENDIF

CXXFLAGS = $(CXXFLAGS) $(OPTFLAGS) $(CRTFLAG)

# Every source except static_redirects.cc, whose job forwarders_$(ARCH).asm
# does here, and winwrap_dll.cc, which holds what only a DLL wants and would
# be dead weight in the static library.
OBJECTS = \
	$(OBJDIR)\cancel_io.obj \
	$(OBJDIR)\condition_variable.obj \
	$(OBJDIR)\critical_section.obj \
	$(OBJDIR)\directories.obj \
	$(OBJDIR)\dispatch.obj \
	$(OBJDIR)\etw.obj \
	$(OBJDIR)\extra.obj \
	$(OBJDIR)\fibers.obj \
	$(OBJDIR)\files.obj \
	$(OBJDIR)\fls.obj \
	$(OBJDIR)\init_once.obj \
	$(OBJDIR)\io_completion.obj \
	$(OBJDIR)\locale.obj \
	$(OBJDIR)\lock_family.obj \
	$(OBJDIR)\misc.obj \
	$(OBJDIR)\net.obj \
	$(OBJDIR)\parking_lot.obj \
	$(OBJDIR)\pipes.obj \
	$(OBJDIR)\process.obj \
	$(OBJDIR)\registry.obj \
	$(OBJDIR)\shell32.obj \
	$(OBJDIR)\srwlock.obj \
	$(OBJDIR)\sync_objects.obj \
	$(OBJDIR)\system.obj \
	$(OBJDIR)\thread_pool.obj \
	$(OBJDIR)\threads.obj \
	$(OBJDIR)\tick_count.obj \
	$(OBJDIR)\timezone.obj \
	$(OBJDIR)\forwarders.obj

HEADERS = \
	src\dispatch.h \
	src\framework.h \
	src\lock_family.h \
	src\parking_lot.h \
	src\public\winwrap_public.h \
	src\winwrap.h \
	src\winwrap_version.h

STATICLIB = $(OUTDIR)\winwrap.lib
SHAREDLIB = $(OUTDIR)\winwrap.dll
TESTEXE   = $(OUTDIR)\winwrap_test.exe

# The library the test links against. Whichever form was built, it is the
# winwrap.lib in this form's directory -- the archive itself for the static
# form, the import library beside the DLL for the shared one -- so the test
# exercises what that form actually produces.
WINWRAPLIB = $(OUTDIR)\winwrap.lib

# What has to be built before the test can link, which is not the same thing.
# In the static form it is the archive. In the shared form it is the DLL,
# because the import library the test links against is a by-product of that
# link rather than a target in its own right -- so naming $(WINWRAPLIB) as the
# dependency asks nmake to produce the import library directly, and the only
# rule matching that path is the one that builds a static archive from
# $(OBJECTS). It would take it: `nmake SHARED=1 test` with the import library
# missing or older than the objects ran lib over it, leaving an archive where
# the import library had been. Nothing reported that, and because
# FORCEREDIRECTS is empty for SHARED the test then linked the archive with no
# /INCLUDE: anchor, never extracted forwarders.obj, and exercised the OS while
# reporting on winwrap. `nmake all` hid it by running dll-build first.
!IFDEF SHARED
LIBTARGET = $(SHAREDLIB)
!ELSE
LIBTARGET = $(STATICLIB)
!ENDIF

# Forcing the redirect object in is a static-form concern only. In the shared
# form the import library generated from winwrap.def already provides every
# __imp_<name>, so there is nothing to force -- and the anchor is deliberately
# not exported, so naming it there would fail with LNK2001.
!IFDEF SHARED
FORCEREDIRECTS =
!ELSE
FORCEREDIRECTS = /INCLUDE:$(ANCHOR)
!ENDIF

# The test compiles against the CURRENT SDK, not winwrap's lowered one: it
# calls the Win32 names in order to test them, and includes evntprov.h for
# EventWriteTransfer, which is gated on WINVER >= 0x0600. So TARGETDEFS is
# deliberately absent here. /EHsc because the harness streams to std::wcout
# and formats through std::wostringstream, which the library's /EHs-c- would
# not support.
# $(FORMDEF) is here as well as in CXXFLAGS, so the test knows which form it
# was linked against: WINWRAP_DLL for SHARED and WINWRAP_STATIC otherwise.
# Without it a `nmake SHARED=1 test` compiles a test that cannot tell, and the
# DllGetVersion coverage -- which only exists in the shared form -- would be
# skipped while reporting success. test/BUILD.gn does the same through
# is_component_winwrap.
#
# WINWRAP_TEST marks the translation unit as the test, as test/BUILD.gn,
# winwrap.gyp and GNUmakefile all do. Nothing reads it yet.
TESTFLAGS = /nologo /c /W4 /std:c++17 /Zc:__cplusplus /EHsc $(FORMDEF) \
	/DWINWRAP_TEST /DUSING_WINWRAP /DUNICODE /D_UNICODE \
	/I src /I src\public /I test \
	$(OPTFLAGS) $(CRTFLAG) \
	/Fo$(OBJDIR)\ /Fd$(OBJDIR)\winwrap_test.pdb

# advapi32 for the registry test, user32 for wsprintfW, ole32 for
# CoTaskMemFree in the AppUserModelID test, shell32 for the SHGetFolderPathW
# that the known-folder test compares against. A Chromium-derived tree links
# all four by default, which is why test/BUILD.gn names none of them.
TESTLIBS = advapi32.lib user32.lib ole32.lib shell32.lib

# FIRST, and so the default: a bare `nmake` prints usage and builds nothing.
# Building is always something you asked for by name.
help:
	@echo  nmake                     this message
	@echo  nmake all                 both flavours, and the tests for each
	@echo  nmake static              only out\$(ARCH)\static\winwrap.lib
	@echo  nmake dll                 only out\$(ARCH)\dll\winwrap.dll
	@echo  nmake test                only the unit tests for this flavour
	@echo  nmake DEBUG=1 all         unoptimised, /MTd, full debug info
	@echo  nmake check-forwarders    verify the generated .asm is current
	@echo  nmake clean
	@echo.
	@echo  This prompt builds for $(ARCH). Run from a Visual Studio command
	@echo  prompt: x86 Native Tools gives a 32-bit winwrap, x64 Native Tools a
	@echo  64-bit one.

# Both flavours. Two NMAKE invocations because the form decides FORMDEF and
# OUTDIR and NMAKE settles macros at parse time, so one run cannot be both.
# DEBUG is forwarded explicitly -- see its note above for why that is safe.
all:
	@$(MAKE) /nologo DEBUG=$(DEBUG) static
	@$(MAKE) /nologo DEBUG=$(DEBUG) test
	@$(MAKE) /nologo DEBUG=$(DEBUG) SHARED=1 dll-build
	@$(MAKE) /nologo DEBUG=$(DEBUG) SHARED=1 test
	@echo.
	@echo  Built both flavours for $(ARCH):
	@echo    out\$(ARCH)\static\winwrap.lib   the archive
	@echo    out\$(ARCH)\dll\winwrap.dll      the shared library
	@echo    out\$(ARCH)\dll\winwrap.lib      its import library
	@echo  and winwrap_test.exe in each, linked against that flavour.
	@echo.
	@echo  Separate directories on purpose: both flavours produce a
	@echo  winwrap.lib, and they are not the same kind of file.

static: $(STATICLIB)
	@echo.
	@echo  Built $(STATICLIB) for $(ARCH).
	@echo  Link with /INCLUDE:$(ANCHOR) $(SUBSYS) and advapi32.lib
	@echo  -- see the header of this makefile.

# Recursive for the reason given on `all`: this invocation was parsed without
# SHARED, so it cannot produce the shared form itself.
dll:
	@$(MAKE) /nologo DEBUG=$(DEBUG) SHARED=1 dll-build

dll-build: $(SHAREDLIB)
	@echo.
	@echo  Built $(SHAREDLIB) for $(ARCH),
	@echo  with its import library at $(OUTDIR)\winwrap.lib.

# The unit tests, beside the library they test. Console rather than windows,
# and at the same subsystem floor, so the executable itself runs on XP -- which
# is the only place several of its assertions mean anything.
test: $(TESTEXE)
	@echo.
	@echo  Built $(TESTEXE).
	@echo  Run it from that directory; nothing needs installing.

$(TESTEXE): $(OBJDIR)\winwrap_unittest.obj $(LIBTARGET)
	link /nologo /OUT:$@ /DEBUG /PDB:$(OUTDIR)\winwrap_test.pdb \
		$(SUBSYSCON) $(FORCEREDIRECTS) \
		$(OBJDIR)\winwrap_unittest.obj $(WINWRAPLIB) $(LIBS) $(TESTLIBS)

$(OBJDIR)\winwrap_unittest.obj: test\winwrap_unittest.cc test\winwrap_unittest.h $(HEADERS)
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	cl $(TESTFLAGS) test\winwrap_unittest.cc

# /ignore:4221 because a couple of these translation units are legitimately
# empty on some configurations, which lib warns about to no useful end.
#
# Guarded, because under SHARED $(STATICLIB) and the DLL's /IMPLIB are the same
# path and this rule would be a way to overwrite one with the other. With it
# absent, `nmake SHARED=1 static` fails with "don't know how to make" instead,
# which is the right answer: there is no static archive in the shared form.
!IFNDEF SHARED
$(STATICLIB): $(OBJECTS)
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	lib /nologo /ignore:4221 /OUT:$@ $(OBJECTS)
!ENDIF

# The shared form. winwrap.def exports the public Win32 names aliased onto the
# ww_ implementations, so the generated import library provides __imp_<name>
# and a consumer binds to winwrap.dll rather than to kernel32.
#
# It still needs the forwarders for itself: winwrap.dll links the static CRT,
# and that CRT calls FlsAlloc and InitializeCriticalSectionEx -- so without
# redirecting its own calls the DLL would import from kernel32 the very
# functions it exists to supply, and fail to load on XP before any of its own
# code runs.
$(SHAREDLIB): $(OBJECTS) $(OBJDIR)\winwrap_dll.obj $(OBJDIR)\winwrap.res src\winwrap.def
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	link /nologo /DLL /OUT:$@ /DEF:src\winwrap.def \
		/IMPLIB:$(OUTDIR)\winwrap.lib /DEBUG /PDB:$(OUTDIR)\winwrap.pdb \
		$(SUBSYS) $(LIBS) \
		$(OBJECTS) $(OBJDIR)\winwrap_dll.obj $(OBJDIR)\winwrap.res

# Batch inference rule, so one cl invocation compiles everything that is out of
# date rather than one per file.
{src}.cc{$(OBJDIR)}.obj::
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	cl $(CXXFLAGS) $<

$(OBJDIR)\forwarders.obj: src\forwarders_$(ARCH).asm
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	$(ASM) $(ASFLAGS) /Fo$@ src\forwarders_$(ARCH).asm

$(OBJDIR)\winwrap.res: src\winwrap.rc src\winwrap_version.h
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	rc /nologo $(FORMDEF) /DUSING_WINWRAP /fo$@ /I src src\winwrap.rc

# Every object depends on every header. The set is small and they include one
# another freely, so tracking it per file would be precision without value.
#
# winwrap_dll.obj needs its own line: it is built only in the shared form and
# so is deliberately not in $(OBJECTS), which meant editing winwrap.h or
# winwrap_version.h left a stale object linked into the DLL.
$(OBJECTS): $(HEADERS)
$(OBJDIR)\winwrap_dll.obj: $(HEADERS)

# forwarders_$(ARCH).asm is generated from static_redirects.cc and checked in,
# so this build needs no Python. This target only verifies the two agree, which
# is why it is not a dependency of anything.
check-forwarders:
	python tools\gen_forwarders.py --check

clean:
	@if exist out rmdir /s /q out
	@echo  Cleaned.

