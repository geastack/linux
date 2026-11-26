#!/usr/bin/env bash
# Build and run all Sailfish OS native C++ unit/sanitizer tests on the host.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -P)"
TARGET_DIR="$(cd "$HERE/.." && pwd -P)"
REPO_ROOT="$(cd "$TARGET_DIR/../.." && pwd -P)"
CORE_INCLUDE="$REPO_ROOT/node_modules/@geastack/core/include"

[[ -d "$CORE_INCLUDE" ]] || {
  echo "Missing $CORE_INCLUDE — run 'npm ci' in $REPO_ROOT first." >&2
  exit 1
}

CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++
  elif command -v g++ >/dev/null 2>&1; then CXX=g++
  else echo "No C++20 compiler found (clang++ or g++)" >&2; exit 1
  fi
fi

# Resolve SDL2 flags cleanly on Linux (pkg-config/sdl2-config) and macOS (Homebrew).
SDL_CFLAGS=()
SDL_LIBS=()
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
  read -r -a SDL_CFLAGS <<< "$(pkg-config --cflags sdl2)"
  read -r -a SDL_LIBS <<< "$(pkg-config --libs sdl2)"
elif command -v sdl2-config >/dev/null 2>&1; then
  read -r -a SDL_CFLAGS <<< "$(sdl2-config --cflags)"
  read -r -a SDL_LIBS <<< "$(sdl2-config --libs)"
elif command -v brew >/dev/null 2>&1 && brew --prefix sdl2-compat >/dev/null 2>&1; then
  SDL_PREFIX="$(brew --prefix sdl2-compat)"
  SDL_CFLAGS=(-I"$SDL_PREFIX/include" -I"$SDL_PREFIX/include/SDL2")
  SDL_LIBS=(-L"$SDL_PREFIX/lib" -Wl,-rpath,"$SDL_PREFIX/lib" -lSDL2)
elif command -v brew >/dev/null 2>&1 && brew --prefix sdl2 >/dev/null 2>&1; then
  SDL_PREFIX="$(brew --prefix sdl2)"
  SDL_CFLAGS=(-I"$SDL_PREFIX/include" -I"$SDL_PREFIX/include/SDL2")
  SDL_LIBS=(-L"$SDL_PREFIX/lib" -Wl,-rpath,"$SDL_PREFIX/lib" -lSDL2)
else
  echo "SDL2 development headers not found" >&2
  exit 1
fi

# Ensure <SDL2/SDL.h> resolves even when sdl2-config/pkg-config only emits -I.../SDL2.
EXTRA_SDL_INCLUDES=()
for flag in ${SDL_CFLAGS[@]+"${SDL_CFLAGS[@]}"}; do
  if [[ "$flag" == -I*/SDL2 ]]; then
    EXTRA_SDL_INCLUDES+=("${flag%/SDL2}")
  fi
