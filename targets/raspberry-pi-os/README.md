# targets/raspberry-pi-os — Raspberry Pi OS desktop target (SDL2)

Runs gea apps as native Linux desktop binaries rendering into an SDL2 window.
Developed and verified on a Raspberry Pi 5 (Raspberry Pi OS Bookworm desktop,
labwc/Wayland; SDL picks X11 via Xwayland there). Nothing is Pi-hardware-
specific — this is plain SDL2 on Linux — but the target is named for the OS
we develop and test on; other distros can become sibling targets later.

## Build & run

Apps opt in with `"raspberry-pi-os": true` under `gea.targets` in their
`package.json` and depend on `@geastack/linux` and `@geastack/cli` (0.1.104 or
later). From the app project:

```bash
sudo apt install build-essential pkg-config libsdl2-dev libcurl4-openssl-dev libfreetype-dev   # once
npx gea build --target raspberry-pi-os
./dist/raspberry-pi-os/<app-id>/<app-id>
```

`gea build` runs `build-raspberry-pi-os.sh <app-id>` from the installed
package in the app folder; the script needs the CLI's environment
(`GEA_CLI_BIN`) and exits when started on its own. Output lands under the app:
the executable at `dist/raspberry-pi-os/<app-id>/<app-id>`, objects in
`dist/raspberry-pi-os/<app-id>/build/`, generated C++ in
`dist/raspberry-pi-os/.generated/<app-id>/`. `GEA_LINUX_OUTPUT_DIR` moves the
tree. Cross-building for AArch64 is described in the repository README.

Environment knobs (all optional):

| Variable | Default | Meaning |
| --- | --- | --- |
| `GEA_RPIOS_WIDTH` / `GEA_RPIOS_HEIGHT` | 410 / 502 | logical canvas size (amoled-2.06 shape) |
| `GEA_RPIOS_SCALE` | 2 | integer window scale (window = canvas × scale) |
| `GEA_RPIOS_DPR` | 2.0 | device pixel ratio passed to `Application::init` |
| `GEA_RPIOS_FONT_VIEWPORT_WIDTH/HEIGHT`, `GEA_RPIOS_FONT_DEVICE_PIXEL_RATIO` | 410 / 502 / 2.0 | build-time font rasterization metrics |
| `GEA_RPIOS_STORAGE_DIR` | `$XDG_DATA_HOME/gea/<app-id>` (falls back to `~/.local/share/gea/<app-id>`) | where `localStorage` + settings persist |
| `GEA_LOOP_PERF` | off | per-second `[rpios.loop]` fps/phase log |

## Headless PNG capture

Native builds with `libpng-dev` installed support prerendering without X11,
Wayland, or an SDL window. Set `GEA_LINUX_PRERENDER_RGBA=1` when building
to preserve 8-bit color channels in captures. The executable accepts the output path, physical
width and height, DPR, FPS, and animation timestamp in milliseconds:

```bash
./dist/raspberry-pi-os/<app-id>/<app-id> --gea-prerender frame.png 1280 720 1 60 0
```

The `--gea-prerender-worker` mode accepts newline-delimited JSON requests on
stdin. Each request supplies `input` and `out` paths and may specify `width`,
`height`, `dpr`, and `reusePage`. The app can read the current input path
from `GEA_PRERENDER_INPUT`. Set `reusePage: true` to capture an already
mounted matching input and viewport. Replies include `ok`, `ms`, and
`reusedPage` on success, or `ok: false` and the error on failure.

## Window & input

- The window is **resizable** — drag the frame and the framework viewport
  reflows live (`setViewportMetrics` + full-tree repaint). **F11** toggles
  borderless fullscreen. **Esc** is a normal app key; closing the window quits. The title bar
  shows the app's display name. Known limitation: `<canvas>` apps whose
  drawing surface no longer matches the resized viewport fall back to
  per-rect `streamRect` flushes, each of which presents, so frame rate drops
  sharply after resizing such apps (tree/DOM apps reflow fine).
- **Mouse**: left button drives the touch pipeline (pointer 0) — tap, drag,
  momentum scroll. The **wheel** scrolls the first scrollable container
  (48 px per notch) and is also delivered as `rotary` detents for knob-aware
  apps.
- **Touchscreens**: SDL finger events feed the multi-pointer touch path with
  stable pointer ids (up to 10 fingers), so a Pi touch display gets
  multi-touch (SDL's touch→mouse synthesis is disabled to prevent double
  injection).
- **Keyboard**: keys are queued as web keyCodes and dispatched framework-side
  (focused `<input>` first, then the document handler). Printable text goes
  straight into the focused `<input>`'s value (same path as the on-screen
  virtual keyboard, which also still works via mouse/touch); Backspace edits,
  Enter submits.
- **Lifecycle**: minimizing/hiding the window pauses the frame loop (events
  still pump; input dispatch gated off); restoring resumes and re-presents.
  Exposed windows re-present the last frame.

## Platform services

- **localStorage / settings** (`main/rpios_storage.cpp`) — file-backed
  `StorageService`: the facade's blob persists to
  `<storage-dir>/localstorage.bin`, settings strings to `settings.bin`,
  both written atomically (tmp + rename). The main loop mirrors
  `runtime.cpp`'s plumbing: `Storage.load()` at boot, `flushPending()` per
  frame.
