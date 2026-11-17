# Copyright 2026 Alex313031. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# GYP build for winwrap, alongside BUILD.gn.
#
# winwrap is integrated into Chromium's GN build, but GYP is what older
# Chromium-derived trees still use -- anything at M55 or earlier has no
# BUILD.gn to put winwrap into. This file is here rather than in the consuming
# tree on purpose: a consumer syncs winwrap with gclient, and gclient sync
# --reset --delete_unversioned_trees deletes whatever that checkout does not
# itself carry. A build file kept outside the checkout survives but has to
# reach back into it; one shipped here is simply present after every sync.
#
# Referenced as, for a tree that syncs winwrap to third_party/winwrap:
#
#   '<(DEPTH)/third_party/winwrap/winwrap.gyp:winwrap'
#
# ---------------------------------------------------------------------------
# Prerequisite: the "C++ Clang Compiler for Windows" Visual Studio component
#
# This target builds with clang-cl even in a tree that is otherwise MSVC,
# because static_redirects.cc needs Clang asm labels and MSVC has no
# equivalent. That component is not installed by default. See the long note on
# the target itself for why, and for what it would take to lift the
# requirement. Check for it with:
#
#   "%ProgramFiles%\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\Llvm\\bin\\clang-cl.exe" --version
#
# ---------------------------------------------------------------------------
# READ THIS BEFORE DEPENDING ON IT: the consumer has work to do
#
# Two things this file cannot do for you.
#
# 1. Link order. winwrap works by defining the __imp_<name> slots that
#    dllimport call sites load through, which only captures the call if
#    winwrap is searched before the import library it stands in front of.
#    static_redirects.cc explains that kernel32.lib arrives through
#    /DEFAULTLIB directives, which are processed after libraries named on the
#    command line, so an explicitly linked winwrap wins -- and that is true of
#    Chromium's GN build.
#
#    It is not true everywhere. GYP-era Chromium names kernel32.lib and a
#    dozen others explicitly in common.gypi's AdditionalDependencies, so they
#    are on the command line too and order decides. Confirmed the hard way:
#    with winwrap linked but after kernel32.lib, every function it covers was
#    still bound to kernel32 in the finished binary -- Fls*, the SRWLOCK pair,
#    InitializeCriticalSectionEx, the condition variables, all of them.
#
#    So the consumer should add, on the executable that links winwrap:
#
#      'msvs_settings': {
#        'VCLinkerTool': {
#          'AdditionalOptions': ['/WHOLEARCHIVE:winwrap.lib'],
#        },
#      },
#
#    which pulls every winwrap object in up front, so the slots are defined
#    before kernel32 is ever consulted.
#
# 2. The subsystem version, which is the other half of running on XP. winwrap
#    supplies the imports XP does not export; MinimumRequiredVersion is what
#    lets the loader accept the binary at all. 5.01 for Win32 and 5.02 for x64,
#    which is as low as the linker goes for that machine.
#
# ---------------------------------------------------------------------------
# Only the static form is built
#
# winwrap.gni defaults is_component_winwrap to false, and static is what a
# self-contained executable wants. The shared form would need winwrap.def
# through ModuleDefinitionFile, winwrap.rc compiled in, winwrap_dll.cc added
# and the MSVC static CRT; it is not built here until something wants one
# winwrap.dll shared between binaries.
{
  'variables': {
    # For a Chromium-derived consumer, this selects first-party warning
    # settings. Harmless anywhere else -- nothing in this file reads it.
    'chromium_code': 1,
  },
  'targets': [
    {
      'target_name': 'winwrap',
      'type': 'static_library',

      # winwrap compiles with clang-cl, even in a tree that is otherwise MSVC.
      # This is a requirement, not a preference.
      #
      # static_redirects.cc names its symbols with a Clang asm label:
      #
      #   extern "C" decltype(&ww_Foo) winwrap_imp_Foo __asm__("__imp__Foo@4")
      #       = &ww_Foo;
      #
      # which is the only way to produce the name from source, because
      # __imp__Foo@4 is not a legal C identifier. MSVC has no asm labels and
      # no equivalent, so cl.exe reports a wall of C2059 and C2146 on every
      # WINWRAP_REDIRECT line. /alternatename is the obvious-looking answer
      # and does not work: the linker consults an alternate name only when the
      # original symbol is undefined, and kernel32.lib defines __imp_<name>,
      # so it always loses.
      #
      # Mixing toolsets within one solution is fine here. Symbol names are
      # ABI, not compiler-specific, so MSVC-compiled call sites reference
      # __imp__Foo@4 and this object defines it; winwrap's whole surface is
      # extern "C", so no C++ ABI crosses the boundary. /WHOLEARCHIVE is a
      # link.exe flag and is unaffected.
      #
      # The cost is that a GYP consumer needs the "C++ Clang Compiler for
      # Windows" Visual Studio component, which is not installed by default.
      # Removing that requirement means giving static_redirects.cc an MSVC
      # path -- the __imp_ symbols defined in MASM, which can name anything --
      # and that is worth doing, since the whole point of shipping a .gyp is
      # to be consumable outside a Chromium-style clang build. Until then this
      # is the narrow way to get the redirects at all.
      #
      # On Common_Base because gyp-next reads msbuild_toolset from the
      # target's default configuration, and every concrete configuration
      # inherits from this one.
      'configurations': {
        'Common_Base': {
          'abstract': 1,
          'msbuild_toolset': 'ClangCL',
        },
      },

      # Mirrors _winwrap_sources in BUILD.gn. static_redirects.cc is the whole
      # mechanism in this mode: it is what defines the __imp_<name> slots the
      # consumer's call sites load through.
      'sources': [
        'src/app_user_model_id.cc',
        'src/cancel_io.cc',
        'src/condition_variable.cc',
        'src/critical_section.cc',
        'src/directories.cc',
        'src/dispatch.cc',
        'src/dispatch.h',
        'src/etw.cc',
        'src/fibers.cc',
        'src/files.cc',
        'src/fls.cc',
        'src/framework.h',
        'src/init_once.cc',
        'src/io_completion.cc',
        'src/locale.cc',
        'src/lock_family.cc',
        'src/lock_family.h',
        'src/misc.cc',
        'src/parking_lot.cc',
        'src/parking_lot.h',
        'src/pipes.cc',
        'src/process.cc',
        'src/registry.cc',
        'src/srwlock.cc',
        'src/static_redirects.cc',
        'src/sys.cc',
        'src/sys.h',
        'src/threads.cc',
        'src/tick_count.cc',
        'src/winwrap.h',
        'src/winwrap_version.h',
      ],
      'include_dirs': [
        'src',
      ],
      'defines': [
        # WINWRAP_STATIC picks the static form.
        'WINWRAP_STATIC',
        # USING_WINWRAP is from winwrap_public_config, and GN applies a
        # public_config to the target itself as well as to its dependents --
        # hence both here and in direct_dependent_settings.
        'USING_WINWRAP',
      ],
      'direct_dependent_settings': {
        'include_dirs': [
          'src',
        ],
        'defines': [
          'USING_WINWRAP',
        ],
      },
      'msvs_settings': {
        'VCCLCompilerTool': {
          'AdditionalOptions': [
            # The equivalent of winwrap_internal_config in BUILD.gn. These go
            # in AdditionalOptions rather than 'defines', and in this order,
            # because MSBuild emits AdditionalOptions last on the command line
            # and cl honours /D and /U in command-line order -- so these are
            # what finally takes effect over whatever the consuming tree set.
            #
            # A guardrail, not a target setting. winwrap has to run on the
            # oldest OS it supports, so it must not see a declaration the SDK
            # would only offer on something newer: that is the mistake it
            # exists to repair. This applies to winwrap's own sources only.
            '/U_WIN32_WINNT',
            '/D_WIN32_WINNT=0x0501',  # XP
            # NTDDI_VERSION too, or the guardrail only half works: plenty of
            # SDK headers gate on it rather than on _WIN32_WINNT.
            # OVERLAPPED_ENTRY is the case that caught this -- still visible
            # at _WIN32_WINNT=0x0501 until NTDDI came down as well.
            '/UNTDDI_VERSION',
            '/DNTDDI_VERSION=0x05010300',  # NTDDI_WINXPSP3
            # WINVER as well, which winwrap_internal_config does not need to
            # touch because Chromium's GN build does not set it. GYP-era
            # common.gypi does, and some SDK headers gate on WINVER rather
            # than on either of the above.
            '/UWINVER',
            '/DWINVER=0x0501',
            # winwrap is a C++17 project. Appended rather than substituted:
            # the last /std: on the command line is the one that counts, so
            # this pins winwrap's sources without disturbing a consumer that
            # builds at a different standard -- and without winwrap quietly
            # coming to depend on whichever one that is.
            '/std:c++17',
          ],
        },
      },
    },
  ],
}
