#!/usr/bin/env bash
# targets/raspberry-pi-os/build-raspberry-pi-os.sh — build a gea app as a
# native Linux desktop binary rendering into an SDL2 window. First machine:
# Raspberry Pi 5 (aarch64, Raspberry Pi OS desktop); compiles natively with
# the system toolchain.
#
# Framework sources + include roots come from the shared manifest
# core/gea_sources.sh (single source of truth, shared with web/esp32/macos),
# resolved relative to @geastack/core in node_modules. The app is resolved by
# the gea CLI from the directory this script is run in -- run it from the app
# project you want to compile.
#
# Usage: cd <app project>; targets/raspberry-pi-os/build-raspberry-pi-os.sh <app-id>
# Run:   targets/raspberry-pi-os/dist/<app-id>/<app-id>
#   GEA_RPIOS_WIDTH/GEA_RPIOS_HEIGHT  logical canvas size (default 410x502)
#   GEA_RPIOS_SCALE                   integer window scale (default 2)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TARGET_DIR="$ROOT_DIR/targets/raspberry-pi-os"
APP_ID="${1:-bouncing-balls-jsx}"

command -v sdl2-config >/dev/null || { echo "sdl2-config not found — install libsdl2-dev" >&2; exit 1; }

# Resolve the geastack packages through node_modules (npm-managed file: links).
GEA_CORE="$(node -e "process.stdout.write(require('fs').realpathSync('$ROOT_DIR/node_modules/@geastack/core'))" 2>/dev/null || true)"
GEA_COMPILER="$(node -e "process.stdout.write(require('fs').realpathSync('$ROOT_DIR/node_modules/@geastack/compiler'))" 2>/dev/null || true)"
GEA_PLUGIN="$(node -e "process.stdout.write(require('fs').realpathSync('$ROOT_DIR/node_modules/@geastack/geatsc-plugin-gea'))" 2>/dev/null || true)"
[[ -n "$GEA_CORE" && -f "$GEA_CORE/package.json" ]] || { echo "Cannot resolve @geastack/core — run \`npm install\` in $ROOT_DIR" >&2; exit 1; }
[[ -n "$GEA_COMPILER" && -f "$GEA_COMPILER/dist/cli.js" ]] || { echo "Cannot resolve @geastack/compiler — run \`npm install\` in $ROOT_DIR" >&2; exit 1; }
[[ -n "$GEA_PLUGIN" && -f "$GEA_PLUGIN/dist/index.js" ]] || { echo "Cannot resolve @geastack/geatsc-plugin-gea — run \`npm install\` in $ROOT_DIR" >&2; exit 1; }
GEA_GEAOS="$GEA_CORE/../geaos"
GEA_CLI="$GEA_CORE/bin/gea-embedded.mjs"
# No default: this package cannot know where the caller keeps its apps, and a
# sibling-checkout guess only works on the machine it was written on.

# Framework source manifest (defines gea_fw_include_flags / gea_fw_c_sources /
# gea_fw_cxx_sources; lives at the core repo root, two levels above the package).
source "$GEA_CORE/../../gea_sources.sh"

# Font metrics: match the default logical canvas (410x502, the amoled-2.06
# shape most apps are tuned for). Tunable via env, like the other targets.
FONT_VW="${GEA_RPIOS_FONT_VIEWPORT_WIDTH:-410}"
FONT_VH="${GEA_RPIOS_FONT_VIEWPORT_HEIGHT:-502}"
FONT_DPR="${GEA_RPIOS_FONT_DEVICE_PIXEL_RATIO:-2.0}"

# The project is the directory this script runs in; the CLI resolves it from
# cwd exactly as it does for a bare `gea` invocation.
resolve_app_json() { node "$GEA_CLI" inspect "$1" --json; }

APP_META="$(resolve_app_json "$APP_ID")" || { echo "Unknown app id: $APP_ID (run this from the app project that holds it; cwd is $(pwd -P))" >&2; exit 1; }
APP_ROOT="$(node -e "const a=JSON.parse(process.argv[1]);console.log(a.root);" "$APP_META")"
APP_ENTRY="$(node -e "const a=JSON.parse(process.argv[1]);console.log(a.entry||'index.tsx');" "$APP_META")"
APP_NAME="$(node -e "const a=JSON.parse(process.argv[1]);console.log(a.name||a.id);" "$APP_META")"
# `apps inspect` reports an absolute directory; nothing to join it to.
APP_DIR="$APP_ROOT"
[[ -f "$APP_DIR/$APP_ENTRY" ]] || { echo "no $APP_DIR/$APP_ENTRY" >&2; exit 1; }

