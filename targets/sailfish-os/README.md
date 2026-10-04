# Sailfish OS target

Builds a Gea JSX/CSS app into a local Sailfish OS RPM. The host runs Vite and
geatsc; `sfdk` compiles the staged C/C++ sources for Sailfish OS 5.1.0.11.

Supported `sfdk` targets are `SailfishOS-5.1.0.11-i486` (emulator),
`SailfishOS-5.1.0.11-armv7hl`, and `SailfishOS-5.1.0.11-aarch64`. Pass the
matching architecture to the build script. Publish the RPM that matches the
device; the i486 package is only for the emulator.

Requirements: Node.js 20.19+, `npm ci` in the app (so the locked `@geastack/*`
packages are installed), and the Sailfish SDK with the selected build target.
A separate `geastack/core` checkout is not required. Pass `--core-repo` (Bash)
or `-CoreRepository` (PowerShell) to build the framework from a source checkout
instead. That checkout's `core`, `host`, `engine`, `elements`, and `geaos`
package versions must match the app's installed packages; the error names the
package that is missing or different.

The app's `node_modules/@geastack/*` packages are used for code generation and
for the staged framework. If the app has not installed one of those packages,
preparation falls back to this Linux repository's `node_modules` so target
development still works. Do not patch those installed packages.

From the Linux repository, or from an app that depends on `@geastack/linux`:

```sh
./targets/sailfish-os/build-sailfish-os.sh ../examples/apps/tic-tac-toe --arch aarch64
```

When `@geastack/linux` is installed in the app, the same script lives at
`node_modules/@geastack/linux/targets/sailfish-os/build-sailfish-os.sh`.

The script finds `sfdk` on `PATH`. Pass `--sfdk /path/to/SailfishOS/bin/sfdk`
if it is elsewhere. `--prepare-only` runs code generation and stages the
portable project without invoking `sfdk`. `--clean` removes the staged RPM and
CMake outputs, including object files, and then builds.

Generated sources and the staged project are written under the app, at
`<app>/.gea-sailfish/`. Add that directory to the app's ignore rules. The RPM
is `<app>/.gea-sailfish/build/<app-id>-<architecture>/project/RPMS/`.

Before each `sfdk build`, empty or non-ELF `.o` files left by an interrupted
SDK engine are deleted. `sfdk` would otherwise treat a zero-byte object as up
to date and fail the link with undefined references. If `sfdk` fails, the
script exits with `sfdk`'s status and prints the last error line from the log.

From the Linux repository on Windows:

```powershell
./targets/sailfish-os/build-sailfish-os.ps1 -AppDirectory ../examples/apps/tic-tac-toe -Architecture aarch64
```

Use `-SdkRoot` when Sailfish SDK is installed outside `C:\SailfishOS`.
`-PrepareOnly` stops after code generation. `-Clean` clears the staged build
before compiling.

Install the RPM on a device with `sfdk` or by copying it to the phone and using
`pkcon install-local`. The SDK build engine must be running for the Windows
script. On this host, invoking `sfdk.exe` inside MSYS2 fails to identify its
Docker engine, while the PowerShell invocation works.

The app's `gea.icons` entry supplies the icon source. Preparation generates
86, 108, 128, and 172 pixel PNGs for Harbour. For sandbox permissions, set
`gea.sailfish.organizationName`, `applicationName`, and `permissions` in the
app's `package.json`. The default identity is `org.geastack` and the app ID
with hyphens replaced by underscores; permissions default to an empty list.
Set only permissions the app actually needs, such as `Audio` for sound and
`Internet` for `fetch` or other network requests. For an app using both, set
`"permissions": ["Audio", "Internet"]`. An empty list does not allow networking;
permissions are not inferred from app source code.

## App native sources

Sailfish-only C and C++ belong in the app and are declared in `package.json`.
Desktop, Apple, and Windows sources are not picked up automatically. The
system-keyboard bridge is part of this target, not an app source.

```json
{
  "gea": {
    "sailfish": {
      "nativeSources": ["native/host.cpp", "native/host.h"],
      "includePaths": ["native/vendor"],
      "libraries": ["openssl"]
    }
  }
}
```

`nativeSources` lists app-relative C, C++, and header files. Each compiled
file's directory is added to the include path, so `native/host.cpp` can
`#include "host.h"` when the header sits next to it. `includePaths` copies
extra header or vendor directories into the staged project. `libraries` are
pkg-config module names: each one is linked and added to the RPM
`BuildRequires` as `pkgconfig(<name>)`. Runtime library dependencies are the
ones rpm records from the linked binary.

