#!/usr/bin/env bash
# Verify a Sailfish OS RPM build, official Jolla Harbour compliance (`sfdk check`
# or `/usr/libexec/sdk-harbour-rpmvalidator/rpmvalidation.sh`), RPM contents,
# and optional deployment onto a physical aarch64 Sailfish OS device.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -P)"
TARGET_DIR="$(cd "$HERE/.." && pwd -P)"

APP_DIR=""
ARCH="aarch64"
SFDK_BIN="${SFDK_BIN:-sfdk}"
DOCKER_VOLUME="${SAILFISH_DOCKER_VOLUME:-}"
DOCKER_IMAGE="${SAILFISH_DOCKER_IMAGE:-sailfish-arm64-probe:local}"
RPM_PATH=""
DEVICE=""
CORE_REPO=""

usage() {
  cat >&2 <<EOF
usage: $0 <app-directory> [--arch aarch64|armv7hl|i486] [--sfdk path] [--docker-volume name] [--docker-image image] [--rpm path] [--device user@host] [--core-repo path]
EOF
}

while (($#)); do
  case "$1" in
    --arch|--sfdk|--docker-volume|--docker-image|--rpm|--device|--core-repo)
      (($# >= 2)) || { usage; exit 2; }
      case "$1" in
        --arch) ARCH="$2" ;;
        --sfdk) SFDK_BIN="$2" ;;
        --docker-volume) DOCKER_VOLUME="$2" ;;
        --docker-image) DOCKER_IMAGE="$2" ;;
        --rpm) RPM_PATH="$2" ;;
        --device) DEVICE="$2" ;;
        --core-repo) CORE_REPO="$2" ;;
      esac
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *)
      [[ -z "$APP_DIR" ]] || { usage; exit 2; }
      APP_DIR="$1"
      shift
      ;;
  esac
done

[[ -n "$APP_DIR" ]] || { usage; exit 2; }
case "$ARCH" in aarch64|armv7hl|i486) ;; *) usage; exit 2 ;; esac
APP_DIR="$(cd "$APP_DIR" && pwd -P)"

APP_ID="$(node -e 'const fs=require("node:fs"); process.stdout.write(JSON.parse(fs.readFileSync(process.argv[1],"utf8")).gea.id)' "$APP_DIR/package.json")"
PKG_NAME="harbour-gea-$APP_ID"
PROJECT_DIR="$APP_DIR/.gea-sailfish/build/$APP_ID-$ARCH/project"

# Auto-select Docker chroot build volume if sfdk is absent and Docker has the
# Sailfish aarch64 build volume.
if [[ -z "$DOCKER_VOLUME" ]] && ! command -v "$SFDK_BIN" >/dev/null 2>&1; then
  if [[ "$ARCH" == "aarch64" ]] && command -v docker >/dev/null 2>&1 && docker volume inspect sailfish-5.1.0.11-aarch64-build >/dev/null 2>&1; then
    DOCKER_VOLUME="sailfish-5.1.0.11-aarch64-build"
  fi
fi

run_docker_chroot_verify() {
  command -v docker >/dev/null 2>&1 || { echo "docker not found" >&2; exit 1; }
  [[ "$ARCH" == "aarch64" ]] || { echo "Docker chroot build currently supports aarch64 only (got $ARCH)" >&2; exit 2; }

  echo "==> Preparing Sailfish project ($APP_ID, $ARCH)"
  PREPARE_ARGS=("$APP_DIR" "$ARCH")
  if [[ -n "$CORE_REPO" ]]; then PREPARE_ARGS+=("$CORE_REPO"); fi
  node "$TARGET_DIR/prepare.mjs" "${PREPARE_ARGS[@]}"

  mkdir -p "$PROJECT_DIR/RPMS"
  echo "==> Building & validating RPM inside Docker volume $DOCKER_VOLUME ($DOCKER_IMAGE)"
  docker run --rm --platform linux/arm64 \
    --cap-add SYS_ADMIN \
    -v "$DOCKER_VOLUME:/opt/build" \
    -v "$PROJECT_DIR:/mnt/stage:ro" \
    -v "$PROJECT_DIR/RPMS:/mnt/rpms" \
    -e "PKG_NAME=$PKG_NAME" \
    -e "ARCH=$ARCH" \
    --entrypoint bash "$DOCKER_IMAGE" -c '
set -euo pipefail
WORK_REL="tmp/gea-rpm-$$"
WORK_HOST="/opt/build/$WORK_REL"
WORK_CHROOT="/$WORK_REL"
MOUNTED_PROC=0
CREATED_DEV_FD=0
cleanup() {
  if [[ "$CREATED_DEV_FD" -eq 1 ]]; then rm -f /opt/build/dev/fd; fi
  if [[ "$MOUNTED_PROC" -eq 1 ]]; then umount /opt/build/proc 2>/dev/null || true; fi
  rm -rf "$WORK_HOST"
}
trap cleanup EXIT

if mount -t proc proc /opt/build/proc 2>/dev/null; then
  MOUNTED_PROC=1
  if [[ ! -e /opt/build/dev/fd ]]; then
    ln -s /proc/self/fd /opt/build/dev/fd
    CREATED_DEV_FD=1
  fi
fi

mkdir -p "$WORK_HOST/rpmbuild"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} "$WORK_HOST/inspect"
cp -a /mnt/stage "$WORK_HOST/project"

