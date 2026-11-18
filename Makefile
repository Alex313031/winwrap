# Copyright 2026 Alex313031. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# NMAKE makefile for winwrap. For MSVC, deliberately -- not MinGW.
#
#   nmake                     winwrap.lib for this prompt's architecture
#   nmake dll                 winwrap.dll and its import library as well
#   nmake DEBUG=1             unoptimised, /MTd, with full debug info
#   nmake check-forwarders    verify the generated .asm is current
#   nmake clean
#   nmake help
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

!IF "$(VSCMD_ARG_TGT_ARCH)" == "x64" || "$(PLATFORM)" == "x64"
ARCH    = x64
ASM     = ml64
ASFLAGS = /nologo /c
ANCHOR  = winwrap_static_redirects
SUBSYS  = /SUBSYSTEM:WINDOWS,5.02
!ELSE
ARCH    = x86
ASM     = ml
ASFLAGS = /nologo /c /coff
ANCHOR  = _winwrap_static_redirects
SUBSYS  = /SUBSYSTEM:WINDOWS,5.01
!ENDIF

OUTDIR = out\$(ARCH)
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
	/DWINWRAP_STATIC /DUSING_WINWRAP /DUNICODE /D_UNICODE \
	$(TARGETDEFS) /I src /Fo$(OBJDIR)\ /Fd$(OBJDIR)\winwrap.pdb

!IFDEF DEBUG
CXXFLAGS = $(CXXFLAGS) /Od /MTd /Zi /D_DEBUG
!ELSE
CXXFLAGS = $(CXXFLAGS) /O2 /Oi /MT /Zi /DNDEBUG
!ENDIF

# Every source except static_redirects.cc, whose job forwarders_$(ARCH).asm
# does here, and winwrap_dll.cc, which holds what only a DLL wants and would
# be dead weight in the static library.
OBJECTS = \
	$(OBJDIR)\app_user_model_id.obj \
	$(OBJDIR)\cancel_io.obj \
	$(OBJDIR)\condition_variable.obj \
	$(OBJDIR)\critical_section.obj \
	$(OBJDIR)\directories.obj \
	$(OBJDIR)\dispatch.obj \
	$(OBJDIR)\etw.obj \
	$(OBJDIR)\fibers.obj \
	$(OBJDIR)\files.obj \
	$(OBJDIR)\fls.obj \
	$(OBJDIR)\init_once.obj \
	$(OBJDIR)\io_completion.obj \
	$(OBJDIR)\locale.obj \
	$(OBJDIR)\lock_family.obj \
	$(OBJDIR)\misc.obj \
	$(OBJDIR)\parking_lot.obj \
	$(OBJDIR)\pipes.obj \
	$(OBJDIR)\process.obj \
	$(OBJDIR)\registry.obj \
	$(OBJDIR)\srwlock.obj \
	$(OBJDIR)\sys.obj \
	$(OBJDIR)\threads.obj \
	$(OBJDIR)\tick_count.obj \
	$(OBJDIR)\forwarders.obj

HEADERS = \
	src\dispatch.h \
	src\framework.h \
	src\lock_family.h \
	src\parking_lot.h \
	src\sys.h \
	src\winwrap.h \
	src\winwrap_version.h

STATICLIB = $(OUTDIR)\winwrap.lib
SHAREDLIB = $(OUTDIR)\winwrap.dll

all: $(STATICLIB)
	@echo.
	@echo  Built $(STATICLIB) for $(ARCH).
	@echo  Link with /INCLUDE:$(ANCHOR) and $(SUBSYS)
	@echo  -- see the header of this makefile.

dll: $(SHAREDLIB)
	@echo.
	@echo  Built $(SHAREDLIB) and $(OUTDIR)\winwrap.lib for $(ARCH).

# /ignore:4221 because a couple of these translation units are legitimately
# empty on some configurations, which lib warns about to no useful end.
$(STATICLIB): $(OBJECTS)
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
	lib /nologo /ignore:4221 /OUT:$@ $(OBJECTS)

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

$(OBJDIR)\winwrap.res: src\winwrap.rc
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	rc /nologo /fo$@ /I src src\winwrap.rc

# Every object depends on every header. The set is small and they include one
# another freely, so tracking it per file would be precision without value.
$(OBJECTS): $(HEADERS)

# forwarders_$(ARCH).asm is generated from static_redirects.cc and checked in,
# so this build needs no Python. This target only verifies the two agree, which
# is why it is not a dependency of anything.
check-forwarders:
	python3 tools\gen_forwarders.py --check

clean:
	@if exist out rmdir /s /q out
	@echo  Cleaned.

help:
	@echo  nmake                     winwrap.lib for this prompt's architecture
	@echo  nmake dll                 winwrap.dll and its import library as well
	@echo  nmake DEBUG=1             unoptimised, /MTd, full debug info
	@echo  nmake check-forwarders    verify the generated .asm is current
	@echo  nmake clean
	@echo.
	@echo  Run from a Visual Studio command prompt. The prompt's architecture
	@echo  decides the build's: x86 Native Tools gives a 32-bit winwrap, x64
	@echo  Native Tools a 64-bit one.
