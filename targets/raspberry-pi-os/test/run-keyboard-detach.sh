#!/usr/bin/env bash
set -euo pipefail
[[ $# == 1 && "$1" == --build-only ]] || { echo 'Exactly --build-only required' >&2;exit 2; }
: "${GEA_CORE_ROOT:?Core checkout required}"
: "${GEA_SDL_INCLUDE_ROOT:?SDL include directory required}"
: "${GEA_SDL_ARCH_INCLUDE_ROOT:?Architecture include directory required}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$(cd "$HERE/../../.." && pwd)"
ROOT="$GEA_CORE_ROOT"
BUILD_DIR="$SDK/dist/keyboard"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/keyboard-detach-test" "$HERE/keyboard-detach.test.cpp" -I"$HERE/../main" -I"$GEA_SDL_INCLUDE_ROOT" -I"$GEA_SDL_INCLUDE_ROOT/SDL2" -I"$GEA_SDL_ARCH_INCLUDE_ROOT"
