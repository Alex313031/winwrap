## List of Winwrap consumers

A list of programs that have been successfully ported to Windows XP/Vista using this library.

For how to use winwrap in your own program, see [USAGE.md](USAGE.md), and if you are specifically
looking for extra functions winwrap consumers can use, see [Extra functions](USAGE.md#extra-functions).

| Repo | Description |
|------|-------------|
| [Electron Legacy](https://github.com/Alex313031/electron-legacy#readme) | Build scripts and patches for up-to-date [Electron](https://electronjs.org/) on legacy Windows. One binary runs on everything from XP to 11. winwrap supplies what Chromium, Node and the CRT expect of Vista and later, including tray icons and message boxes on XP |
| [Node Legacy](https://github.com/Alex313031/node-legacy#readme) | A build script and patches for up-to-date [Node.js](https://nodejs.org) on legacy Windows, XP to 11 from one binary. winwrap covers the Vista-and-later functions libuv, SQLite and the C++ runtime call |
| [Sawbuck-ng](https://github.com/Alex313031/sawbuck-ng#readme) | A maintained fork of Sawbuck, Google's log viewer and ETW controller for Chromium logging, built with Visual Studio 2022. Links winwrap through `winwrap.gyp`, the MSVC-only build with no clang-cl. |