DIST_DIR="$TARGET_DIR/dist/$APP_ID"
BUILD_DIR="$DIST_DIR/build"
GENERATED_DIR="$TARGET_DIR/generated/$APP_ID"
mkdir -p "$BUILD_DIR/objs" "$GENERATED_DIR"

# The compiler emits a multi-file program; geatsc-sources.txt lists the
# generated .cpp files (absolute paths) to compile.
SOURCE_LIST="$GENERATED_DIR/geatsc-sources.txt"

# 1) JSX -> C++. Regenerate if app sources or the toolchain are newer than the
#    emitted source list.
needs_regen() {
  [[ ! -f "$SOURCE_LIST" ]] && return 0
  [[ -n "$(find "$APP_DIR" \( -name '*.tsx' -o -name '*.ts' -o -name '*.css' \) \
      -not -path '*/node_modules/*' -not -path '*/dist/*' -newer "$SOURCE_LIST" -print -quit 2>/dev/null)" ]] && return 0
  [[ -n "$(find "$GEA_CORE/scripts/build-gea-vite-geatsc.mjs" \
      "$GEA_COMPILER/dist" "$GEA_PLUGIN/dist" \
      "$0" -newer "$SOURCE_LIST" -print -quit 2>/dev/null)" ]]
}

if needs_regen; then
  echo "[rpios] generating C++ for $APP_ID from $APP_DIR/$APP_ENTRY"
  node "$GEA_CORE/scripts/build-gea-vite-geatsc.mjs" \
    --app-dir "$APP_DIR" --entry "$APP_ENTRY" --out-dir "$GENERATED_DIR" \
    --geatsc-bin "$GEA_COMPILER/dist/cli.js" \
    --geatsc-gea-plugin "$GEA_PLUGIN/dist/index.js" \
    --font-viewport-width "$FONT_VW" --font-viewport-height "$FONT_VH" \
    --font-device-pixel-ratio "$FONT_DPR"
  # The object cache below keys only on `obj -nt src` (the .cpp), never on
  # included headers. A regeneration can change a generated HEADER's shape
  # (e.g. a store method going template → typed) while an includer .cpp comes
  # out byte-identical with its old mtime — the stale object then links
  # against the new header's world and dies with undefined references
  # (GameStore::play<double> after the typed-snapshot change). Objects are
  # only trustworthy for the generation they were compiled against, so drop
  # them whenever the generator runs.
  rm -rf "$BUILD_DIR/objs"
  mkdir -p "$BUILD_DIR/objs"
fi

# host/*.cpp files do `#include "gea_runtime.cpp"` to pull in the gea_cpp_value
# runtime; write the one-line wrapper into the generated dir (same as macos/esp32).
if [[ ! -f "$GENERATED_DIR/gea_runtime.cpp" ]] || [[ "$(cat "$GENERATED_DIR/gea_runtime.cpp")" != '#include "runtime_pch.h"' ]]; then
  echo '#include "runtime_pch.h"' > "$GENERATED_DIR/gea_runtime.cpp"
fi

# 2) Sources. Framework set from the shared manifest; like the macos build,
#    drop host/camera (no platform camera backend), runtime.cpp (this target
#    runs its own frame loop), and the framework services (no mirror/
#    diagnostics server here yet).
INCLUDES=(
  -I"$TARGET_DIR/include"
  -I"$GENERATED_DIR"
)
while IFS= read -r __inc; do INCLUDES+=("$__inc"); done < <(gea_fw_include_flags)

C_SOURCES=()
while IFS= read -r __src; do C_SOURCES+=("$__src"); done < <(gea_fw_c_sources)
C_SOURCES+=("$TARGET_DIR/main/rpios_apps.c")