done
if (( ${#EXTRA_SDL_INCLUDES[@]} > 0 )); then
  SDL_CFLAGS+=("${EXTRA_SDL_INCLUDES[@]}")
fi

if [[ "$(uname -s)" == Darwin ]]; then
  EXTRA_SDL_RPATHS=()
  for flag in ${SDL_LIBS[@]+"${SDL_LIBS[@]}"}; do
    if [[ "$flag" == -L* ]]; then
      EXTRA_SDL_RPATHS+=("-Wl,-rpath,${flag#-L}")
    fi
  done
  if (( ${#EXTRA_SDL_RPATHS[@]} > 0 )); then
    SDL_LIBS+=("${EXTRA_SDL_RPATHS[@]}")
  fi
fi

CURL_CFLAGS=()
CURL_LIBS=(-lcurl)
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
  read -r -a CURL_CFLAGS <<< "$(pkg-config --cflags libcurl)"
  read -r -a CURL_LIBS <<< "$(pkg-config --libs libcurl)"
elif command -v curl-config >/dev/null 2>&1; then
  read -r -a CURL_CFLAGS <<< "$(curl-config --cflags)"
  read -r -a CURL_LIBS <<< "$(curl-config --libs)"
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gea-sailfish-native-XXXXXX")"
SERVER_PID=""
cleanup() {
  if [[ -n "$SERVER_PID" ]]; then kill "$SERVER_PID" 2>/dev/null || true; fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# Use real @geastack/host headers if available, else a minimal test fetch.h stub.
HOST_INCLUDE=""
if [[ -d "$REPO_ROOT/node_modules/@geastack/host/include" ]]; then
  HOST_INCLUDE="$REPO_ROOT/node_modules/@geastack/host/include"
elif [[ -n "${GEA_CORE_ROOT:-}" && -d "$GEA_CORE_ROOT/packages/host/include" ]]; then
  HOST_INCLUDE="$GEA_CORE_ROOT/packages/host/include"
else
  HOST_INCLUDE="$TMP_DIR/host-include"
  mkdir -p "$HOST_INCLUDE/host"
  cat > "$HOST_INCLUDE/host/fetch.h" <<'EOF'
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gea::host {
using FetchHeaderMap = std::map<std::string, std::string>;
struct FetchRequestInit {
  std::string method = "GET";
  FetchHeaderMap headers;
  std::vector<std::uint8_t> body;
  int timeout_ms = 0;
};
struct FetchResponse {
  double status = 0.0;
  bool ok = false;
  std::string status_text;
  FetchHeaderMap headers;
  std::vector<std::uint8_t> body;
};
}  // namespace gea::host
EOF
fi

COMMON_FLAGS=(
  -std=c++20 -O1 -g -Wall -Wextra -Werror
  -fsanitize=address,undefined -fno-omit-frame-pointer
  -I"$TARGET_DIR/include"
  -I"$CORE_INCLUDE"
)

# Intentional singletons (Engine::instance) are not freed at process exit.
export LSAN_OPTIONS="${LSAN_OPTIONS:+$LSAN_OPTIONS:}detect_leaks=0"

echo "==> [1/6] dpr.test"
"$CXX" "${COMMON_FLAGS[@]}" "$HERE/dpr.test.cpp" -o "$TMP_DIR/dpr.test"
"$TMP_DIR/dpr.test"

echo "==> [2/6] keyboard_maliit.test"
"$CXX" "${COMMON_FLAGS[@]}" "${SDL_CFLAGS[@]}" "$HERE/keyboard_maliit.test.cpp" -o "$TMP_DIR/keyboard_maliit.test"
"$TMP_DIR/keyboard_maliit.test"

echo "==> [3/6] touch.test"
"$CXX" "${COMMON_FLAGS[@]}" "${SDL_CFLAGS[@]}" \
  "$HERE/touch.test.cpp" "$TARGET_DIR/main/sailfish_sensors.cpp" -pthread \
  -o "$TMP_DIR/touch.test"
"$TMP_DIR/touch.test"

echo "==> [4/6] storage.test"
"$CXX" "${COMMON_FLAGS[@]}" \
  -DGEA_SAILFISH_ORGANIZATION_NAME=\"org.geastack\" \
  -DGEA_SAILFISH_APPLICATION_NAME=\"probe_app\" \
  "$HERE/storage.test.cpp" "$TARGET_DIR/main/sailfish_storage.cpp" \
  -o "$TMP_DIR/storage.test"
"$TMP_DIR/storage.test"

echo "==> [5/6] audio.test"
"$CXX" "${COMMON_FLAGS[@]}" "${SDL_CFLAGS[@]}" \
  -DGEA_SAILFISH_APP_TITLE=\"SailfishProbe\" \
  "$HERE/audio.test.cpp" "${SDL_LIBS[@]}" -pthread \
  -o "$TMP_DIR/audio.test"
"$TMP_DIR/audio.test"

echo "==> [6/6] network.test"
PORT_FILE="$TMP_DIR/http-port"
node -e '
const http = require("node:http");
const fs = require("node:fs");
const srv = http.createServer((req, res) => {
  if (req.url === "/ok") {
    res.writeHead(200, { "Content-Type": "text/plain", "X-Test": "1" });
    res.end("hello-sailfish");
  } else if (req.url === "/redirect") {
    res.writeHead(302, { Location: "/ok", "X-Hop": "stale" });
    res.end();
  } else if (req.url === "/echo") {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      res.writeHead(200, { "Content-Type": "text/plain" });
      res.end((req.headers["x-client"] || "") + ":" + Buffer.concat(chunks).toString());
    });
  } else if (req.url === "/slow") {
    setTimeout(() => { res.writeHead(200); res.end("late"); }, 1000);
  } else {
    res.writeHead(404); res.end();
  }
});
srv.listen(0, "127.0.0.1", () => {
  fs.writeFileSync(process.argv[1], String(srv.address().port));
});
' "$PORT_FILE" &
SERVER_PID=$!

for _ in $(seq 1 50); do
  [[ -s "$PORT_FILE" ]] && break
  sleep 0.05
done
[[ -s "$PORT_FILE" ]] || { echo "Local test HTTP server failed to start" >&2; exit 1; }
TEST_HTTP_URL="http://127.0.0.1:$(cat "$PORT_FILE")"

NETWORK_EXTRA_SRCS=()
NETWORK_EXTRA_DEFS=()
if [[ -d "$REPO_ROOT/node_modules/@geastack/host" && -f "$REPO_ROOT/node_modules/@geastack/host/wifi.cpp" ]]; then
  NETWORK_EXTRA_SRCS+=("$REPO_ROOT/node_modules/@geastack/host/wifi.cpp")
elif [[ -n "${GEA_CORE_ROOT:-}" && -f "$GEA_CORE_ROOT/packages/host/wifi.cpp" ]]; then
  NETWORK_EXTRA_SRCS+=("$GEA_CORE_ROOT/packages/host/wifi.cpp")
else
  NETWORK_EXTRA_DEFS+=(-DGEA_SAILFISH_TEST_STUB_WIFIDRIVER=1)
fi

"$CXX" "${COMMON_FLAGS[@]}" -I"$HOST_INCLUDE" ${CURL_CFLAGS[@]+"${CURL_CFLAGS[@]}"} \
  ${NETWORK_EXTRA_DEFS[@]+"${NETWORK_EXTRA_DEFS[@]}"} \
  "$HERE/network.test.cpp" "$TARGET_DIR/main/sailfish_network.cpp" \
  ${NETWORK_EXTRA_SRCS[@]+"${NETWORK_EXTRA_SRCS[@]}"} \
  ${CURL_LIBS[@]+"${CURL_LIBS[@]}"} -pthread \
  -o "$TMP_DIR/network.test"
TEST_HTTP_URL="$TEST_HTTP_URL" "$TMP_DIR/network.test"

echo "ALL 6 NATIVE SAILFISH TESTS PASSED"
