#!/bin/sh
# test-physical-kbd.sh — the X-side physical-keyboard regression.
#
# Reproduces the full physical key path as close as a container can:
# physical X keycodes (evdev+8, exactly what XWayland and the X11
# backend receive) -> compositor X keycode -8 -> seat +8 -> the wl
# wire -> a RAW Wayland keyboard client (build/tests/keyboardprobe),
# which decodes with its own xkb state like any text app must.
#
# PASS bar: the probe must report, for the injected matrix,
#   wl 22 -> BackSpace   (the "Backspace types u" shape must NOT appear)
#   wl 30 -> u, wl 38 -> a
#   exactly 16 key events, zero anomalies
#
# Usage: scripts/test-physical-kbd.sh   (from the repo root)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XVFB_DISPLAY=":97"
RTD="$(mktemp -d /tmp/xw-kbd-XXXXXX)"
LOG="$RTD/probe.log"
COMP_LOG="$RTD/compositor.log"

cleanup() {
    [ -n "${PROBE_PID:-}" ] && kill "$PROBE_PID" 2>/dev/null
    [ -n "${COMP_PID:-}" ] && kill "$COMP_PID" 2>/dev/null
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$RTD"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*"
    echo "--- probe log tail ---"
    tail -40 "$LOG" 2>/dev/null
    echo "--- compositor log tail ---"
    tail -15 "$COMP_LOG" 2>/dev/null
    exit 1
}

command -v Xvfb >/dev/null 2>&1 || fail "Xvfb not available"
[ -x "$ROOT/build/tests/keyboardprobe" ] || fail "keyboardprobe not built (make)"
[ -x "$ROOT/build/tests/kbddriver" ] || fail "kbddriver not built (make)"
[ -x "$ROOT/build/bin/xw-compositor" ] || fail "xw-compositor not built (make)"

# 1. X server + the nested-X11 compositor
Xvfb "$XVFB_DISPLAY" -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1

XDG_RUNTIME_DIR="$RTD" DISPLAY="$XVFB_DISPLAY" \
    "$ROOT/build/bin/xw-compositor" -B x11 --socket kbdtest \
    >"$COMP_LOG" 2>&1 &
COMP_PID=$!

for i in $(seq 1 50); do
    [ -S "$RTD/kbdtest" ] && break
    sleep 0.1
done
[ -S "$RTD/kbdtest" ] || fail "compositor socket never appeared"

# 2. the raw keyboard client (background, writes the wire record)
XDG_RUNTIME_DIR="$RTD" "$ROOT/build/tests/keyboardprobe" kbdtest 25 \
    >"$LOG" 2>&1 &
PROBE_PID=$!

# wait for it to map + take keyboard focus
for i in $(seq 1 100); do
    grep -q "^enter:" "$LOG" 2>/dev/null && break
    sleep 0.1
done
grep -q "^enter:" "$LOG" || fail "probe never got keyboard focus"
grep -q "wl 22 (raw 14, KEY_BACKSPACE) -> BackSpace" "$LOG" || \
    fail "keymap spot-check: wl 22 is not BackSpace in the delivered keymap"

# 3. drive the matrix through the X keycode space
DISPLAY="$XVFB_DISPLAY" "$ROOT/build/tests/kbddriver" \
    "XFCE-Wayland (nested X11)" || fail "kbddriver could not find/drive the window"

# 4. let the probe record everything, then stop it cleanly (the
#    summary + anomaly count print on exit)
sleep 1.5
kill -INT "$PROBE_PID" 2>/dev/null
for i in $(seq 1 30); do
    grep -q "^summary:" "$LOG" 2>/dev/null && break
    sleep 0.1
done
kill "$PROBE_PID" 2>/dev/null
wait "$PROBE_PID" 2>/dev/null

# 5. the assertions — the exact wire stream
grep -q "wl=22 press -> client decodes: keysym BackSpace" "$LOG" || \
    fail "Backspace press did not decode as BackSpace (the physical bug shape)"
grep -q "wl=22 release -> client decodes: keysym BackSpace" "$LOG" || \
    fail "Backspace release did not decode as BackSpace"
grep -q "wl=30 press -> client decodes: keysym u text=" "$LOG" || \
    fail "u key did not decode as u"
grep -q "wl=38 press -> client decodes: keysym a text=" "$LOG" || \
    fail "a key did not decode as a"
grep -q "anomalies=0" "$LOG" || \
    fail "probe reported anomalies (double delivery / serial / pairing): $(grep '^ANOMALY' "$LOG" | head -3)"
NKEY=$(grep -c "^key: serial=" "$LOG")
[ "$NKEY" -eq 16 ] || fail "probe saw $NKEY key events, expected 16"

echo "PASS: physical key matrix through the X keycode space"
echo "      wl 22 = BackSpace (not 'u'), wl 30 = u, wl 38 = a,"
echo "      16/16 events, 0 anomalies"
exit 0
