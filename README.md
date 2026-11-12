# winwrap lib

Win32 Legacy support library, designed for Chromium/Electron usage.

This is similar to [Supermium's](https://www.supermium.net/) [`progwrp.dll`](https://github.com/win32ss/supermium/blob/bf7e71c95f9a5a9a3ae5206ef8274633255de50b/.github/README.md#this-supermium-component-receives-enhancements-and-improvements-from-time-to-time-primarily-for-operating-systems-prior-to-windows-vista).

It was originally created as a component of [Electron Legacy](https://github.com/Alex313031/electron-legacy#synopsis) to allow it to run on Windows XP/Vista (7/8/8.1 support out of the box).

It is integrated into Chromium's [GN](https://gn.googlesource.com/gn/) build systen, although support for other build systems so that one may integrate it into their own project is planned.

## About
It can be compiled as a static lib `winwrap.lib` or shared library `winlib.dll`, the main thing is that it *must* be linked ahead of any other libraries or default libs, to make sure that the binary in question imports OUR functions, instead of the (missing in XP/Vista's case) system DLL functions.

To see a list of the functions that this library shims/spoofs, see [FUNCTION_LIST.md](FUNCTION_LIST.md).
