# Winwrap Usage

How to incorporate winwrap into your own project to support legacy Windows.
For building the library itself, see [BUILDING.md](./BUILDING.md).

## Integrating into your own code
The main thing, no matter what the build system, is that the consumer imports winwraps functions over the system ones.

That happens entirely at link time. **No source file in the consumer changes, and
nothing includes a winwrap header.** A `__declspec(dllimport)` call — which is
how the Windows SDK declares every Win32 function — does not jump to the
function; it loads a pointer out of `__imp_<name>` and calls through that. So
defining `__imp_<name>` is enough to capture every call site in the binary,
including the ones inside the CRT and inside static libraries you did not build.
The few references that name a function directly instead — a C table of
function addresses, a declaration without `dllimport` — are answered too; see
[*Functions referenced by name*](./BUILDING.md#functions-referenced-by-name).
See [*About function overriding*](./BUILDING.md#about-function-overriding) for
how that works.

What that leaves is four link settings and one check.

### 1. Link winwrap, ahead of `kernel32.lib`
Archive members are pulled in to satisfy references that are *still undefined*,
so whether winwrap or kernel32 supplies a given slot comes down to which library
the linker reaches first.

If your link line names no import libraries explicitly, there is nothing to do:
`kernel32.lib` arrives through the `/DEFAULTLIB` directives the SDK embeds in
every object, and those are processed *after* libraries named on the command
line. An explicitly linked winwrap therefore already wins. That is the case for
Chromium's GN build.

If your link line *does* name `kernel32.lib` — GYP-era Chromium's `common.gypi`
names it and a dozen others — then it is a command-line library too and position
decides. winwrap has to come first. In GYP that means the `+` suffix, which is
its prepend policy for a list being merged into an existing one:

```python
'msvs_settings': {
  'VCLinkerTool': {
    'AdditionalDependencies+': ['$(OutDir)lib\\winwrap.lib'],
  },
},
```

A plain `AdditionalDependencies` appends, which puts winwrap after
`kernel32.lib` and produces a binary where every function winwrap covers is
still bound to kernel32 — with no warning of any kind. In a hand-maintained
`.vcxproj`, the equivalent is putting `winwrap.lib` at the *start* of
Linker → Input → Additional Dependencies rather than leaving it to the project
reference.

### 2. Force the redirect object in
Static form only. Nothing in a consumer references winwrap by name, so pass
`/INCLUDE:` for the anchor symbol winwrap provides for exactly this purpose:

| architecture | option |
|---|---|
| x86 | `/INCLUDE:_winwrap_static_redirects` |
| x64 | `/INCLUDE:winwrap_static_redirects` |

The leading underscore on x86 is the usual decoration, so this has to be set per
architecture rather than once. One symbol is enough, and it does not matter
which: the whole redirect table is a single translation unit, so one object
carries every slot, and those slots reference the `ww_*` implementations, which
pulls those in behind them.

Step 1 alone is not quite enough, which is why both are here. Ordering decides
who wins a reference that *exists*; the anchor guarantees the redirect object is
in the link even for a slot nothing in the binary happens to call.

The anchor is a symbol of winwrap's own rather than one of the `__imp_` slots on
purpose. `/INCLUDE:__imp__GetTickCount64@0` works, but it breaks with `LNK2001`
if that shim is ever dropped, and a consumer should not have to track which
functions winwrap covers.

Not `/WHOLEARCHIVE:winwrap.lib`. It looks like exactly the right tool and it
does nothing at all — see the note in [`winwrap.gyp`](../winwrap.gyp) for the
evidence, recorded there because someone will reach for it again.

### 3. Lower the subsystem version
This is the other half of the recipe, and it is independent of everything above:
winwrap supplies the imports the old Windows does not export, and the subsystem
version is what makes its loader accept the binary in the first place. A binary
with a spotless import table is still refused if it claims 6.00.

| architecture | option | GYP |
|---|---|---|
| x86 | `/SUBSYSTEM:WINDOWS,5.01` (or `CONSOLE,5.01`) | `'MinimumRequiredVersion': '5.01'` |
| x64 | `/SUBSYSTEM:WINDOWS,5.02` | `'MinimumRequiredVersion': '5.02'` |

5.02 on x64 is not a typo and not a limitation of winwrap: 64-bit Windows begins
at Server 2003 / XP x64, and 5.02 is as low as the linker goes for that machine.

Note what this is *not*: it is a linker setting, and it is not `_WIN32_WINNT`.
Lowering `_WIN32_WINNT` in your own tree hides declarations from your code,
breaks third-party libraries that expect a current SDK, and does nothing
whatsoever about the imports that actually fail at load. winwrap exists so that
you do not have to.

### 4. Link `advapi32.lib`
winwrap itself needs it, for `RegOpenKeyExW`, `RegQueryValueExW` and
`RegCloseKey` in `registry.cc`. Most Windows programs link it already, so this
is easy to miss until something does not. winwrap's entire static-link surface
is kernel32 plus advapi32; user32, shell32 and ntdll are all reached through
`GetProcAddress` and produce no import at all.

### 5. Check that it actually worked
Linking winwrap and *using* it are different things, and when the difference
goes wrong nothing says so: the binary links, it runs on your machine, and it
fails at load on the Windows it was built for. So verify rather than assume:

```bat
python3 tools\check_imports.py --min-os 5.01 example.exe
```

It reads the import table and the subsystem version straight out of the PE and
checks the two halves of the recipe against each other — that nothing winwrap
covers is still bound to a system DLL, and that the subsystem floor is low
enough. The covered-function list comes from `src/static_redirects.cc`, so it
cannot disagree with the library. There are no dependencies, and it does not
have to run on Windows.

```
example.exe  (x86, subsystem version 5.01)
  ok     subsystem version 5.01 is at or below 5.01
  ok     nothing winwrap covers is bound to a system DLL
```

A failure names the functions that got away:

```
  FAIL   kernel32.dll supplies 11 function(s) winwrap covers:
           AcquireSRWLockExclusive
           FlsAlloc
           ...
```

which is step 1 or step 2 — winwrap is linked but losing. `--show-all` lists
every import and marks the covered ones, which is what to reach for when the
answer is not obvious from that.

### Porting a new program: finding what is still missing
The same tool is the quickest way into a program that has never run on an old
Windows. Passing step 5 means winwrap won every function it covers. It does
not mean the program will start: an import of a newer function winwrap has no
shim for has nothing to be compared against, and passes in silence. On the old
Windows that shows up as the loader refusing the program and naming one
function it could not find — one per attempt, however many there are.

`--show-all` gives the whole list at once:

```bat
python3 tools\check_imports.py --show-all --min-os 5.01 YourProgram.exe
```

```
  [import] kernel32.dll (212)
      AcquireSRWLockExclusive   <- covered by winwrap
      CreateFileW
      GetProductInfo
      ...
  [delay] shell32.dll (9)
      ...
```

Every import is there, grouped by DLL, static imports first and delay-loaded
ones after, with the functions winwrap supplies marked.
The unmarked ones are the candidates. The tool does not know which Windows
each of those arrived in, so that part is yours: the loader's error message
names the first, Microsoft's documentation gives a "minimum supported client"
for each, and Dependency Walker run on the old system flags every missing one
in a single pass.

For each function that turns out to be missing there are two ways out.

**Change the program.** Often the better answer, and sometimes the only one:

- If the call sits on a path the old Windows never needs, look the function up
  with `GetProcAddress` and skip it when it is absent, or delay-load the DLL so
  that the import is only resolved if the call is made.
- If an older function does the same job, call that.
- If the import came from a header rather than from anything the program
  wrote, a build setting may remove it. `PSAPI_VERSION=1` is the usual
  example: without it, a Windows 7 SDK turns `GetProcessMemoryInfo` into
  `K32GetProcessMemoryInfo`, which XP and Vista do not have, and with it the
  call goes to psapi.dll, which they do.

**Add it to winwrap.** Worth doing when the function can be built honestly out
of what the old Windows has, and when a second program is likely to want it.
[FUNCTION_LIST.md](FUNCTION_LIST.md) shows what is already covered and how, and
[INCOMPLETE_FUNCS.md](INCOMPLETE_FUNCS.md) shows the standard for a shim that
cannot do everything the real function does: it says so, and fails in a way the
caller already handles. A new one needs:

1. the implementation, `ww_<Name>` in the source file for its area, handing
   over to the system's own function wherever that exists;
2. its declaration in `src/winwrap.h`;
3. a `WINWRAP_REDIRECT(<Name>, <bytes>)` line in `src/static_redirects.cc`,
   where `<bytes>` is the x86 `__stdcall` argument size;
4. an export line in `src/dll/winwrap.def`;
5. `python3 tools/gen_forwarders.py`, which regenerates the MASM tables --
   slots and plain names both -- and the MinGW `.def` from those;
6. a unit test in `test/winwrap_unittest.cc` — every shim has one;
7. a row in `FUNCTION_LIST.md`, and an entry in `INCOMPLETE_FUNCS.md` if it
   does less than the real function.

Pull requests for new functions are welcome. Say which program needed it and
which Windows you ran the unit tests on; a shim that has only been compiled is
worth much less than one that has been run on the system it is for.

### Using the shared form
Linking `winwrap.dll` instead changes two of the five steps:

* **Step 2 does not apply.** The import library generated from
  `src/dll/winwrap.def` exports the public Win32 names aliased onto the `ww_*`
  implementations, so it supplies every `__imp_<name>` directly and there is
  nothing to force in.
* **Step 4 does not apply** to your link line; the DLL carries advapi32 itself.

Steps 1, 3 and 5 are unchanged, ordering included — an import library is still a
library, and still has to be reached before `kernel32.lib`. In
`check_imports.py` output the covered names move rather than disappearing:

```
  ok     winwrap.dll supplies 12: AcquireSRWLockExclusive, FlsAlloc, ...
```

Ship `winwrap.dll` beside the executable, and run `check_imports.py` on the DLL
as well — it has to load on the old Windows itself before anything it provides
can matter.

### What a consumer can include
Linking winwrap puts `src/public` on your include path, and that directory is
the whole public surface:

```c++
#include "winwrap_public.h"
```

`USING_WINWRAP` is defined for anything that links winwrap, through
`direct_dependent_settings` in `winwrap.gyp` and `winwrap_public_config` in
`BUILD.gn`. Nothing inside winwrap reads it; it is there so that a consumer can
test for it.

#### Extra functions
`winwrap_public.h` declares the handful of useful extra functions that winwrap
supplies (in its own right, rather than as stand-ins for Win32):

| Function | Description |
|----------|-------------|
| `GetRealNTVersion()` | The host's real NT version from `RtlGetNtVersionNumbers`, in the form `_WIN32_WINNT` takes — `0x0501` on XP. Unaffected by the manifest-based lying `GetVersionEx` does from Windows 8 on. Compare with `>=`, so an unknown version reads as older |
| `IsRunningOnWine(const char** out = nullptr)` | Whether this is Wine, optionally with its version string. The pointer is into ntdll's own data and needs no freeing |
| `GetWinWrapVersion(unsigned* major, unsigned* minor, unsigned* build)` | Which winwrap this is. Returns the version as a string, e.g. `L"0.3.8"`; every out parameter is optional, so pass three `nullptr`s to use the return value alone |

`DllGetVersion` is **not** in the header, though `winwrap.dll` **does** export it.
A dozen system DLLs export that name too and winwrap's is rarely the one a caller
means, so a declaration on the include path would resolve to the wrong thing
as easily as the right one. Use `GetProcAddress` on the module in question,
with the SDK's own `DLLGETVERSIONPROC` for the pointer type — which is what
`shlwapi.h` instructs where it declares the protocol:

> The caller should always `GetProcAddress("DllGetVersion")`, not implicitly
> link to it.

It is a shared-build export only; there is no such entry point in
`winwrap.lib`. `test/winwrap_unittest.cc` exercises it exactly that way, and
only when built against the DLL.

### Calling a shim directly
Rarely wanted, and required by none of the above: the shims are reached through
the import table, so you write `GetTickCount64` and winwrap owns the `__imp_`
slot behind it.

`winwrap.h`, which declares every implementation as `ww_<Win32Name>` with the
exact signature of the function it stands in for, is **not** on a consumer's
include path — `winwrap_public_config` exposes `src/public` and nothing else.
That is deliberate, so the only names offered are ones meant to be written. If
you genuinely need one — to test a shim, or to take winwrap's behaviour
deliberately on a current OS — add `src` to your own include path and include
`winwrap.h` directly. winwrap's own unit test does exactly that, in
`test/BUILD.gn`.

### Working examples
[Sawbuck-ng](https://github.com/Alex313031/sawbuck-ng) does all five steps in
one conditional block, for a GYP project that is not Chromium:
`sawbuck/viewer/viewer.gyp`, under `sawbuck_use_winwrap`. Every setting there
carries a comment on why it is written the way it is, including the two attempts
that failed first and the import-table evidence that identified each.

For a list of other projects that use winwrap, see [CONSUMERS.md](CONSUMERS.md).

## Integrating into Chromium/Electron's GN system
This is how winwrap is able to be built in tree using Chromium's native tools, and linked
to all targets.

Sync it to `//third_party/winwrap` — `BUILD.gn` and `winwrap.gni` ship in this
repository, so they are in place after any `gclient sync` — then turn it on with
the two arguments described in
[BUILDING.md](./BUILDING.md#using-chromiumelectrons-gn-system):

```gn
use_winwrap = true
is_component_winwrap = false
```

"Linked to all targets" is the part GN does for you, and it is why none of the
five steps above reappear here:

* **Everything gets it, from one edit.** `//build/config:common_deps` is
  depended on by every executable, shared library and loadable module in the
  tree — it is where Chromium already puts libc++ and the allocator. Adding
  `//third_party/winwrap:winwrap_lib` there covers the whole build, all several
  hundred binaries of it, with nothing per-target.

* **Depend on the group, not on a library.** `:winwrap_lib` expands to
  `:winwrap_static`, to `:winwrap_shared`, or to nothing at all when
  `use_winwrap = false`, so the dependency can stay unconditional at the call
  site and an ordinary build links nothing.

* **Ordering is already right**, which is step 1: GN names no import libraries
  on the link line, so `kernel32.lib` comes in through `/DEFAULTLIB` and is
  searched after everything explicit. No `/INCLUDE:` anchor and no reordering —
  this is the case winwrap's own notes describe as winning for free.

* **The subsystem version comes from the toolchain**, which is step 3: the
  `console` and `windowed` configs in `//build/config/win/BUILD.gn` append it to
  every link, from one variable. In a tree carrying electron-legacy's patch
  series that is already 5.01 on x86 and 5.02 on x64 — reverting upstream's
  "set subsystem version to 10.0 across the board" is what puts it back. Outside
  such a tree, that revert is the one change the host tree needs.

* **winwrap opts out of the cycle it would otherwise create.** `common_deps`
  reaching winwrap means winwrap reaching itself, since every `shared_library`
  picks `common_deps` up by default; `winwrap_shared` sets
  `no_default_deps = true` to break that. The static form needs nothing, because
  `static_library` targets never get `common_deps` in the first place.

* **It still redirects its own calls.** `static_redirects.cc` is in both link
  modes, not only the static one. winwrap.dll links the MSVC static CRT, and
  that CRT calls `FlsAlloc` and `InitializeCriticalSectionEx` — so without the
  redirects applying to itself, the DLL would import from kernel32 the very
  functions it exists to supply, and fail to load on XP before any of this runs.

The GN build is also the one place `src/static_redirects.cc` is compiled rather
than the generated `.asm` pair, since Chromium and Electron build with clang-cl
regardless; [BUILDING.md](./BUILDING.md#msvc) has the detail on why that table
exists twice.

To build and run the tests in tree, `//third_party/winwrap:winwrap_all` builds
both forms plus `winwrap_test`, and is `testonly`. Step 5 still applies —
run `check_imports.py` on the finished binaries, since it is the only thing that
distinguishes a working integration from a silently absent one.
