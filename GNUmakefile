# Copyright 2026 Alex313031. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# GNU Makefile for winwrap. For MinGW-w64, not MSVC.
#
#  make                   print usage and build nothing
#  make all               both flavours, and the tests for each
#  make static            only out/<arch>/static/libwinwrap.a
#  make dll               only out/<arch>/dll/winwrap.dll
#  make test              only the unit tests for this flavour
#  make DEBUG=1 all       unoptimised, -Og, with full debug info -g2
#  make check-forwarders  verify the generated files are current
#  make clean             remove build artifacts
#
# On Windows this is `mingw32-make`, from a prompt with g++, ar, windres and
# dlltool on PATH. Cross-compiling from another system, name the toolchain:
#
#  make CROSS=i686-w64-mingw32- all
#  make CROSS=x86_64-w64-mingw32- all
#
# The architecture is whatever that g++ targets; there is no -m32/-m64 switch
# here, for the same reason the NMAKE file takes it from the prompt.
#
# --- Why this exists alongside Makefile, BUILD.gn and winwrap.gyp ----------
#
# GNU make reads GNUmakefile in preference to Makefile and NMAKE reads only
# Makefile, so the two sit side by side and each tool finds its own. They build
# the same library from the same sources with the same defines; where a flag
# below has an MSVC counterpart, the comment names it, so the two can be
# compared line for line.
#
# What differs is the redirect table. MSVC cannot compile
# src/static_redirects.cc, which names its symbols with asm labels, and
# assembles src/asm/forwarders_<arch>.asm in its place. GCC can, so this build
# compiles the .cc and never touches the .asm.
#
# --- Consuming the result --------------------------------------------------
#
# Link libwinwrap.a and force the redirect object in, which the static form
# needs because nothing in a consumer references winwrap by name. This is
# /INCLUDE: in the MSVC builds:
#
#   x86   g++ ... -Wl,--undefined=_winwrap_static_redirects libwinwrap.a
#   x64   g++ ... -Wl,--undefined=winwrap_static_redirects  libwinwrap.a
#
# Put libwinwrap.a ahead of the system libraries, which is where g++ puts
# anything named on the command line. Then set the subsystem floor, or the
# loader rejects the binary whatever its imports turn out to be -- MSVC's
# /SUBSYSTEM:WINDOWS,5.01:
#
#   x86   -Wl,--subsystem,windows:5.01     (or console:5.01)
#   x64   -Wl,--subsystem,windows:5.02
#
# And link -ladvapi32, which winwrap itself needs; see LIBS below.

# --- Tools -----------------------------------------------------------------

CROSS   ?=
CXX     := $(CROSS)g++
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
WINDRES := $(CROSS)windres
DLLTOOL := $(CROSS)dlltool
PYTHON  ?= python3

# mingw32-make runs recipes through cmd.exe unless it finds sh.exe on PATH, and
# the two disagree about how to make a directory. cmd.exe echoes the quotes it
# is given and a POSIX shell strips them, which is the cheapest way to tell.
ifeq ($(shell echo "sh"),sh)
MKDIR = mkdir -p $1
RMRF  = rm -rf $1
else
MKDIR = if not exist "$(subst /,\,$1)" mkdir "$(subst /,\,$1)"
RMRF  = if exist "$(subst /,\,$1)" rmdir /s /q "$(subst /,\,$1)"
endif

# --- Architecture ----------------------------------------------------------
#
# ANCHOR is decorated the way a C symbol is, so x86 wants the leading
# underscore and x64 does not. The subsystem floors are the Makefile's.
ifneq ($(filter x86_64-%,$(shell $(CXX) -dumpmachine)),)
ARCH      := x64
ANCHOR    := winwrap_static_redirects
SUBSYS    := -Wl,--subsystem,windows:5.02
SUBSYSCON := -Wl,--subsystem,console:5.02
else
ARCH      := x86
ANCHOR    := _winwrap_static_redirects
SUBSYS    := -Wl,--subsystem,windows:5.01
SUBSYSCON := -Wl,--subsystem,console:5.01
endif

