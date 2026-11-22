# GeaStack Linux

Native Linux desktop target support for GeaStack.

This repo contains the targets that let Gea applications run as native Linux
desktop apps. Apps render through the shared gea framework raster pipeline
(RGB565 canvas → dirty-rect SDL2 texture upload) into a resizable desktop
window, with mouse, touchscreen (multi-touch), keyboard, and scroll-wheel
input mapped onto the framework input pipelines, plus persistent
`localStorage` (file-backed) and HTTP(S) `fetch` (libcurl). The
targets include Raspberry Pi OS on the Raspberry Pi 5 and Sailfish OS.

## What Is Here

| Path | Purpose |
| --- | --- |
| `targets/raspberry-pi-os` | Raspberry Pi OS (Bookworm+ desktop, labwc/Wayland) SDL2 target: platform sources, build script, and target docs. |
| `targets/sailfish-os` | Sailfish OS 5.1 target: its own platform sources, host code generation, portable CMake project, and i486 RPM packaging. |

## Quick Start

The app depends on `@geastack/linux` and on the `gea` CLI (`@geastack/cli`
0.1.104 or later, which is where the `raspberry-pi-os` target arrived; the
CLI is a separate package and `npx gea` does not fetch it on demand), and
declares the target in the `gea` block of its `package.json`:

```sh
npm install @geastack/linux
npm install --save-dev @geastack/cli
```

```json
"gea": {
  "id": "my-app",
  "entry": "index.tsx",
  "targets": { "raspberry-pi-os": true }
}
```

From the app directory, build the real native SDL target:

```sh
npx gea build --target raspberry-pi-os
```

This command only builds; it never launches or deploys. The executable is
`dist/raspberry-pi-os/<app-id>/<app-id>` under the app. It uses the shared
compiler and raster renderer, with no browser fallback. The build script
(`targets/raspberry-pi-os/build-raspberry-pi-os.sh`) is started by the CLI and
refuses to run on its own.

For an AArch64 cross build, provide `CC`/`CXX` as Clang with `lld` installed
(the cross link uses `-fuse-ld=lld`), an absolute `GEA_LINUX_SYSROOT` with GCC
14 target libraries (`usr/lib/gcc/aarch64-linux-gnu/14`), staged development
roots `GEA_LINUX_SDL_ROOT`, `GEA_LINUX_CURL_ROOT` and `GEA_LINUX_FREETYPE_ROOT`,
and the matching runtime libraries `GEA_LINUX_SDL_LIBRARY`,
`GEA_LINUX_CURL_LIBRARY` and `GEA_LINUX_FREETYPE_LIBRARY`. All of these are
required. `GEA_LINUX_LIBRARY_DIR` (optional, absolute) adds a directory for
the linker to search and to resolve transitive libraries from. These inputs
are explicit; the target does not install packages or modify a sysroot.
`GEA_BUILD_JOBS` bounds compilation to 1–16 jobs (default 2). `GEA_LINUX_ROOT`
selects a local package checkout when developing the CLI/target together.

Click and drag with the mouse (it acts as the touch finger); the wheel
scrolls; the keyboard types into focused inputs; F11 toggles fullscreen; Esc
is delivered to the app as a normal key; closing the window quits. See
[targets/raspberry-pi-os/README.md](targets/raspberry-pi-os/README.md) for
window size/scale/DPR/storage knobs and target architecture.

For Sailfish OS, use the standalone Bash or PowerShell build script described in
[targets/sailfish-os/README.md](targets/sailfish-os/README.md).

## Dependencies

- `build-essential`, `pkg-config`, `libsdl2-dev` (SDL 2.0.22 or later),
  `libcurl4-openssl-dev`, `libfreetype-dev` (Debian/Raspberry Pi OS
  packages). The build stops if `sdl2-config` or the `freetype2` pkg-config
  module is missing.
- Node.js 22 or later, which `@geastack/cli` requires. Debian Bookworm's
  `nodejs` package is older; install Node 22 from nodejs.org, NodeSource or a
  version manager.
- The rest of the stack from npm: `@geastack/linux` brings in `@geastack/core`,
  `@geastack/compiler` and `@geastack/geatsc-plugin-gea`, which carry the
  codegen toolchain. `@geastack/cli` is installed separately (above).

Audio is not implemented on Raspberry Pi OS: `rpios_audio.cpp` is a silent
stub, so sound APIs run without output.

## How This Fits The Stack

Linux targets consume compiled Gea apps and present the framework's software
raster through a desktop window system. This repo is the Linux platform
adapter layer. It owns the SDL2 display/input glue, the Linux storage/network
backends, and Linux-specific target build scripts.

## Maintenance Notes

- Framework sources and include roots come from the shared manifest
  `core/gea_sources.sh` — never hardcode framework source lists in target
  build scripts.
- Keep the target platform files thin: platform glue implementing the
  `gea::platform::*` / `StorageService` / fetch-seam surfaces. Behavior
  belongs in the shared framework packages.
- This target excludes core's `runtime.cpp` and runs its own frame loop, so
  the per-frame plumbing runtime.cpp normally does (key/rotary input drains,
  `Storage.load()`/`flushPending()`) is replicated in `rpios_main.cpp` — if
  runtime.cpp grows new per-frame duties, mirror them there.
- When adding target behavior, update the target README in the same change.

## License

Apache-2.0 (see `LICENSE`). You can ship closed-source products
built on it. The only GeaStack code under a different license is
the embedded board support (`targets` and `@geastack/chips`, GPL-3.0-only):
shipping closed-source firmware through those needs a commercial license.
Contact [contact@geastack.com](mailto:contact@geastack.com) for commercial terms, support and hosted builds.
