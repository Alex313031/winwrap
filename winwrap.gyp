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
# Builds with MSVC. No clang-cl, and no extra Visual Studio component
#
# src/static_redirects.cc is absent from the sources below, and
# src/forwarders_x86.asm and src/forwarders_x64.asm are there instead. They are
# the same table of __imp_<name> slots: the C++ version names its symbols with
# a Clang asm label, because __imp__GetTickCount64@0 is not a legal C
# identifier, and MSVC has no asm labels at all -- cl.exe answers with C2059
# and C2146 on every line of it. MASM can name anything, so the table
# assembles rather than compiles.
#
# A build takes one or the other and never both: two definitions of a slot is
# a duplicate symbol. BUILD.gn keeps static_redirects.cc because the GN build
# serves Chromium and Electron, which compile with clang-cl regardless.
#
# One prerequisite, and it is on gyp-next rather than on the toolchain: its
# MASM settings table has to know about PreprocessorDefinitions, or the
# architecture define below is silently dropped. See the long note on the
# target. A consumer on an unpatched gyp-next needs to carry that fix as a
# local patch.
#
# ---------------------------------------------------------------------------
# READ THIS BEFORE DEPENDING ON IT: the consumer has work to do
#
# Two things this file cannot do for you.
#
# 1. Forcing the redirects into the link. winwrap works by defining the
#    __imp_<name> slots that dllimport call sites load through, which only
#    captures the call if winwrap is searched before the import library it
#    stands in front of.
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
#    So the consumer has to force winwrap's redirects into the link itself,
#    with /INCLUDE: on the executable that links winwrap. Name the anchor
#    winwrap provides for this, winwrap_static_redirects, which
#    takes the usual decoration -- so x86 wants a leading underscore and x64
#    does not, and it therefore has to be set per configuration:
#
#      'configurations': {
#        'x86_Base': {
#          'abstract': 1,
#          'msvs_settings': {
#            'VCLinkerTool': {
#              'AdditionalOptions': ['/INCLUDE:_winwrap_static_redirects'],
#            },
#          },
#        },
#        'x64_Base': {
#          'abstract': 1,
#          'msvs_settings': {
#            'VCLinkerTool': {
#              'AdditionalOptions': ['/INCLUDE:winwrap_static_redirects'],
#            },
#          },
#        },
#      },
#
#    One symbol is enough, and any symbol in that object would do: the whole
#    redirect table is one translation unit -- forwarders_<arch>.asm in this
#    build, static_redirects.cc in the GN one -- so a single object carries all
#    of the slots, and they reference the ww_* implementations, which pulls
#    those in turn. The anchor exists so the contract is a symbol
#    of winwrap's own rather than whichever shim a consumer happened to pick --
#    naming __imp__GetTickCount64@0 works, but it breaks with LNK2001 if that
#    shim is ever dropped, and a consumer should not have to track which
#    functions winwrap covers.
#
#    Not /WHOLEARCHIVE:winwrap.lib, which was the first attempt and does
#    nothing. It is recorded here because everything about it looks right and
#    someone will reach for it again: the option was on the link line, the
#    archive held every member including static_redirects.obj, and dumpbin
#    confirmed the symbols were correctly decorated -- and the import table
#    did not change by a single entry.
#
#    That last part is what proves the option matched nothing, rather than
#    matching and losing a race. /WHOLEARCHIVE promotes an archive's members
#    to command-line objects, and objects are loaded unconditionally before
#    any library is searched -- so had it matched, winwrap's members would
#    have been in the link whether or not anything referenced them, bringing
#    their own imports along: SleepEx, SwitchToThread, CreateFiber,
#    ConvertThreadToFiber, SetErrorMode, CancelIo, ExpandEnvironmentStringsW.
#    Those all appeared the moment /INCLUDE: worked. Under /WHOLEARCHIVE not
#    one of them did. The likely cause is that it matches the library by the
#    name as written while MSBuild passes a quoted absolute path; either way
#    there is no warning and no error, and the result is indistinguishable
#    from never having linked winwrap. /INCLUDE: sidesteps the matching
#    question entirely, because an undefined-symbol reference makes the linker
#    go find whichever member defines it.
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

      # The architecture define for the forwarders, and the setting most easily
      # forgotten here: both .asm files guard their whole body on it, so the
      # one that does not apply assembles to an empty object and the one that
      # does has to be told which it is. Get this wrong and winwrap builds
      # perfectly cleanly and does nothing at all -- or, as it actually
      # presented, fails at the consumer's link with LNK2001 on the anchor
      # symbol, because that sits inside the same guard.
      #
      # Which is worth knowing because gyp-next drops this setting unless it
      # has been patched. Its MASM table carried one entry,
      # UseSafeExceptionHandlers, and ConvertToMSBuildSettings keeps only what
      # its table knows: anything else is reported as "unrecognized setting
      # MASM/PreprocessorDefinitions while converting to MSBuild" and then
      # silently omitted from the generated project. A consumer on an
      # unpatched gyp-next sees four such warnings and a broken link. The fix
      # is additive and upstream-shaped -- three _Same lines beside the
      # existing one -- so a consumer can carry it as a local patch until it
      # lands upstream.
      #
      # On the abstract per-architecture configurations for the same reason
      # MinimumRequiredVersion is, on the consumer side: one generated project
      # holds all four configurations, so nothing substituted at generation
      # time can differ between them.
      #
      # UseSafeExceptionHandlers is /safeseh, and required rather than tidy on
      # x86: link.exe defaults to /SAFESEH:YES there, and a MASM object
      # without the metadata fails the link with LNK2026, "module unsafe for
      # SAFESEH image". x64 keeps no such table and ml64 does not take it.
      #
      # Delivered twice, through two channels that do not overlap. The msvs
      # generator reads msvs_settings.MASM and writes PreprocessorDefinitions
      # into the project; the ninja generator does not read msvs_settings.MASM
      # at all -- its assembler rule is `$asm $defines $includes $asmflags`,
      # where $asmflags comes from GetAsmflags and carries only /safeseh, and
      # $defines is the target's own 'defines'. So a ninja generation got
      # neither WINWRAP_X86 nor WINWRAP_X64, both forwarders assembled empty,
      # and winwrap.lib came out with no slots and no anchor. Generating for
      # ninja is a reachable configuration rather than a hypothetical one --
      # it is the default off Windows for anything driving GYP there -- so
      # this has to hold for both generators.
      #
      # Putting it in 'defines' as well costs an extra /D on the cl.exe command
      # line for the .cc files, where nothing reads it. The .asm files now also
      # refuse to assemble when neither define arrives, so a third generator
      # with a third convention fails loudly instead of shipping an empty
      # library.
      'configurations': {
        'x86_Base': {
          'abstract': 1,
          'defines': ['WINWRAP_X86=1'],
          'msvs_settings': {
            'MASM': {
              'PreprocessorDefinitions': ['WINWRAP_X86=1'],
              'UseSafeExceptionHandlers': 'true',
            },
          },
        },
        'x64_Base': {
          'abstract': 1,
          'defines': ['WINWRAP_X64=1'],
          'msvs_settings': {
            'MASM': {
              'PreprocessorDefinitions': ['WINWRAP_X64=1'],
            },
          },
        },
      },

      # _winwrap_sources from src/BUILD.gn, with static_redirects.cc replaced by the
      # two forwarders_*.asm -- see the note at the top of this file. They are
      # what define the __imp_<name> slots the consumer's call sites load
      # through, so without them winwrap builds and does nothing.
      'sources': [
        'src/cancel_io.cc',
        'src/condition_variable.cc',
        'src/critical_section.cc',
        'src/directories.cc',
        'src/dispatch.cc',
        'src/dispatch.h',
        'src/etw.cc',
        'src/extra.cc',
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
        'src/net.cc',
        'src/parking_lot.cc',
        'src/parking_lot.h',
        'src/pipes.cc',
        'src/process.cc',
        'src/public/winwrap_public.h',
        'src/registry.cc',
        'src/shell32.cc',
        'src/srwlock.cc',
        'src/forwarders_x86.asm',
        'src/forwarders_x64.asm',
        'src/sync_objects.cc',
        'src/system.cc',
        'src/thread_pool.cc',
        'src/threads.cc',
        'src/tick_count.cc',
        'src/timezone.cc',
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
        # As the makefiles and src/BUILD.gn pass them. The sources define both
        # for themselves if nobody has, so this is agreement, not behaviour.
        'UNICODE',
        '_UNICODE',
      ],

      # Drop the consuming tree's Windows version defines from this target, so
      # that the /D flags further down are the only ones naming these macros on
      # the command line.
      #
      # Purely to silence warning D9025, "overriding '/Dx' with '/Dy'", which
      # cl emits once per superseded flag per translation unit whenever one
      # command-line flag overrides another for the same macro. Three macros
      # across every source file is a lot of noise for something working
      # exactly as intended.
      #
      # An optimisation, not the mechanism. The /D flags below remain what
      # guarantees the version winwrap compiles at, because they are emitted
      # last and cl honours these in command-line order. If the values here
      # stop matching what the consumer sets -- a Chromium roll moving
      # common.gypi's numbers, or a tree that sets its own -- the exclusion
      # matches nothing and the only consequence is that D9025 returns.
      # Correctness does not depend on this list being accurate, which is what
      # makes it safe to keep a copy of someone else's values here at all.
      #
      # These are build/common.gypi's values for a Chromium around M49:
      # Windows 10. A tree that overrides them for its own targets usually
      # does so in a file it includes only into those, which does not reach
      # third_party, so this is what arrives here.
      'defines!': [
        '_WIN32_WINNT=0x0A00',
        'WINVER=0x0A00',
        'NTDDI_VERSION=0x0A000000',
      ],
      'direct_dependent_settings': {
        # src/public and not src, matching winwrap_public_config in BUILD.gn,
        # so a consumer of either build system sees the same one header.
        # Exposing src here instead would put winwrap.h on a GYP consumer's
        # include path and not a GN one, and the documented
        # #include "winwrap_public.h" would resolve in only one of them.
        #
        # The ww_* shims are reached through the import table rather than by
        # name -- a consumer never writes ww_GetTickCount64, it writes
        # GetTickCount64 and winwrap owns the __imp_ slot behind it -- so
        # winwrap.h offers a consumer only names nobody should be calling.
        'include_dirs': [
          'src/public',
        ],
        'defines': [
          'USING_WINWRAP',
        ],
      },
      'msvs_settings': {
        'VCCLCompilerTool': {
          'AdditionalOptions': [
            # The equivalent of winwrap_private_config in src/BUILD.gn. These go
            # in AdditionalOptions rather than 'defines', and in this order,
            # because MSBuild emits AdditionalOptions last on the command line
            # and cl honours /D and /U in command-line order -- so these are
            # what finally takes effect over whatever the consuming tree set.
            #
            # A guardrail, not a target setting. winwrap has to run on the
            # oldest OS it supports, so it must not see a declaration the SDK
            # would only offer on something newer: that is the mistake it
            # exists to repair. This applies to winwrap's own sources only.
            #
            # No /U before each /D, unlike src/BUILD.gn's copy of this, whose
            # comment says the undef is needed "to avoid redefinition errors".
            # It is not: /D on a command line is resolved by the driver rather
            # than by the preprocessor, so a repeated one is not a macro
            # redefinition. cl takes the last and says so with warning D9025,
            # "overriding '/Dx' with '/Dy'" -- and a /U followed by a /D is the
            # same override, which is where three D9025 lines per translation
            # unit were coming from. The 'defines!' above removes the
            # consuming tree's /D so that, in the common case, each of these
            # is the only flag naming its macro and nothing is overridden.
            #
            # Should that exclusion stop matching, this still wins on being
            # last, and D9025 comes back to say the list needs updating. That
            # is the right failure: noisy, not wrong.
            '/D_WIN32_WINNT=0x0501',  # XP
            # NTDDI_VERSION too, or the guardrail only half works: plenty of
            # SDK headers gate on it rather than on _WIN32_WINNT.
            # OVERLAPPED_ENTRY is the case that caught this -- still visible
            # at _WIN32_WINNT=0x0501 until NTDDI came down as well.
            '/DNTDDI_VERSION=0x05010300',  # NTDDI_WINXPSP3
            # WINVER as well, which winwrap_private_config does not need to
            # touch because Chromium's GN build does not set it. GYP-era
            # common.gypi does, and some SDK headers gate on WINVER rather
            # than on either of the above.
            '/DWINVER=0x0501',
            # winwrap is a C++17 project. This pins winwrap's own sources
            # without disturbing a consumer that builds at a different
            # standard, and without winwrap quietly coming to depend on
            # whichever one that is.
            #
            # Last on the command line is what makes it take effect, and the
            # regex filter below is what keeps it from being an override: with
            # any other /std: removed from this target, this is the only one
            # cl sees.
            '/std:c++17',
            # Make MSVC emit the real C++ standard, not 199711L
            '/Zc:__cplusplus',
          ],

          # Remove every /std: except winwrap's own, so that nothing is
          # overridden and cl has no reason to emit D9025.
          #
          # Exclude-then-include rather than one pattern with a negative
          # lookahead: it is GYP's own idiom for this -- see the docstring on
          # ProcessListFiltersInDict -- and a later 'include' overriding an
          # earlier 'exclude' is the documented behaviour rather than an
          # implementation detail. It also reads as what it is.
          #
          # Written as a regex filter and not an 'AdditionalOptions!'
          # exclusion list because that matches by exact string, so it would
          # have to name every standard a consumer might have chosen.
          # /std:c++14, c++20, c++latest and anything added later are all
          # covered here without naming any of them.
          #
          # Applies to this target only. The filter runs after the consuming
          # tree's target_defaults have been merged in, so the list it sees is
          # the combined one -- and it is scoped to the dict it sits in, which
          # is winwrap's. Nothing else in the tree changes standard.
          #
          # If this ever stops matching, the failure is the same as for the
          # version defines above: /std:c++17 still wins on being last, and
          # D9025 returns to say so.
          'AdditionalOptions/': [
            ['exclude', '^/std:'],
            ['include', '^/std:c\\+\\+17$'],
          ],
        },
      },
    },
    {
      # The unit tests. Not built unless asked for: a consumer depends on
      # ':winwrap', so adding this changes nothing for them.
      #
      #   gyp ... && msbuild winwrap.vcxproj /t:winwrap_test
      #
      'target_name': 'winwrap_test',
      'type': 'executable',
      'dependencies': [
        'winwrap',
      ],
      # All three named here rather than leaning on the dependency, which
      # supplies src/public and nothing else -- that is the consumer's view,
      # and this test is not a consumer: it includes <winwrap.h> to reach the
      # ww_* shims by name. src/public is still listed because the test
      # includes <winwrap_public.h> the way a consumer writes it, so that
      # moving that header out from under the public surface breaks the test
      # rather than passing quietly. Matches test/BUILD.gn, which names the
      # same three.
      'include_dirs': [
        'test',
        'src',
        'src/public',
      ],
      'sources': [
        'test/winwrap_unittest.cc',
        'test/winwrap_unittest.h',
      ],
      # WINWRAP_TEST marks the translation unit as the test, and
      # WINWRAP_STATIC says which form it was linked against -- this file
      # builds no other. Both as test/BUILD.gn and the makefiles set them.
      # USING_WINWRAP arrives through the dependency.
      'defines': [
        'WINWRAP_TEST',
        'WINWRAP_STATIC',
        'UNICODE',
        '_UNICODE',
      ],
      'msvs_settings': {
        'VCCLCompilerTool': {
          # The test compiles against the CURRENT SDK, not winwrap's lowered
          # one. It calls the Win32 names in order to test them, and includes
          # evntprov.h for EventWriteTransfer, which is gated on
          # WINVER >= 0x0600. That happens by itself here: winwrap's /U and /D
          # pairs are on its own target rather than in
          # direct_dependent_settings, so nothing lowers this one.
          #
          # Exceptions on, unlike the library. The harness streams to
          # std::wcout and formats through std::wostringstream, which a
          # Chromium-derived tree's ExceptionHandling 0 would not support.
          'ExceptionHandling': '1',
          'RuntimeTypeInfo': 'true',
        },
        'VCLinkerTool': {
          # Console, so the harness output goes somewhere.
          'SubSystem': 1,
          # advapi32 for the registry test, user32 for wsprintfW, ole32 for
          # CoTaskMemFree in the AppUserModelID test, shell32 for the
          # SHGetFolderPathW the known-folder test compares against. Named
          # explicitly because
          # this file has to work outside a Chromium-derived tree, where they
          # would already be on every link line -- which is why
          # test/BUILD.gn names none of them.
          'AdditionalDependencies': [
            'advapi32.lib',
            'user32.lib',
            'ole32.lib',
            'shell32.lib',
          ],
        },
      },
      'configurations': {
        # Same subsystem floor as the library's consumers, so the test
        # executable itself runs on XP -- which is the only place several of
        # its assertions mean anything. And the same /INCLUDE: anchor, for the
        # same reason a consumer needs it: nothing here references a winwrap
        # symbol by name, so without it the redirect object is never pulled in
        # and the test would quietly measure the operating system instead.
        'x86_Base': {
          'abstract': 1,
          'msvs_settings': {
            'VCLinkerTool': {
              'MinimumRequiredVersion': '5.01',
              'AdditionalOptions': ['/INCLUDE:_winwrap_static_redirects'],
            },
          },
        },
        'x64_Base': {
          'abstract': 1,
          'msvs_settings': {
            'VCLinkerTool': {
              'MinimumRequiredVersion': '5.02',
              'AdditionalOptions': ['/INCLUDE:winwrap_static_redirects'],
            },
          },
        },
      },
    },
  ],
}
