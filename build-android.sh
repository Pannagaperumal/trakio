#!/usr/bin/env bash
#
# build-android.sh — Build & sign the Trakio Tauri app for Android.
#
# What it does:
#   1. Resolves the Android SDK / NDK / JDK toolchain (auto-detect or env override).
#   2. Generates a release keystore on first run (idempotent — reused afterwards).
#   3. Writes src-tauri/gen/android/keystore.properties so Gradle can sign release builds.
#   4. Runs `tauri android build`, producing signed APK(s) and an AAB.
#   5. Prints the location of every artifact it produced.
#
# Usage:
#   ./build-android.sh                # signed release APK + AAB (default)
#   ./build-android.sh --apk          # signed release APK only
#   ./build-android.sh --aab          # signed release AAB only (for Play Store)
#   ./build-android.sh --debug        # unsigned debug APK (no keystore needed)
#   ./build-android.sh --clean        # wipe gradle build outputs first
#
# Config (override via env or a .env.android file next to this script):
#   ANDROID_HOME      Android SDK location      (default: ~/Android/Sdk)
#   NDK_HOME          Android NDK location      (default: autodetected under $ANDROID_HOME/ndk)
#   JAVA_HOME         JDK 17 location           (default: system java)
#   KEYSTORE_PATH     Keystore file location    (default: src-tauri/keystore/trakio-release.jks)
#   KEYSTORE_PASS     Keystore + key password   (prompted/generated if unset)
#   KEY_ALIAS         Key alias                 (default: trakio)
#   KEY_DNAME         Cert distinguished name   (default: CN=Trakio,O=Pannaga,C=IN)
#
set -euo pipefail

# ----------------------------------------------------------------------------
# Locate project paths
# ----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ANDROID_PROJECT_DIR="$SCRIPT_DIR/src-tauri/gen/android"
KEYSTORE_PROPERTIES="$ANDROID_PROJECT_DIR/keystore.properties"

# Load optional local config
[ -f "$SCRIPT_DIR/.env.android" ] && source "$SCRIPT_DIR/.env.android"

# ----------------------------------------------------------------------------
# Defaults
# ----------------------------------------------------------------------------
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
export ANDROID_HOME
export ANDROID_SDK_ROOT="$ANDROID_HOME"

if [ -z "${NDK_HOME:-}" ]; then
  if [ -d "$ANDROID_HOME/ndk" ]; then
    # pick the highest-versioned NDK installed
    NDK_HOME="$ANDROID_HOME/ndk/$(ls -1 "$ANDROID_HOME/ndk" | sort -V | tail -1)"
  fi
fi
export NDK_HOME

# JAVA_HOME: fall back to resolving the system java
if [ -z "${JAVA_HOME:-}" ] && command -v java >/dev/null 2>&1; then
  JAVA_BIN="$(readlink -f "$(command -v java)")"
  JAVA_HOME="$(dirname "$(dirname "$JAVA_BIN")")"
fi
export JAVA_HOME

KEYSTORE_PATH="${KEYSTORE_PATH:-$SCRIPT_DIR/src-tauri/keystore/trakio-release.jks}"
KEY_ALIAS="${KEY_ALIAS:-trakio}"
KEY_DNAME="${KEY_DNAME:-CN=Trakio,O=Pannaga,C=IN}"

# ----------------------------------------------------------------------------
# Args
# ----------------------------------------------------------------------------
BUILD_MODE="release"     # release | debug
TARGETS=()               # extra flags passed to tauri android build
DO_CLEAN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --apk)   TARGETS+=("--apk") ;;
    --aab)   TARGETS+=("--aab") ;;
    --debug) BUILD_MODE="debug" ;;
    --clean) DO_CLEAN=1 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//' | head -30
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------
log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# ----------------------------------------------------------------------------
# Preflight checks
# ----------------------------------------------------------------------------
log "Toolchain"
echo "    ANDROID_HOME = $ANDROID_HOME"
echo "    NDK_HOME     = ${NDK_HOME:-<unset>}"
echo "    JAVA_HOME    = ${JAVA_HOME:-<unset>}"

