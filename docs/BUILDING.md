# Building Winwrap

How to build winwrap using various build systems.
For using the built library, or integrating into other projects, see [USAGE.md](./USAGE.md).

## Building the Library

It can be compiled as a static lib `winwrap.lib` or shared library `winwrap.dll`, the main thing is that it *must* be linked ahead of
any other libraries or default libs, to make sure that the binary in question routes these calls through winwrap rather than binding straight to the system DLL.
Routing through it does not mean replacing it. Each function asks the running Windows for the real one first and only falls back to its own implementation where
there isn't one, so a binary with winwrap linked in uses the system's code on Windows 7 and the emulation on XP, with no separate build for either.
The cost on a modern OS is a cached pointer load and a jump.

### MSVC
This is the default supported way to build the project.

Two of the sources are the same table written twice, and **a build takes one or
the other, never both** — two definitions of a slot is a duplicate symbol:

| source | needs | used by |
|---|---|---|
| `src/static_redirects.cc`, `src/asm/plain_names.cc` | clang-cl or GCC | the GN build and the GNUmakefile |
| `src/asm/forwarders_x86.asm`, `src/asm/forwarders_x64.asm`, `src/asm/plain_names_x86.asm`, `src/asm/plain_names_x64.asm` | MASM (`ml`/`ml64`) | the Makefile and the GYP build |