# --- The three macros, which form they select, and where it goes -----------
#
# WINWRAP_DLL and WINWRAP_STATIC say which form is being built, and
# USING_WINWRAP is the marker a consumer may test for. All three mean what the
# Makefile says they mean, and are set in the same places: on the library, on
# the resource compiler, and on the test.
#
# Each form gets its own directory, out/$(ARCH)/static and out/$(ARCH)/dll,
# matching the NMAKE layout. The file names inside differ from that build's --
# libwinwrap.a for the archive and libwinwrap.dll.a for the import library,
# where MSVC calls both winwrap.lib -- so the collision that layout was
# designed around does not arise here, but a reader comparing the two trees
# should find things in the same place.
ifdef SHARED
FORM    := dll
FORMDEF := -DWINWRAP_DLL
else
FORM    := static
FORMDEF := -DWINWRAP_STATIC
endif

OUTDIR := out/$(ARCH)/$(FORM)
OBJDIR := $(OUTDIR)/obj

# The only import library winwrap needs beyond kernel32; see the Makefile's
# note on LIBS. A consumer of the static library needs it on its own link line.
LIBS := -ladvapi32

# winwrap compiles against the oldest SDK surface it supports, as a guardrail:
# it stops winwrap reaching for a declaration the OS it stands in for does not
# have. The Makefile's TARGETDEFS, value for value.
TARGETDEFS := -D_WIN32_WINNT=0x0501 -DNTDDI_VERSION=0x05010300 -DWINVER=0x0501

# DEBUG is tested by value so the recursive invocations below can forward it
# unconditionally, as in the Makefile.
#
# There is no counterpart here to /MT against /MTd: MinGW has one C runtime,
# and -static, on the link lines, is what makes libgcc and libstdc++ part of
# the binary rather than DLLs it would need beside it.
DEBUG ?= 0
ifneq ($(DEBUG),0)
OPTFLAGS := -Og -g2 -D_DEBUG
else
OPTFLAGS := -O2 -g0 -DNDEBUG
endif

# -fno-exceptions and -fno-rtti are /EHs-c- and /GR-: winwrap needs neither,
# and both would pull runtime machinery into a library whose purpose is to
# have no surprising dependencies. -Wall -Wextra stands in for /W4.
CXXFLAGS := -std=c++17 -Wall -Wextra -fno-exceptions -fno-rtti \
	$(FORMDEF) -DUSING_WINWRAP -DUNICODE -D_UNICODE \
	$(TARGETDEFS) -Isrc $(OPTFLAGS)

# Every source except winwrap_dll.cc, which holds what only a DLL wants.
# static_redirects.cc is in the list, unlike the Makefile's: see the top.
# plain_names.cc is its companion -- the same table as functions' own names
# rather than their import slots -- and an ordinary archive member, extracted
# when a name is referenced rather than forced in by the anchor.
SOURCES := \
	cancel_io \
	condition_variable \
	critical_section \
	dbghelp \
	directories \
	dispatch \
	etw \
	extra \
	fibers \
	files \
	fls \
	init_once \
	io_completion \
	locale \
	lock_family \
	misc \
	net \
	parking_lot \
	pipes \
	plain_names \
	process \
	registry \
	shell32 \
	srwlock \
	static_redirects \
	sync_objects \
	system \
	thread_pool \
	threads \
	tick_count \
	timezone

OBJECTS := $(SOURCES:%=$(OBJDIR)/%.o)

HEADERS := \
	src/dispatch.h \
	src/framework.h \
	src/lock_family.h \
	src/parking_lot.h \
	src/public/winwrap_public.h \
	src/winwrap.h \
	src/winwrap_version.h

STATICLIB := $(OUTDIR)/libwinwrap.a
SHAREDLIB := $(OUTDIR)/winwrap.dll
IMPLIB    := $(OUTDIR)/libwinwrap.dll.a
TESTEXE   := $(OUTDIR)/winwrap_test.exe