[ -d "$ANDROID_HOME" ]            || die "Android SDK not found at $ANDROID_HOME (set ANDROID_HOME)"
[ -n "${NDK_HOME:-}" ] && [ -d "$NDK_HOME" ] || die "Android NDK not found (set NDK_HOME)"
command -v keytool >/dev/null     || die "keytool not found (install a JDK)"
command -v npm >/dev/null         || die "npm not found"

# Ensure rust android targets are present
if command -v rustup >/dev/null 2>&1; then
  for t in aarch64-linux-android armv7-linux-androideabi i686-linux-android x86_64-linux-android; do
    if ! rustup target list --installed 2>/dev/null | grep -q "^$t$"; then
      log "Installing rust target $t"
      rustup target add "$t"
    fi
  done
fi

# Ensure JS deps are installed (tauri build runs `npm run build` first)
if [ ! -d "$SCRIPT_DIR/node_modules" ]; then
  log "Installing npm dependencies"
  npm install
fi

# ----------------------------------------------------------------------------
# Keystore + signing config (release only)
# ----------------------------------------------------------------------------
if [ "$BUILD_MODE" = "release" ]; then
  if [ ! -f "$KEYSTORE_PATH" ]; then
    log "No keystore found — generating one at $KEYSTORE_PATH"
    mkdir -p "$(dirname "$KEYSTORE_PATH")"

    if [ -z "${KEYSTORE_PASS:-}" ]; then
      read -r -s -p "    Create a keystore password (min 6 chars): " KEYSTORE_PASS; echo
      [ -n "$KEYSTORE_PASS" ] || die "Empty password"
      read -r -s -p "    Confirm password: " KEYSTORE_PASS_CONFIRM; echo
      [ "$KEYSTORE_PASS" = "$KEYSTORE_PASS_CONFIRM" ] || die "Passwords do not match"
    fi

    keytool -genkeypair \
      -keystore "$KEYSTORE_PATH" \
      -alias "$KEY_ALIAS" \
      -keyalg RSA -keysize 2048 -validity 10000 \
      -storepass "$KEYSTORE_PASS" -keypass "$KEYSTORE_PASS" \
      -dname "$KEY_DNAME"
    log "Keystore created. KEEP IT SAFE — losing it means you can't update the app on the Play Store."
  else
    log "Using existing keystore at $KEYSTORE_PATH"
    if [ -z "${KEYSTORE_PASS:-}" ]; then
      read -r -s -p "    Keystore password: " KEYSTORE_PASS; echo
    fi
  fi

  log "Writing $KEYSTORE_PROPERTIES"
  cat > "$KEYSTORE_PROPERTIES" <<EOF
storeFile=$KEYSTORE_PATH
storePassword=$KEYSTORE_PASS
keyAlias=$KEY_ALIAS
keyPassword=$KEYSTORE_PASS
EOF
fi

# ----------------------------------------------------------------------------
# Clean
# ----------------------------------------------------------------------------
if [ "$DO_CLEAN" = "1" ]; then
  log "Cleaning previous Android build outputs"
  rm -rf "$ANDROID_PROJECT_DIR/app/build"
fi

# ----------------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------------
BUILD_ARGS=()
[ "$BUILD_MODE" = "debug" ] && BUILD_ARGS+=("--debug")
[ "${#TARGETS[@]}" -gt 0 ] && BUILD_ARGS+=("${TARGETS[@]}")

log "Building Android app ($BUILD_MODE) ${BUILD_ARGS[*]:-}"
if [ "${#BUILD_ARGS[@]}" -gt 0 ]; then
  npm run tauri android build -- "${BUILD_ARGS[@]}"
else
  npm run tauri android build
fi

# ----------------------------------------------------------------------------
# Report artifacts
# ----------------------------------------------------------------------------
OUT_DIR="$ANDROID_PROJECT_DIR/app/build/outputs"
log "Build complete. Artifacts:"
if [ -d "$OUT_DIR" ]; then
  find "$OUT_DIR" \( -name "*.apk" -o -name "*.aab" \) -print | while read -r f; do
    printf '    %s\n' "$f"
  done
else
  warn "No outputs directory found at $OUT_DIR"
fi
