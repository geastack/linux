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

SDL_CONFIG="${GEA_SDL2_CONFIG:-sdl2-config}"
if [[ -z "${GEA_LINUX_SYSROOT:-}" ]]; then
command -v "$SDL_CONFIG" >/dev/null || { echo "sdl2-config not found — install libsdl2-dev" >&2; exit 1; }
fi

# Resolve the geastack packages through node_modules (npm-managed file: links).
resolve_package() { node -e 'const p=require("path");const r=require("module").createRequire(p.join(process.cwd(),"package.json"));process.stdout.write(p.dirname(r.resolve(process.argv[1]+"/package.json")))' "$1"; }
GEA_CORE="${GEA_CORE:-$(resolve_package @geastack/core)}"
GEA_COMPILER="${GEA_COMPILER:-$(resolve_package @geastack/compiler)}"
GEA_PLUGIN="${GEA_PLUGIN:-$(resolve_package @geastack/geatsc-plugin-gea)}"
GEA_GEAOS="${GEA_GEAOS_PACKAGE_DIR:-$GEA_CORE/../geaos}"
GEA_CLI="${GEA_CLI_BIN:?Use gea build --target raspberry-pi-os}"
[[ -f "$GEA_CORE/gea_sources.sh" && -f "$GEA_COMPILER/dist/cli.js" && -f "$GEA_PLUGIN/dist/index.js" ]] || { echo "Missing native framework/compiler/plugin inputs" >&2; exit 1; }
# No default: this package cannot know where the caller keeps its apps, and a
# sibling-checkout guess only works on the machine it was written on.

# Framework source manifest (defines gea_fw_include_flags / gea_fw_c_sources /
# gea_fw_cxx_sources; lives at the core repo root, two levels above the package).
GEA_HOST_DIR="${GEA_HOST_DIR:-$GEA_CORE/../host}"
GEA_ENGINE_DIR="${GEA_ENGINE_DIR:-$GEA_CORE/../engine}"
GEA_ELEMENTS_DIR="${GEA_ELEMENTS_DIR:-$GEA_CORE/../elements}"
GEA_GEAOS_PACKAGE_DIR="${GEA_GEAOS_PACKAGE_DIR:-$GEA_CORE/../geaos}"
GEA_COMPILER_RUNTIME="${GEA_COMPILER_RUNTIME:-$GEA_COMPILER/src/targets/cpp/runtime}"
export GEA_CORE GEA_HOST_DIR GEA_ENGINE_DIR GEA_ELEMENTS_DIR GEA_GEAOS_PACKAGE_DIR GEA_COMPILER_RUNTIME
source "$GEA_CORE/gea_sources.sh"

# Font metrics: match the default logical canvas (410x502, the amoled-2.06
# shape most apps are tuned for). Tunable via env, like the other targets.
FONT_VW="${GEA_RPIOS_FONT_VIEWPORT_WIDTH:-410}"
FONT_VH="${GEA_RPIOS_FONT_VIEWPORT_HEIGHT:-502}"
FONT_DPR="${GEA_RPIOS_FONT_DEVICE_PIXEL_RATIO:-2.0}"

# The project is the directory this script runs in; the CLI resolves it from
# cwd exactly as it does for a bare `gea` invocation.
resolve_app_json() { node "$GEA_CLI" apps inspect "$1" --json; }

APP_META="$(resolve_app_json "$APP_ID")" || { echo "Unknown app id: $APP_ID (run this from the app project that holds it; cwd is $(pwd -P))" >&2; exit 1; }
APP_ROOT="$(node -e "const a=JSON.parse(process.argv[1]);console.log(a.root);" "$APP_META")"
APP_ENTRY="$(node -e "const a=JSON.parse(process.argv[1]);console.log(a.entry||'index.tsx');" "$APP_META")"
APP_NAME="$(node -e "const a=JSON.parse(process.argv[1]);console.log(a.name||a.id);" "$APP_META")"
# `apps inspect` reports an absolute directory; nothing to join it to.
APP_DIR="$APP_ROOT"
[[ -f "$APP_DIR/$APP_ENTRY" ]] || { echo "no $APP_DIR/$APP_ENTRY" >&2; exit 1; }

