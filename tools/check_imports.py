#!/usr/bin/env python3

# Copyright 2026 Alex313031. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Checks the import table of a supplied binary and ensures that it is
importing winwrap's functions instead of Microsoft's.

Linking winwrap is not the same as using it. Its whole mechanism is to define
the __imp_<name> slots a dllimport call site loads through, and whether that
captures the call depends on link order and on something forcing the redirect
object in -- neither of which produces a diagnostic when it goes wrong. The
binary links, it runs on a current Windows, and it fails at load on the old one
it was built for. This reads the import table and says which happened.

Two things are checked, because they are the two halves of the recipe:

  * No function winwrap covers is still bound to a system DLL. If
    AcquireSRWLockExclusive appears in the KERNEL32 import table, winwrap lost
    the link and the slot came from kernel32.lib instead.
  * The subsystem version is low enough for the target. A binary can have a
    spotless import table and still be refused by the loader for claiming
    6.00.

Usage:

    python3 tools/check_imports.py out/x86/static/winwrap_unittests.exe
    python3 tools/check_imports.py --min-os 5.01 Sawbuck.exe
    python3 tools/check_imports.py --show-all Sawbuck.exe

The covered-function list is read from src/static_redirects.cc, so it cannot
drift from what winwrap actually redirects.

No dumpbin and no pefile: the PE is parsed here. winwrap has no dependencies
and its tools should not acquire any -- and this way the check runs in CI on
any machine, Windows or not, with nothing installed.
"""

import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REDIRECTS = os.path.join(ROOT, 'src', 'static_redirects.cc')

# The modules winwrap stands in front of. A covered function imported from any
# of these is the failure this tool exists to catch; the same name imported
# from winwrap.dll is the shared form working correctly.
SYSTEM_DLLS = frozenset(
    ['kernel32.dll', 'advapi32.dll', 'user32.dll', 'shell32.dll', 'ntdll.dll'])


def covered_functions(path=REDIRECTS):
    """Every function winwrap redirects, read from static_redirects.cc."""
    text = open(path, encoding='utf-8').read()
    # Drop the macro definition itself, whose parameter is literally 'name'.
    text = re.sub(r'#define WINWRAP_REDIRECT.*?\n(?:.*\\\n)*.*\n', '', text)
    names = set(re.findall(r'WINWRAP_REDIRECT\((\w+),\s*\d+\)', text))
    if not names:
        sys.exit('error: no WINWRAP_REDIRECT entries found in %s' % path)
    return names


# --- A very small PE reader ------------------------------------------------
#
# Only as much as the question needs: the subsystem version from the optional
# header, and the names in the import and delay-import tables. No exports, no
# relocations, no resources.

class PEError(Exception):
    pass


class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b'MZ':
            raise PEError('not a PE file (no MZ signature)')
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        if data[pe:pe + 4] != b'PE\0\0':
            raise PEError('not a PE file (no PE signature)')

        self.machine, sections = struct.unpack_from('<HH', data, pe + 4)
        opt_size = struct.unpack_from('<H', data, pe + 20)[0]

        opt = pe + 24
        magic = struct.unpack_from('<H', data, opt)[0]
        if magic == 0x10B:
            self.bits, dirs_at = 32, opt + 96
        elif magic == 0x20B:
            self.bits, dirs_at = 64, opt + 112
        else:
            raise PEError('unknown optional header magic 0x%X' % magic)

        # MajorSubsystemVersion sits at optional-header offset 48 in both PE32
        # and PE32+, which is the one convenient thing about that header --
        # everything after ImageBase is shifted by 4 between the two.
        self.subsystem_major, self.subsystem_minor = struct.unpack_from(
            '<HH', data, opt + 48)

        count = struct.unpack_from('<I', data, dirs_at - 4)[0]
        self.directories = [struct.unpack_from('<II', data, dirs_at + 8 * i)
                            for i in range(count)]

        self.sections = []
        sec = opt + opt_size
        for i in range(sections):
            _, vsize, vaddr, rawsize, rawptr = struct.unpack_from(
                '<8sIIII', data, sec + 40 * i)
            self.sections.append((vaddr, vsize, rawsize, rawptr))

    @property
    def arch(self):
        return {0x14C: 'x86', 0x8664: 'x64', 0xAA64: 'arm64'}.get(
            self.machine, 'machine 0x%X' % self.machine)

    def offset(self, rva):
        """File offset for a virtual address, or None if it is not mapped."""
        for vaddr, vsize, rawsize, rawptr in self.sections:
            if vaddr and vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None

    def string(self, rva):
        at = self.offset(rva)
        if at is None:
            return None
        end = self.data.find(b'\0', at)
        return self.data[at:end].decode('ascii', 'replace') if end > at else None

    def _thunk_names(self, rva):
        """Names in a thunk array. Imports by ordinal have no name to give."""
        names = []
        at = self.offset(rva)
        if at is None:
            return names
        step = 4 if self.bits == 32 else 8
        fmt = '<I' if self.bits == 32 else '<Q'
        ordinal_flag = 1 << (self.bits - 1)
        while at + step <= len(self.data):
            value = struct.unpack_from(fmt, self.data, at)[0]
            if value == 0:
                break
            if not value & ordinal_flag:
                # IMAGE_IMPORT_BY_NAME is a hint word followed by the name.
                name = self.string(value + 2)
                if name:
                    names.append(name)
            at += step
        return names

    def imports(self):
        """{lowercased dll: set(names)} from the import directory."""
        return self._walk(1, 20, lambda f: (f[3], f[0] or f[4]))

    def delay_imports(self):
        """The same, from the delay-load directory."""
        return self._walk(13, 32, lambda f: (f[1], f[4] or f[3]))

    def _walk(self, directory, stride, pick):
        """Shared descriptor-table walk. `pick` returns (name_rva, thunk_rva)."""
        result = {}
        if len(self.directories) <= directory:
            return result
        rva = self.directories[directory][0]
        at = self.offset(rva) if rva else None
        if at is None:
            return result
        fields = stride // 4
        while at + stride <= len(self.data):
            values = struct.unpack_from('<%dI' % fields, self.data, at)
            if not any(values):
                break
            name_rva, thunk_rva = pick(values)
            dll = (self.string(name_rva) or '?').lower()
            result.setdefault(dll, set()).update(self._thunk_names(thunk_rva))
            at += stride
        return result


def check(path, covered, min_os, show_all):
    """Reports on one binary. Returns the number of problems found."""
    try:
        pe = PE(open(path, 'rb').read())
    except (OSError, PEError, struct.error) as error:
        print('  FAIL   %s: %s' % (path, error))
        return 1

    print('%s  (%s, subsystem version %d.%02d)'
          % (path, pe.arch, pe.subsystem_major, pe.subsystem_minor))

    problems = 0
    static, delayed = pe.imports(), pe.delay_imports()

    # 1. Covered functions still coming from the system.
    for kind, table in (('', static), (' [delay]', delayed)):
        for dll in sorted(table):
            if dll not in SYSTEM_DLLS:
                continue
            lost = sorted(table[dll] & covered)
            if lost:
                problems += len(lost)
                print('  FAIL   %s%s supplies %d function(s) winwrap covers:'
                      % (dll, kind, len(lost)))
                for name in lost:
                    print('           %s' % name)

    # 2. The shared form, where those names should come from winwrap.dll.
    from_winwrap = sorted(
        (static.get('winwrap.dll', set()) | delayed.get('winwrap.dll', set()))
        & covered)
    if from_winwrap:
        print('  ok     winwrap.dll supplies %d: %s'
              % (len(from_winwrap), ', '.join(from_winwrap)))

    # 3. The subsystem floor, which an import table cannot speak to.
    if min_os:
        want = tuple(int(p) for p in min_os.split('.'))
        have = (pe.subsystem_major, pe.subsystem_minor)
        if have > want:
            problems += 1
            print('  FAIL   subsystem version %d.%02d is above the requested '
                  '%d.%02d, so the loader will refuse this' % (have + want))
        else:
            print('  ok     subsystem version %d.%02d is at or below %d.%02d'
                  % (have + want))

    if show_all:
        for title, table in (('import', static), ('delay', delayed)):
            for dll in sorted(table):
                print('  [%s] %s (%d)' % (title, dll, len(table[dll])))
                for name in sorted(table[dll]):
                    mark = '   <- covered by winwrap' if name in covered else ''
                    print('      %s%s' % (name, mark))

    if not problems:
        print('  ok     nothing winwrap covers is bound to a system DLL')
    return problems


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split('\n\n')[0],
        epilog='The covered-function list comes from src/static_redirects.cc.',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('binaries', nargs='+', metavar='BINARY',
                        help='executables or DLLs to inspect')
    parser.add_argument('--min-os', metavar='MAJOR.MINOR',
                        help='also require a subsystem version no higher than '
                             'this; 5.01 for Windows XP, 5.02 for XP x64')
    parser.add_argument('--show-all', action='store_true',
                        help='list every import, marking the covered ones')
    args = parser.parse_args()

    covered = covered_functions()
    problems = sum(check(path, covered, args.min_os, args.show_all) or 0
                   for path in args.binaries)
    print()
    if problems:
        print('%d problem(s). If covered functions are still bound to a system '
              'DLL, winwrap is linked but not winning -- see the note on '
              'forcing the redirect object in, in winwrap.gyp or the Makefile.'
              % problems)
        return 1
    print('All clear.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