- **fetch / HTTP(S)** (`main/rpios_network.cpp`) — networking via
  libcurl, injected through the desktop fetch seam (strong override of the
  weak `test_record_request`/`test_canned_response` hooks in core's
  `host/fetch.cpp`, the same pattern the Android target uses). Sync and
  async (`fetchAsync`) both work; async runs on detached worker threads.
  Redirects followed, gzip auto-decoded, `init.timeout_ms` honored
  (default 30 s).
- **Wall-clock** — `Date`/`Clock.epochMs()` read the system clock
  (`system_clock`/`gettimeofday`); no platform code is needed.

## Architecture

Same shape as the geaos target (Linux-phone fbdev), minus the phone/watch
hardware paths:

- `main/rpios_display.cpp` — the `gea::platform::display::Display` backend.
  The framework Canvas is bound to an in-process RGB565 buffer;
  `flush`/`flushRects` upload dirty rects into an `SDL_PIXELFORMAT_RGB565`
  streaming texture and present (vsynced when the renderer supports it; the
  main loop skips its pacing sleep on presented frames to avoid double-pacing).
  `rpios_display_resize` rebuilds the framebuffer/texture on window resize.
- `main/rpios_main.cpp` — owns the frame loop: pump SDL events → key/rotary
  drains → `Application::frame` → `Storage.flushPending` → `Tree::refresh`.
  Because this target excludes core's `runtime.cpp` (it runs its own loop),
  the per-frame drains and storage plumbing that runtime.cpp normally does
  are replicated here.
- `main/rpios_timers.cpp` — `gea_embedded_now_ms` + the full `FrameScheduler`
  (POSIX monotonic clock, mutex-guarded ring queue).
- `main/rpios_sensors.cpp` — Accelerometer/Memory stubs + the Touchscreen
  observer/cache (no reader thread; SDL injects from the main loop).
- `main/rpios_storage.cpp`, `rpios_network.cpp` — the storage and network backends (see Platform
  services above).
- `main/rpios_audio.cpp`, `rpios_memory.cpp`, `rpios_apps.c`,
  `rpios_app_platform.cpp` — audio stub (silent, same surface as macos/geaos),
  desktop allocator, and single-app launcher recording (the AppManager
  platform is installed at boot with the built app's id).

Framework sources and include roots come from the shared manifest
`core/gea_sources.sh` (resolved via `node_modules/@geastack/core`). Like the
macos build, the target filters out `host/camera.cpp` (no platform camera —
a `<camera>` element renders nothing), `runtime.cpp` (this target runs its
own frame loop), and the framework services (no mirror/diagnostics server
yet). BLE reports no driver; audio is stubbed silent; the IMU returns zeros.

## Keyboard input

The native keyboard requires SDL2 >=2.0.22 (extended IME events); older headers
refuse compilation. Physical SDL key events do not pass through the
metadata-free integer hardware queue. The target owns a bounded 256-event queue
of native key/text packets and generation-aware NodeHandle focus leases. SDL
keydown and keyup carry key, physical code, legacy keyCode, modifiers, repeat
and text-editing composition state to the shared Tree dispatcher. Only the
current SDL window is admitted; window focus loss/hidden/minimize clears pending
composition/events. Tree focus transitions revoke pending events, including
A→B→A, and removal/reuse/disable or focus supersession refuses delivery.
Controls disabled directly or through a containing fieldset refuse key and text
input. Ordinary Tree realm reset refreshes the document focus subscriptions.
Process termination retires producer admission and subscriptions before
application cleanup. The integer queue remains separately ordered compatibility
for non-SDL hardware, without physical-event duplication.

Escape dispatches its normal key event and does not terminate the app. SDL_QUIT
and the owned window's close event still exit. F11 is a once/nonrepeat native
default only after dispatch and no prevention. Backspace and SDL_TEXTINPUT
mutate only the still-current admitted active input, once after key/default
guards; text is SDL UTF-8 and backspace removes one code point (not a grapheme
editor). Real user edits mark the existing ControlCommit authority; Return
commits once without repeat/IME/modifier bypass. No synthetic extra keydown is
emitted. The mapping supports named navigation/control/F1–F12, Latin letters and
unshifted ASCII; unsupported layout-specific shifted punctuation returns
Unidentified rather than inventing a US key. Physical codes are explicit
supported SDL scancodes; normal text comes from SDL_TEXTINPUT, including
extended IME editing state.

### Keyboard tests

`test/keyboard.test.cpp` exercises the keyboard producer with real SDL structs,
the Tree/Document dispatcher, the integer queue and the key drain extracted from
the current `main/rpios_main.cpp`. It covers key metadata, keyup,
modifiers/repeat/IME, window and focus changes, stale or disabled targets,
prevented defaults and the legacy queue. `test/keyboard-detach.test.cpp` covers
focus detach/refocus cancellation. The tests need no SDL initialization, window,
display or OS input.

Set `GEA_CORE_ROOT` to a Core checkout and `GEA_SDL_INCLUDE_ROOT` /
`GEA_SDL_ARCH_INCLUDE_ROOT` to the SDL and architecture include directories,
then run from this target:

```bash
bash test/run-keyboard.sh            # build and run
bash test/run-keyboard.sh --build-only
bash test/run-keyboard-detach.sh --build-only
```