OUTPUT_ROOT="${GEA_LINUX_OUTPUT_DIR:-$APP_DIR/dist/raspberry-pi-os}"
DIST_DIR="$OUTPUT_ROOT/$APP_ID"
BUILD_DIR="$DIST_DIR/build"
GENERATED_DIR="$OUTPUT_ROOT/.generated/$APP_ID"
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

EXTRA_PLUGIN_ARGS=()
while IFS= read -r plugin; do
  [[ -f "$plugin" ]] || { echo "Missing app compiler plugin: $plugin" >&2; exit 1; }
  EXTRA_PLUGIN_ARGS+=(--extra-geatsc-plugin "$plugin")
done < <(node -e 'const p=require("path"),a=JSON.parse(process.argv[1]);for(const x of a.compilerPlugins||[])console.log(p.resolve(a.root,x))' "$APP_META")

if needs_regen; then
  echo "[rpios] generating C++ for $APP_ID from $APP_DIR/$APP_ENTRY"
  node "$GEA_CORE/scripts/build-gea-vite-geatsc.mjs" \
    --app-dir "$APP_DIR" --entry "$APP_ENTRY" --out-dir "$GENERATED_DIR" \
    --geatsc-bin "$GEA_COMPILER/dist/cli.js" \
    --geatsc-gea-plugin "$GEA_PLUGIN/dist/index.js" \
    --gea-replace-renderers \
    --runtime-ttf-fonts \
    ${EXTRA_PLUGIN_ARGS[@]+"${EXTRA_PLUGIN_ARGS[@]}"} \
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

[[ -s "$SOURCE_LIST" ]] || { echo "Missing native generated source list" >&2; exit 1; }

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
  -I"$APP_DIR"
  -I"$APP_DIR/native"
)
while IFS= read -r __inc; do INCLUDES+=("$__inc"); done < <(gea_fw_include_flags)

C_SOURCES=()
while IFS= read -r __src; do C_SOURCES+=("$__src"); done < <(gea_fw_c_sources)
C_SOURCES+=("$TARGET_DIR/main/rpios_apps.c")

CXX_SOURCES=()
FRAMEWORK_CXX_SOURCES="$(GEA_DESKTOP_WEBSOCKET_CURL=1 gea_fw_cxx_sources)" || exit 1
case "$FRAMEWORK_CXX_SOURCES" in
  *"/host/websocket_curl.cpp"*) ;;
  *) echo "Missing framework Linux WebSocket transport source" >&2; exit 1 ;;
esac
while IFS= read -r __src; do CXX_SOURCES+=("$__src"); done < <(printf '%s\n' "$FRAMEWORK_CXX_SOURCES" | grep -vE "/(host/camera|runtime|services/[a-z_]+)\.cpp$")
CXX_SOURCES+=(
  "$TARGET_DIR/main/rpios_display.cpp"
  "$TARGET_DIR/main/rpios_audio.cpp"
  "$TARGET_DIR/main/rpios_memory.cpp"
  "$TARGET_DIR/main/rpios_network.cpp"
  "$TARGET_DIR/main/rpios_sensors.cpp"
  "$TARGET_DIR/main/rpios_storage.cpp"
  "$TARGET_DIR/main/rpios_timers.cpp"
  "$TARGET_DIR/main/rpios_app_platform.cpp"
  "$TARGET_DIR/main/rpios_main.cpp"
)

# Single-app entry (no resident registry).
CXX_SOURCES+=("$GEA_CORE/gea_app_entry.cpp")
if [[ -f "$GEA_GEAOS/resident_apps.cpp" ]]; then CXX_SOURCES+=("$GEA_GEAOS/resident_apps.cpp"); fi
while IFS= read -r __src; do
  [[ -n "$__src" && -f "$__src" ]] && CXX_SOURCES+=("$__src")
done < "$SOURCE_LIST"
for gen in gea_embedded_font_generated.cpp gea_embedded_assets_generated.cpp; do
  [[ -f "$GENERATED_DIR/$gen" ]] && CXX_SOURCES+=("$GENERATED_DIR/$gen")
done

