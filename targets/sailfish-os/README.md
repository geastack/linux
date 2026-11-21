# Sailfish OS target

Builds a Gea JSX/CSS app into a local Sailfish OS RPM. The host runs Vite and
geatsc; `sfdk` compiles the staged C/C++ sources for Sailfish OS 5.1.0.11.

Requirements: Node.js 20.19+, `npm install` in the app (with `@geastack/linux`
as a dependency), and Sailfish SDK with the selected build target.

The GeaStack packages (core, compiler, plugin and the native framework
packages) are resolved from the app the way Node resolves them, with this
repository's own `node_modules` as a fallback. A `geastack/core` checkout is
optional: pass `--core-repo` (Bash) or `-CoreRepository` (PowerShell) to build
the native framework from it instead. Its version must then match the installed
`@geastack/core`, and local core edits are included in the build.

Build output goes to `<app>/.gea-sailfish/`, never into the installed package;
add that directory to the app's `.gitignore`.

From the app directory on Linux:

```sh
./node_modules/@geastack/linux/targets/sailfish-os/build-sailfish-os.sh . --arch aarch64
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

`"sandboxing": false` in `gea.sailfish` writes `Sandboxing=Disabled` to the
`[X-Sailjail]` section, for apps that must run other programs or see the whole
file system, such as a terminal. Harbour does not accept unsandboxed apps.

App-owned native code is listed in `gea.nativeSources` (app-relative `.c`,
`.cc`, `.cpp` or `.cxx` files). They are copied into the project under
`app_native/` with their relative paths and compiled with the app. Link
dependencies go in `gea.sailfish`:

| Key | Meaning |
| --- | --- |
| `nativeIncludeDirectories` | App-relative directories added to the include path |
| `nativeSystemLibraries` | Library names linked with `-l`, such as `util` for `openpty` |
| `pkgConfigModules` | pkg-config modules, such as `maliit-glib`; each one is also a `BuildRequires` |

Paths must stay inside the app directory.

From the Linux repository on Windows:

```powershell
./targets/sailfish-os/build-sailfish-os.ps1 -AppDirectory ../examples/apps/tic-tac-toe -Architecture i486
```

Use `-SdkRoot` when Sailfish SDK is installed outside `C:\SailfishOS`.

The RPM is written to
`<app>/.gea-sailfish/build/<app-id>-<architecture>/project/RPMS/`.
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
`GEA_SAILFISH_SCALE`, `GEA_SAILFISH_DPR`, `GEA_SAILFISH_HIGHDPI` (`0` creates
the window without `SDL_WINDOW_ALLOW_HIGHDPI`), and `GEA_SAILFISH_STORAGE_DIR`.

Hardware keys and text use the same SDL keyboard producer as the Raspberry Pi
OS target (`rpios_keyboard.h`). Key events carry `key`, `code`, `keyCode` and
the Shift/Control/Alt/Meta flags, and Escape is delivered to the app like any
other key; the app quits on the window close request or SIGTERM. Text from
`SDL_TEXTINPUT` is inserted into the focused input as UTF-8, and Backspace
removes one whole character. F11 toggles fullscreen.

The Sailfish on-screen keyboard (Maliit, through `maliit-glib`) opens while an
input is focused and closes when focus leaves it; `type="password"` inputs ask
for hidden text. Committed text and special keys (Backspace, Enter, Tab,
arrows, Home/End, Delete, Escape) are delivered as SDL events, so they reach the
app exactly like a hardware keyboard. Word prediction and auto-correction are
turned off because the target does not draw pre-edit text. When the keyboard
opens, the compositor shrinks the window and the app receives a resize.

Scale defaults to 1. The canvas
covers the window's drawable in physical pixels divided by scale, at startup and
after every resize, so a Wayland output scale does not stretch a low-resolution
canvas (which made text blocky). The texture is presented with nearest
filtering and integer scaling only, so one canvas pixel is exactly one panel
pixel at scale 1. DPR controls canvas pixels per CSS pixel, while scale controls
presentation magnification. CMake defaults to a Release build.

`gea.sailfish.devicePixelRatio` in the app's `package.json` sets the CSS
density. It defaults to `"auto"`. On ARM builds (`aarch64`, `armv7hl`) auto is
pixel-perfect: the executable takes the largest integer DPR that keeps the
panel's short side at least 360 CSS pixels, so every CSS pixel is a whole NxN
block of panel pixels and edges never fall between pixels. The DPR is derived
from the panel, not the window, so it does not change when the on-screen
keyboard shrinks the window. On the current Sailfish phones this gives:

| Device | Panel | Auto DPR | CSS viewport |
|---|---|---|---|
| Jolla C2 | 720x1600 | 2 | 360x800 |
| Xperia XA2, XA2 Plus, XA2 Ultra | 1080x1920 (XA2 Plus 1080x2160) | 3 | 360x640 (360x720) |
| Xperia 10, 10 Plus, 10 II, 10 III, 10 IV, 10 V | 1080x2520 | 3 | 360x840 |
| Jolla Phone (2026) | 1080x2340 | 3 | 360x780 |

On the i486 emulator, auto is 1 (one CSS pixel per canvas pixel). A number
(0.5 to 8) is used as given on every architecture and multiplied by the Wayland
output scale (physical pixels per window point). `GEA_SAILFISH_DPR` (a number or
`auto`) overrides the setting at runtime; the startup log shows the chosen DPR
and CSS viewport. A non-integer DPR is not pixel-perfect: CSS edges can land
between panel pixels.

Font generation uses the initial 410x502 viewport and the app's DPR; an auto
ARM build generates fonts for DPR 2 and 3. Runtime TTF fonts adapt to the
current viewport and DPR after resize.

Fonts declared with `@font-face` are embedded as TTF files and rasterized at
runtime. Character coverage depends on the core checkout's runtime TTF
rasterizer. Turkish text requires a core version that includes Latin Extended-A;
the ASCII-only rasterizer does not provide those glyphs.

The target has been run on the Sailfish OS 5.1.0.11 emulator (i486); it has not
been tested on a physical device.
