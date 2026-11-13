# winwrap lib

Win32 Legacy support library, designed for Chromium/Electron usage.

This is similar to [Supermium's](https://www.supermium.net/) [`progwrp.dll`](https://github.com/win32ss/supermium/blob/bf7e71c95f9a5a9a3ae5206ef8274633255de50b/.github/README.md#this-supermium-component-receives-enhancements-and-improvements-from-time-to-time-primarily-for-operating-systems-prior-to-windows-vista).

It was originally created as a component of [Electron Legacy](https://github.com/Alex313031/electron-legacy#synopsis) to allow it to run on Windows XP/Vista (7/8/8.1 support out of the box).

It is integrated into Chromium's [GN](https://gn.googlesource.com/gn/) build systen, although support for other build systems so that one may integrate it into their own project is planned.

## About
It can be compiled as a static lib `winwrap.lib` or shared library `winlib.dll`, the main thing is that it *must* be linked ahead of any other libraries or default libs, to make sure that the binary in question routes these calls through winwrap rather than binding straight to the system DLL.

Routing through it does not mean replacing it. Each function asks the running Windows for the real one first and only falls back to its own implementation where there isn't one, so a binary with winwrap linked in uses the system's code on Windows 7 and the emulation on XP, with no separate build for either. The cost on a modern OS is a cached pointer load and a jump.

To see a list of the functions that this library shims/spoofs, see [FUNCTION_LIST.md](FUNCTION_LIST.md).

## Credits

[YY-Thunks](https://github.com/Chuyu-Team/YY-Thunks) (MIT) covers much of the same ground and has had
far longer to find the edge cases. It was consulted while writing several of these functions, and is
cited in the source comments where it mattered — both where it had already been caught out by
something, and where winwrap deliberately went a different way.

No YY-Thunks code is used here, so this is an acknowledgement of a useful reference rather than a
license obligation; winwrap remains BSD-3-Clause.
