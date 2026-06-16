#!/bin/bash
# trakio Pi display diagnostic
# Usage: ./check.sh [ws://HOST:9001]
#   Pass the app's WS URL, or it reads the one saved by the display.
#
# Checks, in order:
#   1. chromium binary present
#   2. DISPLAY / X available (kiosk needs a desktop session)
#   3. app host reachable on the network
#   4. TCP port 9001 open on the app
#   5. live WebSocket — prints messages so you can confirm route_start arrives

set -u
SCRIPT_DIR="$(dirname "$(realpath "$0")")"

# ── Resolve the WS URL ──────────────────────────────────────────
WS_URL="${1:-}"
if [ -z "$WS_URL" ]; then
  # Try the value the display saved to Chromium's localStorage (best-effort)
  LSDB=$(find "$HOME/.config/chromium" -name "*.localstorage" 2>/dev/null | head -1)
  WS_URL=$(strings "$LSDB" 2>/dev/null | grep -oE 'ws://[0-9.]+:[0-9]+' | head -1)
fi
WS_URL="${WS_URL:-ws://localhost:9001}"

HOSTPORT="${WS_URL#ws://}"          # strip scheme
HOST="${HOSTPORT%%:*}"
PORT="${HOSTPORT##*:}"
[ "$PORT" = "$HOST" ] && PORT=9001  # no port given

pass(){ printf '  \033[32m✓\033[0m %s\n' "$1"; }
fail(){ printf '  \033[31m✗\033[0m %s\n' "$1"; }
info(){ printf '  \033[36m•\033[0m %s\n' "$1"; }

echo "════════════════════════════════════════════"
echo " trakio Pi display check"
echo " WS target: $WS_URL   (host=$HOST port=$PORT)"
echo "════════════════════════════════════════════"

# ── 1. chromium ─────────────────────────────────────────────────
echo "[1] Chromium binary"
CHROMIUM="$(command -v chromium-browser || command -v chromium)"
if [ -n "$CHROMIUM" ]; then pass "found: $CHROMIUM"
else fail "no chromium/chromium-browser in PATH — sudo apt install chromium"; fi

# ── 2. display/X session ────────────────────────────────────────
echo "[2] Graphical session"
if [ -n "${DISPLAY:-}" ]; then pass "DISPLAY=$DISPLAY"
else fail "DISPLAY not set — kiosk needs a desktop session (run from the Pi's screen, not pure SSH)"; fi

# ── 3. host reachable ───────────────────────────────────────────
echo "[3] App host reachable ($HOST)"
if ping -c1 -W2 "$HOST" >/dev/null 2>&1; then pass "ping OK"
else fail "no ping reply — wrong IP, or Pi not on same network as the app"; fi

# ── 4. port 9001 open ───────────────────────────────────────────
echo "[4] Port $PORT open on $HOST"
if command -v nc >/dev/null 2>&1; then
  if nc -z -w3 "$HOST" "$PORT" 2>/dev/null; then pass "port open"
  else fail "port closed — is the app's 'Pi Display' streaming toggled ON? (must show 'Live on :9001')"; fi
elif timeout 3 bash -c "exec 3<>/dev/tcp/$HOST/$PORT" 2>/dev/null; then
  pass "port open"
else
  fail "port closed/unreachable — enable Pi Display streaming in the app"
fi

# ── 5. live WebSocket ───────────────────────────────────────────
echo "[5] Live WebSocket messages"
info "Listening 20s — now press 'Start Ride' in the app..."
if command -v python3 >/dev/null 2>&1 && python3 -c "import websockets" 2>/dev/null; then
  python3 - "$WS_URL" <<'PY'
import asyncio, sys, websockets
url = sys.argv[1]
async def main():
    try:
        async with websockets.connect(url, open_timeout=5) as ws:
            print("  \033[32m✓\033[0m connected — waiting for data")
            try:
                while True:
                    msg = await asyncio.wait_for(ws.recv(), timeout=20)
                    print("  \033[36m←\033[0m", msg[:160])
            except asyncio.TimeoutError:
                print("  \033[33m!\033[0m connected but no messages in 20s "
                      "(toggle streaming ON, then press Start Ride)")
    except Exception as e:
        print("  \033[31m✗\033[0m connect failed:", e)
asyncio.run(main())
PY
elif command -v websocat >/dev/null 2>&1; then
  pass "using websocat"
  timeout 20 websocat -n1 "$WS_URL" || info "no messages (press Start Ride while this runs)"
else
  fail "no WS client — install one to see messages:"
  info "sudo apt install python3-websockets   (or:  sudo apt install websocat)"
fi

echo "════════════════════════════════════════════"
echo " Done. All ✓ above = Pi side is fine."
echo " Reminder: app must (a) toggle Pi Display ON,"
echo " then (b) you press Start Ride."
echo "════════════════════════════════════════════"
