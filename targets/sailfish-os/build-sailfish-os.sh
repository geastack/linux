#!/usr/bin/env bash
# Generate a Gea app on the host, then build its Sailfish RPM with sfdk.
set -euo pipefail

TARGET_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
APP_DIR=""
ARCH=i486
CORE_REPO=""
SFDK_BIN="${SFDK_BIN:-sfdk}"
PREPARE_ONLY=0

usage() {
  echo "usage: $0 <app-directory> [--arch i486|armv7hl|aarch64] [--core-repo path] [--sfdk path] [--prepare-only]" >&2
}

while (($#)); do
  case "$1" in
    --arch|--core-repo|--sfdk)
      (($# >= 2)) || { usage; exit 2; }
      case "$1" in
        --arch) ARCH="$2" ;;
        --core-repo) CORE_REPO="$2" ;;
        --sfdk) SFDK_BIN="$2" ;;
      esac
      shift 2
      ;;
    --prepare-only) PREPARE_ONLY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) usage; exit 2 ;;
    *)
      [[ -z "$APP_DIR" ]] || { usage; exit 2; }
      APP_DIR="$1"
      shift
      ;;
  esac
done

[[ -n "$APP_DIR" ]] || { usage; exit 2; }
case "$ARCH" in i486|armv7hl|aarch64) ;; *) usage; exit 2 ;; esac
command -v node >/dev/null || { echo 'node is required' >&2; exit 1; }
APP_DIR="$(cd "$APP_DIR" && pwd -P)"

if [[ -n "$CORE_REPO" ]]; then
  CORE_REPO="$(cd "$CORE_REPO" && pwd -P)"
  node "$TARGET_DIR/prepare.mjs" "$APP_DIR" "$ARCH" "$CORE_REPO"
else
  node "$TARGET_DIR/prepare.mjs" "$APP_DIR" "$ARCH"
fi

((PREPARE_ONLY)) && exit 0
if [[ "$SFDK_BIN" == */* ]]; then
  SFDK_BIN="$(cd "$(dirname "$SFDK_BIN")" && pwd -P)/$(basename "$SFDK_BIN")"
fi
command -v "$SFDK_BIN" >/dev/null || { echo "sfdk not found: $SFDK_BIN" >&2; exit 1; }
APP_ID="$(node -e 'const fs=require("node:fs"); const p=JSON.parse(fs.readFileSync(process.argv[1],"utf8")); process.stdout.write(p.gea.id)' "$APP_DIR/package.json")"
PROJECT_DIR="$TARGET_DIR/build/$APP_ID-$ARCH/project"
(cd "$PROJECT_DIR" && "$SFDK_BIN" -c "target=SailfishOS-5.1.0.11-$ARCH" build)