chroot /opt/build bash -s "$WORK_CHROOT" "$PKG_NAME" "$ARCH" <<'\''INNER'\''
set -euo pipefail
WORK="$1"
PKG_NAME="$2"
ARCH="$3"
cd "$WORK/project"
rpmbuild -bb --build-in-place --define "_topdir $WORK/rpmbuild" "rpm/$PKG_NAME.spec"
INNER

BUILT_RPM="$(find "$WORK_HOST/rpmbuild/RPMS/$ARCH" -maxdepth 1 -name "$PKG_NAME-*.$ARCH.rpm" ! -name "*-debuginfo-*" ! -name "*-debugsource-*" | head -n 1)"
[[ -n "$BUILT_RPM" && -f "$BUILT_RPM" ]] || { echo "No main RPM produced in $WORK_HOST/rpmbuild/RPMS/$ARCH" >&2; exit 1; }
cp -f "$BUILT_RPM" /mnt/rpms/
RPM_CHROOT="$WORK_CHROOT/rpmbuild/RPMS/$ARCH/$(basename "$BUILT_RPM")"

echo "==> Inspecting RPM payload & dependencies"
chroot /opt/build rpm -qpl "$RPM_CHROOT"
echo "==> RPM requires"
chroot /opt/build rpm -qp --requires "$RPM_CHROOT"

chroot /opt/build bash -s "$WORK_CHROOT" "$RPM_CHROOT" "$PKG_NAME" <<'\''INSPECT'\'' || true
cd "$1/inspect" && rpm2cpio "$2" | cpio -id --quiet "./usr/bin/$3"
INSPECT
BIN_STAGE="$(find "$WORK_HOST/inspect" -path "*/usr/bin/$PKG_NAME" -type f | head -n 1 || true)"
if [[ -n "$BIN_STAGE" ]]; then
  echo "==> ELF inspection ($PKG_NAME)"
  readelf -h "$BIN_STAGE" | grep -E "Class:|Machine:|Type:" || true
  readelf -l "$BIN_STAGE" | sed -n "/interpreter/p" || true
  readelf -d "$BIN_STAGE" | sed -n "/NEEDED/p;/RPATH/p;/RUNPATH/p" || true
fi

echo "==> Running official Jolla Harbour validator (rpmvalidation.sh)"
set +e
chroot /opt/build /usr/libexec/sdk-harbour-rpmvalidator/rpmvalidation.sh "$RPM_CHROOT"
VAL_RC=$?
set -e
echo "rpmvalidation.sh exit code: $VAL_RC (0=pass, 2=pass with warnings, 1=fail)"
if [[ "$VAL_RC" -eq 1 ]]; then
  exit 1
fi
'
  RPM_PATH="$(find "$PROJECT_DIR/RPMS" -maxdepth 1 -name "${PKG_NAME}-*.${ARCH}.rpm" ! -name '*-debuginfo-*' ! -name '*-debugsource-*' | head -n 1)"
  [[ -n "$RPM_PATH" && -f "$RPM_PATH" ]] || { echo "Copied RPM not found under $PROJECT_DIR/RPMS" >&2; exit 1; }
  echo "==> Verified RPM: $RPM_PATH"
}

if [[ -n "$DOCKER_VOLUME" && -z "$RPM_PATH" ]]; then
  run_docker_chroot_verify
