# Sailfish OS target

Builds a Gea JSX/CSS app into a local Sailfish OS RPM. The host runs Vite and
geatsc; `sfdk` compiles the staged C/C++ sources for Sailfish OS 5.1.0.11.

Requirements: Node.js 20.19+, Sailfish SDK with the selected build target, a
checkout of `geastack/core` and the app to build. The default core checkout is
`../core` relative to this Linux repository; pass `--core-repo` (Bash) or
`-CoreRepository` (PowerShell) for another location. The examples below build
an app from `geastack/examples`:

```sh
git clone https://github.com/geastack/linux.git
git clone https://github.com/geastack/core.git
git clone https://github.com/geastack/examples.git
(cd examples && npm install)
cd linux && npm ci
```

Code generation uses this repository's npm core, compiler, and plugin. The
source manifest and native framework come from the core checkout. Preparation
compares the `version` in this repository's
`node_modules/@geastack/core/package.json` with the checkout's
`packages/core/package.json` and fails before code generation if they differ;
there is no flag or variable that skips the check. The lockfile pins the
published `@geastack/core` that matches the core repository's `main` branch.
For another core checkout, install the npm core of the same version from this
Linux repository:

```sh
npm install --no-save @geastack/core@"$(node -p "require('../core/packages/core/package.json').version")"
```

Local core edits are still included in the build.

From the Linux repository on Linux:

```sh
./targets/sailfish-os/build-sailfish-os.sh ../examples/apps/tic-tac-toe --arch i486
```

The script finds `sfdk` on `PATH`. Pass `--sfdk /path/to/SailfishOS/bin/sfdk`
if it is elsewhere. `--prepare-only` runs code generation and stages the
portable project without invoking `sfdk`.

The app's `gea.icons` must have a `512`, `256` or `128` entry (the first one
present is used) pointing at an existing image; preparation fails without
one. It generates
86, 108, 128, and 172 pixel PNGs for Harbour. For sandbox permissions, set
`gea.sailfish.organizationName`, `applicationName`, and `permissions` in the
app's `package.json`. The default identity is `org.geastack` and the app ID
with hyphens replaced by underscores; permissions default to an empty list.
Set only permissions the app actually needs, such as `Audio` for sound and
`Internet` for `fetch` or other network requests. For an app using both, set
`"permissions": ["Audio", "Internet"]`. An empty list does not allow networking;
permissions are not inferred from app source code.

From the Linux repository on Windows:

```powershell
./targets/sailfish-os/build-sailfish-os.ps1 -AppDirectory ../examples/apps/tic-tac-toe -Architecture i486
```

Use `-SdkRoot` when Sailfish SDK is installed outside `C:\SailfishOS`.

The RPM is written to
`targets/sailfish-os/build/<app-id>-<architecture>/project/RPMS/`.
Use `-PrepareOnly` in PowerShell for code generation without invoking `sfdk`.
Before submitting a device package to Harbour, run `sfdk check` on its RPM.
The `i486` build is for the emulator; build for the architecture of the target
device before publishing.

The Windows script invokes the installed `sfdk.exe` with session handling off.
The SDK build engine must be running. Run the script from PowerShell: invoking
`sfdk.exe` from an MSYS2 shell can fail to identify the SDK's Docker build
engine.

The Sailfish target owns its platform sources in `main/` and `include/`.
Its SDL2 backend starts fullscreen with a scale of 1. Platform symbols and
configuration use the `sailfish_` and `GEA_SAILFISH_` prefixes. Storage uses
the organization and application names from the app's Sailjail identity.

Runtime overrides are `GEA_SAILFISH_WIDTH`, `GEA_SAILFISH_HEIGHT`,
`GEA_SAILFISH_SCALE`, `GEA_SAILFISH_DPR`, and `GEA_SAILFISH_STORAGE_DIR`.

Scale and DPR both default to 1: one CSS pixel is one canvas pixel and one SDL
window coordinate unit. The initial canvas is 410x502; resize events update it
to the available window dimensions divided by scale. DPR controls canvas pixels
per CSS pixel, while scale controls presentation magnification. Font generation
uses the same initial 410x502 viewport and the app's DPR; runtime TTF fonts adapt
to the current viewport and DPR after resize. CMake defaults to a Release build.

Set `gea.sailfish.devicePixelRatio` in the app's `package.json` to change its
CSS density (default 1). For example, `"devicePixelRatio": 2` renders one CSS
pixel as two canvas pixels while keeping the SDL window scale at 1. A 720x1527
window then provides a 360x763.5 CSS viewport. The same setting is used for font
generation and the executable's default DPR; `GEA_SAILFISH_DPR` overrides it at
runtime. This is useful for phone layouts designed around a 360 CSS pixel width.

Fonts declared with `@font-face` are embedded as TTF files and rasterized at
runtime. Character coverage depends on the core checkout's runtime TTF
rasterizer. Turkish text requires a core version that includes Latin Extended-A;
the ASCII-only rasterizer does not provide those glyphs.

The target has been run on the Sailfish OS 5.1.0.11 emulator (i486); it has not
been tested on a physical device.