# What the test links, and what has to exist first. Named by path rather than
# as -lwinwrap, because -static makes the linker look only for libwinwrap.a
# and it would never find the import library.
#
# Forcing the redirect object in is a static-form concern only: the import
# library already provides every name, and the anchor is not exported.
ifdef SHARED
WINWRAPLIB     := $(IMPLIB)
LIBTARGET      := $(SHAREDLIB)
FORCEREDIRECTS :=
else
WINWRAPLIB     := $(STATICLIB)
LIBTARGET      := $(STATICLIB)
FORCEREDIRECTS := -Wl,--undefined=$(ANCHOR)
endif

# The test compiles against a CURRENT SDK surface, not winwrap's lowered one:
# it calls the Win32 names in order to test them. The Makefile gets that by
# leaving TARGETDEFS off and taking the Windows SDK's default. MinGW's default
# depends on how the toolchain was configured and can be as low as Windows
# 2000, so here it is asked for by name.
#
# Exceptions and RTTI stay on, as /EHsc leaves them: the harness streams to
# std::wcout. WINWRAP_TEST marks the translation unit as the test, as
# test/BUILD.gn does. $(FORMDEF) tells it which form it was linked against.
TESTDEFS := -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000 -DWINVER=0x0A00

TESTCXXFLAGS := -std=c++17 -Wall -Wextra $(FORMDEF) -DWINWRAP_TEST \
	-DUSING_WINWRAP -DUNICODE -D_UNICODE $(TESTDEFS) \
	-Isrc -Isrc/public -Itest $(OPTFLAGS)

# The Makefile's TESTLIBS: advapi32 for the registry test, user32 for
# wsprintfW, ole32 for CoTaskMemFree, shell32 for SHGetFolderPathW.
TESTLIBS := -ladvapi32 -luser32 -lole32 -lshell32

# The export table. x64 has no name decoration and takes winwrap.def as it
# stands, letting the linker write the import library. x86 needs the generated
# file with the @bytes suffixes; --kill-at then exports the plain names, and
# the import library is built separately by dlltool, which keeps the suffix on
# the symbols a call site references. See the header of that file.
ifeq ($(ARCH),x64)
DEFFILE  := src/dll/winwrap.def
DLLFLAGS := -Wl,--out-implib,$(IMPLIB)
else
DEFFILE  := src/dll/winwrap_gnu_x86.def
DLLFLAGS := -Wl,--kill-at
endif

# FIRST, and so the default: a bare `make` prints usage and builds nothing.
help:
	@echo  make                    this message
	@echo  make all                both flavours, and the tests for each
	@echo  make static             only out/$(ARCH)/static/libwinwrap.a
	@echo  make dll                only out/$(ARCH)/dll/winwrap.dll
	@echo  make test               only the unit tests for this flavour
	@echo  make DEBUG=1 all        unoptimised, -Og, full debug info
	@echo  make check-forwarders   verify the generated files are current
	@echo  make clean              remove build artifacts
	@echo  This toolchain, $(CXX), builds for $(ARCH).
	@echo  Set CROSS to a prefix such as i686-w64-mingw32- to pick another.

# Both flavours. Two invocations each because the form decides FORMDEF and
# OUTDIR, and those are settled when the makefile is read.
all:
	@$(MAKE) --no-print-directory DEBUG=$(DEBUG) static
	@$(MAKE) --no-print-directory DEBUG=$(DEBUG) test
	@$(MAKE) --no-print-directory DEBUG=$(DEBUG) SHARED=1 dll-build
	@$(MAKE) --no-print-directory DEBUG=$(DEBUG) SHARED=1 test
	@echo  Built both flavours for $(ARCH) under out/$(ARCH),
	@echo  with winwrap_test.exe in each, linked against that flavour.

ifndef SHARED
static: $(STATICLIB)
	@echo  Built $(STATICLIB) for $(ARCH).
	@echo  Link with -Wl,--undefined=$(ANCHOR) $(SUBSYS) and -ladvapi32
	@echo  -- see the header of this makefile.
else
# There is no static archive in the shared form; say so rather than build one
# into the wrong directory.
static:
	@echo  make static does not take SHARED=1.
	@exit 1
endif

# Recursive for the reason given on `all`.
dll:
	@$(MAKE) --no-print-directory DEBUG=$(DEBUG) SHARED=1 dll-build

dll-build: $(SHAREDLIB)
	@echo  Built $(SHAREDLIB) for $(ARCH),
	@echo  with its import library at $(IMPLIB).