# App-owned native providers are explicit manifest inputs, not target hardcodes.
while IFS= read -r native; do
  [[ -f "$native" ]] || { echo "Missing app native source: $native" >&2; exit 1; }
  case "$native" in
    *.c) C_SOURCES+=("$native") ;;
    *.cpp|*.cc|*.cxx|*.mm|*.m) CXX_SOURCES+=("$native") ;;
    *) echo "Unsupported Linux native source: $native" >&2; exit 1 ;;
  esac
done < <(node -e 'const a=JSON.parse(process.argv[1]);for(const x of a.nativeSourcePaths||[])console.log(x)' "$APP_META")

APP_LINK_RECORD="$(node -e 'const a=JSON.parse(process.argv[1]);const c=a.nativeLink?.["raspberry-pi-os"];if(!c)throw Error("CLI lacks validated raspberry-pi-os nativeLink");process.stdout.write(JSON.stringify(c))' "$APP_META")"
APP_NATIVE_LIBRARIES=()
while IFS= read -r library; do [[ -z "$library" ]] || APP_NATIVE_LIBRARIES+=("$library"); done < <(node -e 'for(const p of JSON.parse(process.argv[1]).nativeLibraries)console.log(p)' "$APP_LINK_RECORD")

node "$GEA_CLI" apps sync-native-resources "$APP_ID" --target raspberry-pi-os --output "$DIST_DIR/resources" > "$BUILD_DIR/native-resources.json"

