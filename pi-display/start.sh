#!/bin/bash
# trakio kiosk launcher for Raspberry Pi Zero 2 W
# Usage: ./start.sh [ws://HOST:9001]
#   If a WS URL is passed it overrides the one saved in localStorage.
#   Otherwise the URL entered on the display is remembered across reboots.

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
PAGE="file://${SCRIPT_DIR}/index.html"

WS_ARG="${1:-}"
if [ -n "$WS_ARG" ]; then
  PAGE="${PAGE}?ws=${WS_ARG}"
fi

# Disable screen blanking and DPMS
export DISPLAY=:0
xset s off    2>/dev/null || true
xset -dpms    2>/dev/null || true
xset s noblank 2>/dev/null || true

# Kill any stale Chromium instances from a previous session
pkill -f "chromium" 2>/dev/null || true
sleep 1

exec chromium-browser \
  --kiosk \
  --noerrdialogs \
  --disable-infobars \
  --disable-session-crashed-bubble \
  --disable-restore-session-state \
  --disable-component-update \
  --no-first-run \
  --no-default-browser-check \
  --disable-features=TranslateUI \
  --autoplay-policy=no-user-gesture-required \
  --check-for-update-interval=31536000 \
  "$PAGE"