# The unit tests, beside the library they test. Console, and at the same
# subsystem floor, so the executable itself runs on XP. -municode because the
# entry point is wmain.
test: $(TESTEXE)
	@echo  Built $(TESTEXE).
	@echo  Run it from that directory. Nothing needs installing.

TESTOBJECTS := $(OBJDIR)/winwrap_unittest.o $(OBJDIR)/plain_names_test.o

$(TESTEXE): $(TESTOBJECTS) $(LIBTARGET)
	$(CXX) -o $@ $(TESTOBJECTS) $(FORCEREDIRECTS) $(WINWRAPLIB) \
		-municode -static $(SUBSYSCON) $(LIBS) $(TESTLIBS)

$(OBJDIR)/winwrap_unittest.o: test/winwrap_unittest.cc test/winwrap_unittest.h \
		test/plain_names_test.h $(HEADERS)
	@$(call MKDIR,$(OBJDIR))
	$(CXX) -c $(TESTCXXFLAGS) -o $@ test/winwrap_unittest.cc

# The test's one C file, which has to be C: see its header. TESTCXXFLAGS
# without the language standard, which gcc would reject for C.
$(OBJDIR)/plain_names_test.o: test/plain_names_test.c test/plain_names_test.h
	@$(call MKDIR,$(OBJDIR))
	$(CC) -c $(filter-out -std=c++17,$(TESTCXXFLAGS)) -o $@ test/plain_names_test.c

# Rebuilt from scratch each time, because `ar r` adds to an existing archive
# and would leave a removed source's object behind in it.
$(STATICLIB): $(OBJECTS)
	@$(call MKDIR,$(OUTDIR))
	@$(call RMRF,$@)
	$(AR) rcs $@ $(OBJECTS)

# The shared form. It still needs the redirects for itself, which is why
# static_redirects.o is among $(OBJECTS) here too: the runtime linked into
# winwrap.dll calls some of the functions winwrap supplies, and without its
# own redirects the DLL would import them from kernel32 and fail to load on XP.
$(SHAREDLIB): $(OBJECTS) $(OBJDIR)/winwrap_dll.o $(OBJDIR)/winwrap_res.o $(DEFFILE)
	@$(call MKDIR,$(OUTDIR))
	$(CXX) -shared -static -o $@ $(OBJECTS) $(OBJDIR)/winwrap_dll.o \
		$(OBJDIR)/winwrap_res.o $(DEFFILE) $(DLLFLAGS) $(SUBSYS) $(LIBS)
ifneq ($(ARCH),x64)
	$(DLLTOOL) -k -d $(DEFFILE) -D winwrap.dll -l $(IMPLIB)
endif

$(OBJDIR)/%.o: src/%.cc $(HEADERS)
	@$(call MKDIR,$(OBJDIR))
	$(CXX) -c $(CXXFLAGS) -o $@ $<

# The same rule for the two subdirectories: src/asm for plain_names.cc and
# src/dll for winwrap_dll.cc. The objects still land flat in $(OBJDIR).
$(OBJDIR)/%.o: src/asm/%.cc $(HEADERS)
	@$(call MKDIR,$(OBJDIR))
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/%.o: src/dll/%.cc $(HEADERS)
	@$(call MKDIR,$(OBJDIR))
	$(CXX) -c $(CXXFLAGS) -o $@ $<

# The version resource. The same three inputs rc gets in the Makefile: the
# form, the marker and the include directory.
$(OBJDIR)/winwrap_res.o: src/dll/winwrap.rc src/winwrap_version.h
	@$(call MKDIR,$(OBJDIR))
	$(WINDRES) $(FORMDEF) -DUSING_WINWRAP -Isrc -O coff -o $@ src/dll/winwrap.rc

# The generated files are checked in, so this build needs no Python. This
# target only verifies they agree with their sources. One of them,
# src/dll/winwrap_gnu_x86.def, is this build's own input.
check-forwarders:
	$(PYTHON) tools/gen_forwarders.py --check

clean:
	@$(call RMRF,out)
	@echo  Cleaned.

.PHONY: help all static dll dll-build test check-forwarders clean