else
  if [[ -z "$RPM_PATH" ]]; then
    echo "==> Building $PKG_NAME for SailfishOS-5.1.0.11-$ARCH"
    BUILD_ARGS=("$APP_DIR" --arch "$ARCH" --sfdk "$SFDK_BIN")
    if [[ -n "$CORE_REPO" ]]; then BUILD_ARGS+=(--core-repo "$CORE_REPO"); fi
    "$TARGET_DIR/build-sailfish-os.sh" "${BUILD_ARGS[@]}"
    RPM_PATH="$(find "$PROJECT_DIR/RPMS" -maxdepth 1 -name "${PKG_NAME}-*.${ARCH}.rpm" | head -n 1)"
  fi

  [[ -n "$RPM_PATH" && -f "$RPM_PATH" ]] || { echo "RPM not found for $PKG_NAME ($ARCH)" >&2; exit 1; }
  echo "==> RPM: $RPM_PATH"

  if command -v rpm >/dev/null 2>&1; then
    echo "==> Checking RPM payload (rpm -qpl)"
    RPM_FILES="$(rpm -qpl "$RPM_PATH")"
    echo "$RPM_FILES"
    for expected in \
      "/usr/bin/$PKG_NAME" \
      "/usr/share/applications/$PKG_NAME.desktop" \
      "/usr/share/icons/hicolor/86x86/apps/$PKG_NAME.png" \
      "/usr/share/icons/hicolor/108x108/apps/$PKG_NAME.png" \
      "/usr/share/icons/hicolor/128x128/apps/$PKG_NAME.png" \
      "/usr/share/icons/hicolor/172x172/apps/$PKG_NAME.png"
    do
      grep -Fxq "$expected" <<< "$RPM_FILES" || { echo "Missing expected RPM file: $expected" >&2; exit 1; }
    done
    echo "==> RPM requires (rpm -qp --requires)"
    rpm -qp --requires "$RPM_PATH"
  fi

  DESKTOP_FILE="$PROJECT_DIR/$PKG_NAME.desktop"
  if [[ -f "$DESKTOP_FILE" ]] && command -v desktop-file-validate >/dev/null 2>&1; then
    echo "==> Validating desktop entry"
    desktop-file-validate "$DESKTOP_FILE"
  fi

  command -v "$SFDK_BIN" >/dev/null 2>&1 || { echo "sfdk not found ($SFDK_BIN) — cannot run Harbour check" >&2; exit 1; }
  echo "==> Running Harbour QA: sfdk -c target=SailfishOS-5.1.0.11-$ARCH check"
  set +e
  CHECK_OUT="$("$SFDK_BIN" -c "target=SailfishOS-5.1.0.11-$ARCH" check "$RPM_PATH" 2>&1)"
  CHECK_RC=$?
  set -e
  echo "$CHECK_OUT"
  if grep -qi "maliit" <<< "$CHECK_OUT"; then
    echo ""
    echo "NOTE: Harbour validator flagged maliit-glib. Migrate targets/sailfish-os/main/sailfish_keyboard.cpp"
    echo "      to direct D-Bus (org.maliit.server) or bundle libmaliit-glib under /usr/share/$PKG_NAME/lib."
  fi
  (( CHECK_RC == 0 )) || { echo "Harbour QA check failed (exit $CHECK_RC)" >&2; exit "$CHECK_RC"; }
  echo "PASS Harbour QA check: 0 errors"
fi

if [[ -n "$DEVICE" ]]; then
  REMOTE_RPM="/tmp/$(basename "$RPM_PATH")"
  echo "==> Copying RPM to $DEVICE:$REMOTE_RPM"
  scp "$RPM_PATH" "$DEVICE:$REMOTE_RPM"
  echo "==> Installing via pkcon on device"
  ssh "$DEVICE" "pkcon install-local -y '$REMOTE_RPM' && file /usr/bin/$PKG_NAME && ldd /usr/bin/$PKG_NAME"
  cat <<EOF

==> Run these interactive checks on $DEVICE (under real Sailjail):
  1. Launch with perf + audio + scroll diagnostics:
     GEA_LOOP_PERF=1 GEA_AUDIO_DEBUG=1 GEA_SCROLL_TRACE=1 sailjail -p $PKG_NAME.desktop /usr/bin/$PKG_NAME
  2. Verify startup log:
     - '[sailfish] init ... dpr=2' (720px panel) or 'dpr=3' (1080px panel)
     - '[sailfish audio] started driver=pulseaudio ... media.role=x-maemo'
     - '[sailfish keyboard] Maliit ready'
  3. Verify on screen:
     - Tap/scroll list -> zero '[sc.qdrop]' lines
     - Focus text & password inputs -> Maliit opens/hides cleanly, DPR stays unchanged
     - Press hardware volume keys during audio playback -> volume responds immediately
     - Minimize to cover, return, swipe-close -> '[sailfish] shutting down' with exit 0
EOF
fi