Each form is two objects. The first of each pair defines the `__imp_<name>`
slots and is forced into every link. The second, `plain_names`, defines the
same functions under their own names, for the callers that do not go through a
slot — see [*Functions referenced by name*](#functions-referenced-by-name) —
and is an ordinary archive member, extracted only when one of those names is
referenced.

The C++ version names its symbols with a Clang asm label, because
`__imp__GetTickCount64@0` is not a legal C identifier and an asm label is the
only way to produce that name from source. MSVC has no asm labels, so `cl.exe`
answers with C2059 and C2146 on every line of it. MASM can name anything, so
the same table assembles instead.

The `.asm` files are generated from `static_redirects.cc` by
`tools/gen_forwarders.py` and checked in, so no build system needs Python.
Re-run it after adding or removing a `WINWRAP_REDIRECT`; `--check` verifies the
two are in step and is what CI runs:

```bat
python3 tools\gen_forwarders.py
python3 tools\gen_forwarders.py --check
```

#### Using the Makefile
This is the method that [CI](../.github/workflows/build_msvc.yml) uses, and the preferred method to get a standalone
library + unittest executables.

It is an NMAKE makefile, so run it from a Visual Studio command prompt. The
prompt's architecture *is* the build's: "x86 Native Tools Command Prompt" gives
a 32-bit winwrap, "x64 Native Tools" a 64-bit one. There is no cross-build
switch.

```bat
nmake                     :: print usage and build nothing
nmake all                 :: both flavours, and the unit tests for each
nmake static              :: only out\<arch>\static\winwrap.lib
nmake dll                 :: only out\<arch>\dll\winwrap.dll
nmake test                :: only the unit tests for this flavour
nmake DEBUG=1 all         :: unoptimised, /MTd, full debug info
nmake check-forwarders    :: verify the generated .asm is current
nmake clean
```

Output is split per flavour, because both of them produce a file called
`winwrap.lib` and they are not the same kind of file — the static form's is the
archive, the shared form's is the import library beside the DLL:

```
out\x86\static\    winwrap.lib               winwrap_test.exe
out\x86\dll\       winwrap.dll winwrap.lib   winwrap_test.exe
out\x64\static\    ...
out\x64\dll\       ...
```

Nothing else needs installing. Python, WiX and clang are all irrelevant here —
`cl`, `link`, `lib`, `ml` and `rc` come with the C++ workload.

#### Using GYP
This is the method that [Sawbuck-ng](https://github.com/Alex313031/sawbuck-ng) uses.

`winwrap.gyp` ships in this repository rather than in the consuming tree, and
deliberately: a consumer syncs winwrap with `gclient`, and
`gclient sync --reset --delete_unversioned_trees` deletes anything in that
checkout which upstream does not itself carry. Reference it as:

```python
'dependencies': [
  '<(DEPTH)/third_party/winwrap/winwrap.gyp:winwrap',
],
```

There is a second target, `winwrap_test`, which is not built unless asked
for — so adding the dependency above changes nothing for a consumer.

**One prerequisite, and it is on gyp-next rather than on the toolchain.** Its
MASM settings table has to know about `PreprocessorDefinitions`, or the
architecture define the `.asm` files guard on is silently discarded:
`ConvertToMSBuildSettings` keeps only the settings its own table lists and
drops the rest after printing a warning. The symptom is four
`unrecognized setting MASM/PreprocessorDefinitions` warnings at generation time
followed by `LNK2001` on the anchor symbol at the consumer's link, because that
symbol sits inside the same guard. The fix is three `_Same` lines beside the
existing one in `pylib/gyp/MSVSSettings.py`; sawbuck-ng carries it as
`patches/06-gyp-next-masm-settings.patch`.

#### Integrating into a custom .sln/vcxproj
Nothing here needs a generator. For a hand-maintained project:

1. **Add the sources** — every `.cc` directly in `src/` *except*
   `static_redirects.cc`, which needs clang, and the four `.asm` files in
   `src/asm/` in its place (`forwarders_*` and `plain_names_*`, both
   architectures). Leave out `src/asm/plain_names.cc`, which is the clang form
   of the latter. Add `src/dll/winwrap_dll.cc` and `src/dll/winwrap.rc`, and
   link with `src/dll/winwrap.def`, only if you are building the DLL.
2. **Enable MASM.** Right-click the project → Build Dependencies → Build
   Customizations → tick **masm**. Visual Studio then assembles `.asm` sources
   with `ml` or `ml64` according to the configuration's platform.
3. **Define the architecture for MASM.** On the `.asm` files' properties, under
   Microsoft Macro Assembler → General → Preprocessor Definitions, set
   `WINWRAP_X86` for Win32 and `WINWRAP_X64` for x64. Both files guard their
   whole body on this, so the one that does not apply assembles to an empty
   object. **Without the define, both do** — the project builds cleanly and
   winwrap does nothing whatsoever.
4. **Turn on `/safeseh` for x86**: Microsoft Macro Assembler → Advanced → Use
   Safe Exception Handlers. `link.exe` defaults to `/SAFESEH:YES` there, and a
   MASM object without the metadata fails with `LNK2026`, *"module unsafe for
   SAFESEH image"*. x64 keeps no such table.
5. **Set the compiler options** for winwrap's own sources: `/std:c++17`, plus
   `_WIN32_WINNT=0x0501`, `NTDDI_VERSION=0x05010300` and `WINVER=0x0501`. Those
   three are a guardrail rather than a target setting — they stop winwrap
   reaching for a declaration the OS it stands in for does not have, which is
   the mistake it exists to repair. They apply to winwrap's sources **only**;
   your own code keeps building against whatever SDK it likes.
6. **Define the form**: `WINWRAP_STATIC` for a static library, `WINWRAP_DLL`
   for a DLL — and give it to the resource compiler too, which is the one place
   it is actually read, selecting `FILETYPE` and `ORIG_FILENAME` in the version
   resource.
7. **Link `advapi32.lib`**, which `registry.cc` needs for `RegOpenKeyExW`,
   `RegQueryValueExW` and `RegCloseKey`. winwrap's entire static-link surface
   is kernel32 plus advapi32; user32, shell32 and ntdll are all reached through
   `GetProcAddress` and produce no import at all.

The Makefile is the reference for all of the above — every flag in it carries a
comment saying why it is there.

### MinGW

`GNUmakefile` is the MinGW-w64 counterpart of the NMAKE `Makefile`. GNU make
reads it in preference to `Makefile` and NMAKE does not know the name, so the
two sit side by side. It has the same targets and builds from the same sources
with the same defines.

On Windows, from a prompt with `g++`, `ar`, `windres` and `dlltool` on `PATH`:

```bat
mingw32-make                    :: print usage and build nothing
mingw32-make all                :: both flavours, and the unit tests for each
mingw32-make static             :: only out/<arch>/static/libwinwrap.a
mingw32-make dll                :: only out/<arch>/dll/winwrap.dll
mingw32-make test               :: only the unit tests for this flavour
mingw32-make DEBUG=1 all        :: unoptimised, -Og, full debug info
mingw32-make check-forwarders   :: verify the generated files are current
mingw32-make clean
```

The architecture is whatever the `g++` on `PATH` targets. To cross-compile, or
to pick between two toolchains, give the prefix:

```sh
make CROSS=i686-w64-mingw32- all
make CROSS=x86_64-w64-mingw32- all
```

The output layout matches the MSVC build's, with GNU file names:

```
out/x86/static/    libwinwrap.a                     winwrap_test.exe
out/x86/dll/       winwrap.dll libwinwrap.dll.a     winwrap_test.exe
```

To consume the static library, link it ahead of the system libraries, force the
redirect object in, and set the subsystem floor — the same three things as
`/INCLUDE:` and `/SUBSYSTEM:` on MSVC:

| | x86 | x64 |
|---|---|---|
| Force the redirects | `-Wl,--undefined=_winwrap_static_redirects` | `-Wl,--undefined=winwrap_static_redirects` |
| Subsystem floor | `-Wl,--subsystem,windows:5.01` | `-Wl,--subsystem,windows:5.02` |

plus `-ladvapi32`.

Two things differ from the MSVC build, both handled for you:

- It compiles `src/static_redirects.cc` and `src/asm/plain_names.cc` and never
  uses the generated `.asm`. GCC supports the asm labels and file-scope
  assembly those files need; MSVC does not.
- The 32-bit DLL is linked from `src/dll/winwrap_gnu_x86.def`, a generated copy of
  `winwrap.def` with the `@bytes` suffixes GNU ld needs written out.
  `tools/gen_forwarders.py` produces it and `check-forwarders` verifies it.

### Using Chromium/Electron's GN system
Winwrap can also be built using GN/Ninja. This is the form Chromium and
Electron use, and the one place `static_redirects.cc` is compiled rather than
the `.asm` pair, since those trees build with clang-cl regardless.

Two arguments, declared in `winwrap.gni`:

```gn
use_winwrap = true            # link it at all; off by default
is_component_winwrap = false  # winwrap.lib (default) rather than winwrap.dll
```

`use_winwrap` is off by default on purpose, and deliberately not implied by
`target_os == "win"`: the redirects capture *every* call in the binary, so a
build that will only ever run on a current Windows would get winwrap's own
emulation — a `CRITICAL_SECTION` in place of a real `SRWLOCK`, and so on — for
no benefit. Turn it on for a build that actually has to run on XP or Vista.

Depend on the group rather than on a library directly, so a build with
`use_winwrap = false` links nothing at all and the dependency can stay
unconditional at the call site:

```gn
deps = [ "//third_party/winwrap:winwrap_lib" ]
```

`//third_party/winwrap:winwrap_all` builds both forms plus the unit tests and
is `testonly`.

## About function overriding
Making a library that overrides the system or CRT's functions is a unique and strange task, it involves:

* **Capturing the call without touching the call site.** A
  `__declspec(dllimport)` call does not jump to the function; it loads a
  pointer out of `__imp_<name>` and calls through that. The Windows SDK
  declares every Win32 function that way, so *defining `__imp_<name>` yourself*
  redirects every caller in the binary with no source change anywhere. That is
  the whole mechanism.

* **Spelling a symbol that C cannot spell.** On x86 the slot is
  `__imp__GetTickCount64@0` — a leading underscore and the `__stdcall` argument
  size — which is not a legal C identifier. Clang can attach an asm label to
  name it and MSVC cannot, which is why the table exists twice, once in C++ and
  once in MASM.

* **Winning the link.** Archive members are pulled only to satisfy references
  that are *still undefined*, so whether winwrap or kernel32 supplies a slot
  comes down to which library the linker reaches first. Get that wrong and
  there is no error, no warning and no symptom until the binary is run on the
  old Windows it was built for. Worse, nothing pulls winwrap's redirect object
  in at all unless asked, because a consumer never references it by name —
  hence the `/INCLUDE:` anchor in [USAGE.md](./USAGE.md).

* **Deferring to the system when it has the function.** Replacing a function
  Windows already implements correctly is a downgrade, so every shim asks for
  the real one first and runs its own code only when the answer is no. The
  resolved pointer is cached in a relaxed atomic, making the cost on a modern
  OS a load, a test and a jump. It cannot use a function-local static: that
  goes through the CRT's thread-safe-statics machinery, and winwrap is called
  *by* the CRT while the CRT is still starting up.

* **Knowing when a family has to be all-or-nothing.** The `SRWLOCK` and
  `CONDITION_VARIABLE` functions share a lock object whose layout each
  implementation owns, so taking some from the system and some from winwrap
  would have one reading the other's internals as if they were its own. The
  `Fls*` four are the same. Those resolve as a group rather than one function
  at a time.

* **Not importing what you are standing in front of.** winwrap's own sources
  compile with `_WIN32_WINNT` lowered to XP, so the SDK hides anything newer
  from them. Without that guardrail it is easy for the shim for one Vista+
  function to be written in terms of another — and a library that cannot load
  is worse than no library.

* **Failing honestly when the job is impossible.** XP has no symbolic links, no
  Restart Manager and no manifest-based ETW, so `CreateSymbolicLinkW`,
  `RegisterApplicationRestart` and `EventWriteTransfer` cannot be made to work.
  Each returns the error its callers already test for rather than a plausible
  success, because a silently wrong answer travels much further than a visible
  failure. See [INCOMPLETE_FUNCS.md](./INCOMPLETE_FUNCS.md).

## Functions referenced by name
An `__imp_<name>` slot captures a call made through `__declspec(dllimport)`,
which is nearly all of them. Three kinds of reference bypass the slot and name
the function itself — `_GetTickCount64@0` on x86, `GetTickCount64` on x64:

- a declaration without `dllimport`. The SDK has a few
  (`SHGetKnownFolderPath` is plain `STDAPI`) and MinGW's headers have many;
- the address of a function in a C constant initializer, which cannot be
  loaded from a slot at run time. SQLite keeps a table of them —
  `{ "GetTickCount64", (SYSCALL)GetTickCount64, 0 }`;
- a caller that declares the function itself to avoid a header.

The system import library can satisfy such a reference, but the member that
does also defines the slot, so the link fails with a duplicate
`__imp_<name>` — reported by lld-link as *duplicate symbol:
`__declspec(dllimport) _GetTickCount64@0`*, defined in winwrap and in
KERNEL32.dll.

So winwrap also defines every redirected function under its own name, as a
jump to the shim, in the `plain_names` object. Nothing needs doing in a
consumer beyond what is already required: winwrap ahead of the system import
libraries on the link line. The unit test's `plain_names_test.c` is a C table
of exactly that kind, and stops linking if the names ever go missing.
