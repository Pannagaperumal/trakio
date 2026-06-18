#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
PAIR_DIR="/tmp/trakio-pairing"
QR_PATH="${PAIR_DIR}/pi-bt-mac.png"

mkdir -p "$PAIR_DIR"

urlencode() {
  python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$1"
}

detect_mac() {
  bluetoothctl show 2>/dev/null | awk '/Controller/ { print $2; exit }'
}

MAC="$(detect_mac || true)"
if [ -z "$MAC" ]; then
  echo "Could not determine the Pi Bluetooth controller MAC." >&2
  echo "Make sure Bluetooth is enabled and bluetoothctl is installed." >&2
  exit 1
fi

QR_QUERY=""
if command -v qrencode >/dev/null 2>&1; then
  qrencode -o "$QR_PATH" -s 10 -m 2 "$MAC"
  QR_QUERY="&qr=$(urlencode "file://${QR_PATH}")"
else
  echo "qrencode not found; continuing with MAC-only pairing screen." >&2
fi

PAGE="file://${SCRIPT_DIR}/pair.html?mac=$(urlencode "$MAC")${QR_QUERY}"

export DISPLAY=:0
xset s off 2>/dev/null || true
xset -dpms 2>/dev/null || true
xset s noblank 2>/dev/null || true

CHROMIUM="$(command -v chromium-browser || command -v chromium)"
if [ -z "$CHROMIUM" ]; then
  echo "ERROR: no chromium binary found (tried chromium-browser, chromium)" >&2
  exit 1
fi

pkill -f "chromium" 2>/dev/null || true
sleep 1

exec "$CHROMIUM" \
  --kiosk \
  --noerrdialogs \
  --disable-infobars \
  --disable-session-crashed-bubble \
  --disable-restore-session-state \
  --disable-component-update \
  --no-first-run \
  --no-default-browser-check \
  "$PAGE"