## Text input

On a Sailfish device the system keyboard is Maliit. Focusing an `<input>`
shows it; blurring, or dismissing the keyboard, hides it. `type="password"`
is sent as Maliit's `hiddenText` flag with prediction turned off. Commit text,
Backspace, and Enter use the same helpers as a hardware keyboard.

`libmaliit-glib` is not in Harbour's allowed library list
([Allowed APIs](https://docs.sailfishos.org/Develop/Apps/Harbour/Allowed_APIs/)).
The bridge uses GIO (`libgio-2.0.so.0`, `libglib-2.0.so.0`, `libgobject-2.0.so.0`),
which is allowed. It reads the server address from the session bus
(`org.maliit.server`, `/org/maliit/server/address`, property `address`) and
then calls `com.meego.inputmethod.uiserver1` on that peer connection. The Gea
on-screen keyboard is not the Sailfish default
(`GEA_EMBEDDED_ENABLE_VIRTUAL_KEYBOARD=0`). The emulator and a hardware
keyboard still deliver `SDL_TEXTINPUT`. If Maliit is absent, that is logged
and SDL input continues.

Text is inserted as UTF-8. Backspace deletes the last Unicode code point
(trailing `10xxxxxx` bytes, then the lead byte), so `ç`, `ğ`, `ı`, `İ`, `ö`,
`ş`, and `ü` round-trip. A grapheme made of several code points is not
collapsed. ASCII input is unchanged.

When the keyboard shrinks the window, the focused input is scrolled into view
after the new layout.

## Pointer coordinates

Hit testing uses canvas pixels. One function, `sailfish_window_to_canvas()`,
converts window points to those pixels for touch, mouse, and the wheel pointer:

- Window points are `SDL_GetWindowSize`. Finger events are normalized across
  that window and converted to window points first.
- Drawable pixels are `SDL_GetRendererOutputSize`. The window is created
  without `SDL_WINDOW_ALLOW_HIGHDPI`, so the drawable matches the window.
  Multiplying by the drawable size as well was the DPR 2 miss: the canvas
  already uses `gea.sailfish.devicePixelRatio`, and the high-DPI flag scaled
  the touch a second time.
- CSS pixels are canvas pixels divided by that DPR. The framework applies DPR.
  Pointer code does not divide by it again.

The same mapping is used after resize, including a keyboard resize. The
startup log prints window, drawable, and canvas sizes.

## Runtime

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

## Device check

Manual smoke list for an aarch64 phone (Sailfish OS 5.1.0.11 or newer):

1. Launch the installed RPM.
2. Tap corner buttons, inputs, and a scrolling view at DPR 1 and DPR 2.
3. Drag and scroll; confirm the control under the finger is the one that moves.
4. Focus a text field. The Sailfish keyboard opens and the field stays visible
   after the window shrinks.
5. Type `ç ğ ı İ ö ş ü`, delete each character with Backspace, and press Enter.
6. Repeat in a `type="password"` field (no prediction, hidden text).
7. Dismiss the keyboard and tap the same controls again. Hits stay aligned.
8. Resize or rotate and repeat one tap and one text entry.
9. Before a Harbour upload, run `sfdk check` on the RPM. The keyboard must not
   add a `libmaliit` dependency; GIO is the allowed library.

Recorded on a Redmi Note 8, Sailfish OS 5.1.0.11, aarch64, before this input
and coordinate work landed: an aarch64 RPM installed and the app launched;
touch missed at `devicePixelRatio` 2 until `SDL_WINDOW_ALLOW_HIGHDPI` was
removed, and worked after that; a temporary Maliit connection opened the
keyboard and the window shrank. Character entry, Backspace, password fields,
Enter, hit testing after the keyboard closed, and `sfdk check` were not
verified on device. This tree has not been installed on a phone since those
fixes.

## Known limits outside this package

- Runtime TTF coverage, including Turkish letters and symbol fallback, belongs
  to `@geastack/engine`. Do not copy a fixed glyph list into this target.
- `declare function` host bindings that throw `throwReferenceError` in
  generated C++ belong to `@geastack/compiler`. Declaring `nativeSources` does
  not make an undeclared global callable.
