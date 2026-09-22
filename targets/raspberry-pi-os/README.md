# targets/raspberry-pi-os — Raspberry Pi OS desktop target (SDL2)

Runs gea apps as native Linux desktop binaries rendering into an SDL2 window.
Developed and verified on a Raspberry Pi 5 (Raspberry Pi OS Bookworm desktop,
labwc/Wayland; SDL picks X11 via Xwayland there). Nothing is Pi-hardware-
specific — this is plain SDL2 on Linux — but the target is named for the OS
we develop and test on; other distros can become sibling targets later.

## Build & run

```bash
sudo apt install libsdl2-dev libcurl4-openssl-dev   # once
bash targets/raspberry-pi-os/build-raspberry-pi-os.sh bouncing-balls-jsx
./targets/raspberry-pi-os/dist/bouncing-balls-jsx/bouncing-balls-jsx
```

Apps are resolved from the app project root named by `GEA_APPS_ROOT` -- your
own project, or a checkout of [geastack/examples](https://github.com/geastack/examples).
There is no default, so set it. Apps opt in with `"raspberry-pi-os": true`
under `gea.targets` in their `package.json`.

Environment knobs (all optional):

| Variable | Default | Meaning |
| --- | --- | --- |
| `GEA_RPIOS_WIDTH` / `GEA_RPIOS_HEIGHT` | 410 / 502 | logical canvas size (amoled-2.06 shape) |
| `GEA_RPIOS_SCALE` | 2 | integer window scale (window = canvas × scale) |
| `GEA_RPIOS_DPR` | 2.0 | device pixel ratio passed to `Application::init` |
| `GEA_RPIOS_FONT_VIEWPORT_WIDTH/HEIGHT`, `GEA_RPIOS_FONT_DEVICE_PIXEL_RATIO` | 410 / 502 / 2.0 | build-time font rasterization metrics |
| `GEA_RPIOS_STORAGE_DIR` | `$XDG_DATA_HOME/gea/<app-id>` (falls back to `~/.local/share/gea/<app-id>`) | where `localStorage` + settings persist |
| `GEA_LOOP_PERF` | off | per-second `[rpios.loop]` fps/phase log |

## Window & input

- The window is **resizable** — drag the frame and the framework viewport
  reflows live (`setViewportMetrics` + full-tree repaint). **F11** toggles
  borderless fullscreen. **Esc** or closing the window quits. The title bar
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
  (`system_clock`/`gettimeofday`); no platform code is needed. The analog-clock example showing
  10:10 is app-side (its store hardcodes the base time), not a platform gap.

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

## Verified (2026-07-14, Raspberry Pi 5) — new platform features

- **fetch/HTTP(S)**: `maps` downloads OpenStreetMap tiles over the network
  (`[tiles] … src=net`) and renders a full map; needed the WiFi status driver
  (apps gate remote fetches on `wifi().connected()`).
- **localStorage persistence**: standalone harness round-trips the storage
  bridge — a value written by one process is read back by a second from
  `localstorage.bin` (the localStorage-using example apps currently fail to
  build for unrelated geatsc codegen reasons: weather/diagnostics/image-demo).
- **Keyboard**: `reactive-counter` — ArrowUp/ArrowDown drive the count via
  `queueKeyDown` → document keydown dispatch.
- **Mouse wheel**: `virtual-list` — each wheel notch scrolls the 5000-row
  list 48 px (`scrollByKeyStep` returns scrolled=1, `GEA_INPUT_TRACE=1`).
- **Window resize**: `bouncing-balls-jsx` — resizing the window to 1200×600
  reflows the viewport to 600×300 logical and the app fills the new bounds
  (see the canvas-app frame-rate limitation above).
- **Title/pause/expose**: window titles show app display names; minimizing
  pauses the frame loop; restoring/exposing re-presents.

## Verified (2026-07-13, Raspberry Pi 5)

- `bouncing-balls-jsx` — renders and animates at ~100 fps, rAF firing every
  frame.
- `tic-tac-toe` — precision click on the center cell places the mark in that
  cell (mouse → touch pipeline → gesture dispatch → store → re-render).
- `analog-clock` — dial + hands render, digital seconds tick (the 10:10 base
  is hardcoded in the app's store; platform `Date`/`Clock` return the system
  wall-clock).
- `css-animation-showcase` — rotate/opacity/color/scale/translate @keyframes
  all animate (the declarative CSS engine is driven per-frame by the main
  loop). Designed for a wider viewport; run with `GEA_RPIOS_WIDTH=600` to
  avoid right-edge truncation.
- `virtual-list` — drag-scroll works: a 150-logical-px drag scrolls exactly
  150 px through the 5000-row virtualized list.
