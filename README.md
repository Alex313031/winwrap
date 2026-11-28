# winwrap lib

Win32 Legacy support library, designed for XP/Vista.

This is similar to [Supermium's](https://www.supermium.net/) [`progwrp.dll`](https://github.com/win32ss/supermium/blob/bf7e71c95f9a5a9a3ae5206ef8274633255de50b/.github/README.md#this-supermium-component-receives-enhancements-and-improvements-from-time-to-time-primarily-for-operating-systems-prior-to-windows-vista).

It was originally created as a component of [Electron Legacy](https://github.com/Alex313031/electron-legacy#synopsis) to allow it to run on Windows XP/Vista (7/8/8.1 support out of the box),
though it has now expanded to be a general library to help with building/porting apps for old Windows.

## Building
For building winwrap from source, see [BUILDING.md](./docs/BUILDING.md).

### Usage
For integrating into your own project, see [USAGE.md](./docs/USAGE.md).

For a list of projects that currently use winwrap, see [CONSUMERS.md](./docs/CONSUMERS.md).

## About
To see a list of the functions that this library shims/spoofs, see [FUNCTION_LIST.md](docs/FUNCTION_LIST.md).
Most of them are complete; the ones that cannot be, and what they do instead, are in
[INCOMPLETE_FUNCS.md](docs/INCOMPLETE_FUNCS.md).

### Credits
[YY-Thunks](https://github.com/Chuyu-Team/YY-Thunks) (MIT) covers much of the same ground and has had
far longer to find the edge cases. It was consulted while writing several of these functions, and is
cited in the source comments where it mattered: both where it had already been caught out by
something, and where winwrap deliberately went a different way.

No YY-Thunks code is used here, so this is an acknowledgement of a useful reference rather than a
license obligation; winwrap remains BSD-3-Clause.