# 3) Native compile + link. Fresh objects preserve header/flag authority.
rm -f "$BUILD_DIR/objs/"*.o
CC="${CC:-gcc}"
CXX="${CXX:-g++}"
CROSS_FLAGS=()
CURL_LIBS=(-lcurl)
FONT_CFLAGS=()
FONT_LIBS=()
APP_LIBS=()
if [[ -n "${GEA_LINUX_SYSROOT:-}" ]]; then
  [[ "$GEA_LINUX_SYSROOT" = /* && -d "$GEA_LINUX_SYSROOT/usr/lib/gcc/aarch64-linux-gnu/14" ]] || { echo "Cross build requires an absolute GCC14 AArch64 sysroot" >&2; exit 1; }
  : "${GEA_LINUX_SDL_ROOT:?Set staged SDL development root}"
  : "${GEA_LINUX_CURL_ROOT:?Set staged curl development root}"
  : "${GEA_LINUX_SDL_LIBRARY:?Set matching SDL runtime library}"
  : "${GEA_LINUX_CURL_LIBRARY:?Set matching curl runtime library}"
  : "${GEA_LINUX_FREETYPE_ROOT:?Set staged FreeType development root}"
  : "${GEA_LINUX_FREETYPE_LIBRARY:?Set matching FreeType runtime library}"
  [[ -f "$GEA_LINUX_FREETYPE_ROOT/usr/include/freetype2/ft2build.h" && -f "$GEA_LINUX_FREETYPE_LIBRARY" ]] || { echo "Missing staged FreeType inputs" >&2; exit 1; }
  FONT_CFLAGS=(-I"$GEA_LINUX_FREETYPE_ROOT/usr/include/freetype2")
  FONT_LIBS=("$GEA_LINUX_FREETYPE_LIBRARY")
  CROSS_FLAGS=(--target=aarch64-linux-gnu "--sysroot=$GEA_LINUX_SYSROOT" "--gcc-install-dir=$GEA_LINUX_SYSROOT/usr/lib/gcc/aarch64-linux-gnu/14" -fuse-ld=lld)
  SDL_CFLAGS=(-I"$GEA_LINUX_SDL_ROOT/usr/include" -I"$GEA_LINUX_SDL_ROOT/usr/include/SDL2" -I"$GEA_LINUX_SDL_ROOT/usr/include/aarch64-linux-gnu" -I"$GEA_LINUX_SDL_ROOT/usr/include/aarch64-linux-gnu/SDL2" -I"$GEA_LINUX_CURL_ROOT/usr/include/aarch64-linux-gnu" -D_REENTRANT)
  SDL_LIBS=("$GEA_LINUX_SDL_LIBRARY")
  CURL_LIBS=("$GEA_LINUX_CURL_LIBRARY")
  if [[ -n "${GEA_LINUX_LIBRARY_DIR:-}" ]]; then
    [[ "$GEA_LINUX_LIBRARY_DIR" = /* && -d "$GEA_LINUX_LIBRARY_DIR" ]] || { echo "Cross library directory must be absolute and existing" >&2; exit 1; }
    CROSS_FLAGS+=(-L"$GEA_LINUX_LIBRARY_DIR" "-Wl,-rpath-link,$GEA_LINUX_LIBRARY_DIR")
  fi
else
  SDL_CFLAGS=($("$SDL_CONFIG" --cflags))
  SDL_LIBS=($("$SDL_CONFIG" --libs))
  command -v pkg-config >/dev/null && pkg-config --exists freetype2 || { echo "Native Linux runtime fonts require FreeType development package (freetype2)" >&2; exit 1; }
  FONT_CFLAGS=($(pkg-config --cflags freetype2))
  FONT_LIBS=($(pkg-config --libs freetype2))
fi

while IFS= read -r directory; do [[ -z "$directory" ]] || INCLUDES+=(-I"$directory"); done < <(node -e 'for(const p of JSON.parse(process.argv[1]).nativeIncludeDirectories||[])console.log(p)' "$APP_LINK_RECORD")
while IFS= read -r library; do [[ -z "$library" ]] || APP_LIBS+=("-l$library"); done < <(node -e 'for(const p of JSON.parse(process.argv[1]).nativeSystemLibraries||[])console.log(p)' "$APP_LINK_RECORD")

CFLAGS_COMMON=(-O3 -DGEA_EMBEDDED_GIF_C_API -DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 -funwind-tables "${INCLUDES[@]}")
CXXFLAGS_COMMON=(-std=c++20 -O3 -DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 -DNDEBUG -funwind-tables \
  -DGEA_EMBEDDED_TTF_RUNTIME_FONTS=1 -DGEA_EMBEDDED_TTF_FREETYPE=1 -DGEA_CSS_FONT_STRETCH=1 \
  -DGEA_RPIOS_APP_ID="\"$APP_ID\"" -DGEA_RPIOS_APP_TITLE="\"$APP_NAME\"" \
  -include "$TARGET_DIR/include/rpios_prelude.h" "${SDL_CFLAGS[@]}" "${FONT_CFLAGS[@]}" "${INCLUDES[@]}")

obj_for() {
  local rel="${1#"$GEA_CORE/../../"}"   # core-repo sources → package-relative
  rel="${rel#"$ROOT_DIR/"}"             # repo-local sources → repo-relative
  echo "$BUILD_DIR/objs/$(echo "$rel" | tr / __ | sed 's/\.\(c\|cpp\)$//').o"
}

# Parallel compile: queue every stale source, J jobs at a time.
J="${GEA_BUILD_JOBS:-2}"
[[ "$J" =~ ^[1-9][0-9]*$ && "$J" -le 16 ]] || { echo "GEA_BUILD_JOBS must be 1..16" >&2; exit 1; }
declare -a OBJS=() PIDS=()
running=0
wait_slot() {
  if (( running >= J )); then
    local failed=0
    for pid in "${PIDS[@]}"; do wait "$pid" || failed=1; done
    PIDS=(); running=0
    (( failed == 0 )) || { echo "[rpios] compile failed" >&2; exit 1; }
  fi
}

compile_one() {
  local src="$1" obj="$2"
  case "$src" in
    *.c)  "$CC" "${CROSS_FLAGS[@]}" "${CFLAGS_COMMON[@]}"   -c "$src" -o "$obj" ;;
    *.cpp|*.cc|*.cxx|*.mm|*.m) "$CXX" "${CROSS_FLAGS[@]}" "${CXXFLAGS_COMMON[@]}" -x c++ -c "$src" -o "$obj" ;;
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
"$CXX" "${CROSS_FLAGS[@]}" -O3 -Wl,--export-dynamic -Xlinker -Map -Xlinker "$BUILD_DIR/link.map" -o "$DIST_DIR/$APP_ID" "${OBJS[@]}" ${APP_NATIVE_LIBRARIES[@]+"${APP_NATIVE_LIBRARIES[@]}"} "${SDL_LIBS[@]}" "${CURL_LIBS[@]}" "${FONT_LIBS[@]}" ${APP_LIBS[@]+"${APP_LIBS[@]}"} -lpthread -lm -lrt
file "$DIST_DIR/$APP_ID"
echo "[rpios] built $DIST_DIR/$APP_ID"
