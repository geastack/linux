#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && "$1" == --build-only ) ]] || { echo 'Use [--build-only]' >&2;exit 2; }
: "${GEA_CORE_ROOT:?Core checkout required}"
: "${GEA_SDL_INCLUDE_ROOT:?SDL usr/include directory required}"
: "${GEA_SDL_ARCH_INCLUDE_ROOT:?Debian architecture include directory required}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$(cd "$HERE/../../.." && pwd)"
ROOT="$GEA_CORE_ROOT"
BUILD_DIR="$SDK/dist/keyboard"
mkdir -p "$BUILD_DIR"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
node "$HERE/extract-keyboard-dispatch.mjs"
output="$BUILD_DIR/keyboard-test"
gea_build_native_test "$BUILD_DIR" "$output" "$HERE/keyboard.test.cpp" "$ROOT/packages/host/input.cpp" -I"$HERE/../main" -I"$GEA_SDL_INCLUDE_ROOT" -I"$GEA_SDL_INCLUDE_ROOT/SDL2" -I"$GEA_SDL_ARCH_INCLUDE_ROOT"
if [[ $# == 0 ]];then "$output";fi