CXX_SOURCES=()
while IFS= read -r __src; do CXX_SOURCES+=("$__src"); done < <(gea_fw_cxx_sources | grep -vE "/(host/camera|runtime|services/[a-z_]+)\.cpp$")
CXX_SOURCES+=(
  "$TARGET_DIR/main/rpios_display.cpp"
  "$TARGET_DIR/main/rpios_audio.cpp"
  "$TARGET_DIR/main/rpios_memory.cpp"
  "$TARGET_DIR/main/rpios_network.cpp"
  "$TARGET_DIR/main/rpios_sensors.cpp"
  "$TARGET_DIR/main/rpios_storage.cpp"
  "$TARGET_DIR/main/rpios_storage_bridge.cpp"
  "$TARGET_DIR/main/rpios_timers.cpp"
  "$TARGET_DIR/main/rpios_app_platform.cpp"
  "$TARGET_DIR/main/rpios_main.cpp"
)

# Single-app entry (no resident registry).
CXX_SOURCES+=("$GEA_CORE/gea_app_entry.cpp")
CXX_SOURCES+=("$GEA_GEAOS/resident_apps.cpp")
while IFS= read -r __src; do
  [[ -n "$__src" && -f "$__src" ]] && CXX_SOURCES+=("$__src")
done < "$SOURCE_LIST"
for gen in gea_embedded_font_generated.cpp gea_embedded_assets_generated.cpp; do
  [[ -f "$GENERATED_DIR/$gen" ]] && CXX_SOURCES+=("$GENERATED_DIR/$gen")
done

# 3) Native compile + link.
CC="${CC:-gcc}"
CXX="${CXX:-g++}"
SDL_CFLAGS=($(sdl2-config --cflags))
SDL_LIBS=($(sdl2-config --libs))

CFLAGS_COMMON=(-O3 -DGEA_EMBEDDED_GIF_C_API -DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 -funwind-tables "${INCLUDES[@]}")
CXXFLAGS_COMMON=(-std=c++20 -O3 -DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 -DNDEBUG -funwind-tables \
  -DGEA_RPIOS_APP_ID="\"$APP_ID\"" -DGEA_RPIOS_APP_TITLE="\"$APP_NAME\"" \
  -include "$TARGET_DIR/include/rpios_prelude.h" "${SDL_CFLAGS[@]}" "${INCLUDES[@]}")

obj_for() {
  local rel="${1#"$GEA_CORE/../../"}"   # core-repo sources → package-relative
  rel="${rel#"$ROOT_DIR/"}"             # repo-local sources → repo-relative
  echo "$BUILD_DIR/objs/$(echo "$rel" | tr / __ | sed 's/\.\(c\|cpp\)$//').o"
}

# Parallel compile: queue every stale source, J jobs at a time.
J="$(nproc)"
declare -a OBJS=() PIDS=()
running=0
wait_slot() {
  if (( running >= J )); then
    wait -n || { wait || true; echo "[rpios] compile failed" >&2; exit 1; }
    (( running-- )) || true
  fi
}

compile_one() {
  local src="$1" obj="$2"
  case "$src" in
    *.c)  "$CC"  "${CFLAGS_COMMON[@]}"   -c "$src" -o "$obj" ;;
    *.cpp) "$CXX" "${CXXFLAGS_COMMON[@]}" -x c++ -c "$src" -o "$obj" ;;
  esac
}

for src in "${C_SOURCES[@]}" "${CXX_SOURCES[@]}"; do
  obj="$(obj_for "$src")"
  OBJS+=("$obj")
  if [[ -f "$obj" && "$obj" -nt "$src" ]]; then continue; fi
  wait_slot
  echo "[cc] ${src#"$GEA_CORE/../../"}"
  compile_one "$src" "$obj" &
  PIDS+=($!)
  (( ++running ))
done
fail=0
for pid in "${PIDS[@]}"; do wait "$pid" || fail=1; done
(( fail == 0 )) || { echo "[rpios] compile failed" >&2; exit 1; }

echo "[rpios] linking $DIST_DIR/$APP_ID"
"$CXX" -O3 -Wl,--export-dynamic -o "$DIST_DIR/$APP_ID" "${OBJS[@]}" "${SDL_LIBS[@]}" -lcurl -lpthread -lm -lrt
file "$DIST_DIR/$APP_ID"
echo "[rpios] built $DIST_DIR/$APP_ID"
