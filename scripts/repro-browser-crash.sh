#!/bin/bash
# repro-browser-crash.sh — the LibreWolf/Firefox interaction-crash
# reproduction (physical report: "browser opens, renders, dies the
# moment I interact with it"; session stderr shows libwayland-server's
# "error in client communication (pid N)"; browser stderr shows
# 5-6x "Exiting due to channel error.").
#
# KEY FACT (established 2026-09-06): "error in client communication"
# is printed by libwayland-server INSIDE OUR COMPOSITOR — the physical
# LibreWolf is a DIRECT native-Wayland client, not an Xwayland client.
# The crash is a Wayland-protocol-level event, so this repro runs
# firefox-esr NATIVELY against the compositor and interacts with it.
#
# Rig: Xvfb :96 -> xw-compositor -B x11 (the REAL seat chain: XTEST
# events are indistinguishable from hardware input) -> firefox-esr
# (native Wayland, MOZ_WEBRENDER_SOFTWARE=1, fresh HOME, -no-remote).
#
# Interaction battery, liveness checked after EVERY step:
#   1. hover/motion  2. click 1  3. click 2  4. wheel x4 (axis events)
#   5. keyboard 'a'+BackSpace  6. focus loss/regain (click the desktop
#   then the window again — enter/leave/serial handling)
# The compositor's stderr will contain libwayland's own protocol-error
# line ("wl_display@N: error ..." / "error in client communication")
# at the moment of death — that line IS the root cause.
#
# Usage: scripts/repro-browser-crash.sh [--keep] [--x11]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

APPS="$ROOT/.apps-root"
export LD_LIBRARY_PATH="$APPS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
XDOTOOL="$APPS/usr/bin/xdotool"
XWININFO="$APPS/usr/bin/xwininfo"

XVFB_DISPLAY=":96"
SOCK="ffcrash"
RTD="$(mktemp -d /tmp/xw-ffcrash.XXXXXX)"
FAKE_HOME="$(mktemp -d /tmp/xw-ffcrash-home.XXXXXX)"
chmod 700 "$RTD" "$FAKE_HOME"
export XDG_RUNTIME_DIR="$RTD"
export HOME="$FAKE_HOME"

KEEP=0; WANT_X11=0; WANT_A11Y=0
for a in "$@"; do
    [ "$a" = "--keep" ] && KEEP=1
    [ "$a" = "--x11" ] && WANT_X11=1
    [ "$a" = "--a11y" ] && WANT_A11Y=1
done

PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done
    rm -f /tmp/.X11-unix/X5 /tmp/.X11-unix/X96
    if [ "$KEEP" = 1 ]; then echo "artifacts kept in: $RTD (home: $FAKE_HOME)"
    else rm -rf "$RTD" "$FAKE_HOME"; fi
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*"
    for f in compositor.log ff.log; do
        echo "--- $f (tail) ---"; tail -25 "$RTD/$f" 2>/dev/null
    done
    exit 1
}
wait_for() { # wait_for <desc> <cmd...>
    local desc="$1"; shift
    for _ in $(seq 1 120); do "$@" >/dev/null 2>&1 && return 0; sleep 0.5; done
    echo "TIMEOUT waiting for: $desc"; return 1
}
alive() { [ -n "${FF_PID:-}" ] && kill -0 "$FF_PID" 2>/dev/null; }

step_check() { # step_check <step-name> — the liveness gate after each step
    if ! alive; then
        wait "$FF_PID" 2>/dev/null; FF_RC=$?
        echo "REPRODUCED: firefox DIED during step: $1 (exit=$FF_RC)"
        [ "$FF_RC" -ge 128 ] && echo "  -> signal $((FF_RC-128))"
        echo "--- ff.log tail ---"; tail -15 "$RTD/ff.log"
        echo "--- compositor.log tail (libwayland lines + wm) ---"
        grep -n -E 'error|communication|MAPPED|UNMAPPED|destroy' "$RTD/compositor.log" | tail -15
        echo "--- compositor.log last 20 raw lines ---"
        tail -20 "$RTD/compositor.log"
        exit 2
    fi
    echo "  ok: survived $1"
}

[ -x "$ROOT/build/bin/xw-compositor" ] || fail "xw-compositor not built"
[ -x "$APPS/usr/lib/firefox-esr/firefox-esr" ] || fail "firefox-esr not in .apps-root"

# 1. Xvfb ("hardware") + compositor (x11 backend: real event chain)
Xvfb "$XVFB_DISPLAY" -screen 0 1280x800x24 +extension GLX >/dev/null 2>&1 &
PIDS+=($!)
wait_for "Xvfb" env DISPLAY="$XVFB_DISPLAY" "$APPS/usr/bin/xdpyinfo" || fail "Xvfb did not come up"
DISPLAY="$XVFB_DISPLAY" "$ROOT/build/bin/xw-compositor" -B x11 -o 1200x740 \
    -s "$SOCK" -v >"$RTD/compositor.log" 2>&1 &
PIDS+=($!)
wait_for "compositor socket" [ -S "$RTD/$SOCK" ] || fail "compositor socket"
sleep 0.5

# 1b. the a11y leg: session dbus + AT-SPI registry, exactly like the
#     physical session (xfsettingsd activates org.a11y.Bus there)
DBUS_ENV=""
if [ "$WANT_A11Y" = 1 ]; then
    XDG_RUNTIME_DIR="$RTD" "$APPS/usr/bin/dbus-daemon" --session --fork \
        --print-address --address="unix:path=$RTD/session-bus" \
        >"$RTD/dbus.log" 2>&1
    BUS_ADDR="unix:path=$RTD/session-bus"
    if [ -S "$RTD/session-bus" ]; then
        DBUS_ENV="DBUS_SESSION_BUS_ADDRESS=$BUS_ADDR"
        echo "session bus: $BUS_ADDR"
        DBUS_SESSION_BUS_ADDRESS="$BUS_ADDR" XDG_RUNTIME_DIR="$RTD" \
            "$APPS/usr/libexec/at-spi-bus-launcher" --launch-immediately \
            >"$RTD/atspi.log" 2>&1 &
        PIDS+=($!)
        sleep 2
        ATSPI_BUS="$(ls "$RTD"/at-spi/bus 2>/dev/null || true)"
        echo "at-spi bus: ${ATSPI_BUS:-NOT-UP} (log: $(grep -c . "$RTD/atspi.log" 2>/dev/null || echo 0) lines)"
    else
        echo "WARN: dbus-daemon failed — running without a11y"
        cat "$RTD/dbus.log" 2>/dev/null
    fi
fi

# 2. firefox, native Wayland (the physical crash path)
FF_ENV=(WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RTD" HOME="$FAKE_HOME"
        MOZ_WEBRENDER_SOFTWARE=1 LD_LIBRARY_PATH="$LD_LIBRARY_PATH")
if [ "$WANT_X11" = 1 ]; then
    echo "(X11 leg: WAYLAND_DISPLAY removed — not the physical path)"
    env -u WAYLAND_DISPLAY "${FF_ENV[@]:1}" \
        "$APPS/usr/bin/firefox-esr" -no-remote about:blank >"$RTD/ff.log" 2>&1 &
elif [ -n "$DBUS_ENV" ]; then
    echo "(a11y leg: firefox sees the session bus + org.a11y.Bus)"
    env "${FF_ENV[@]}" "$DBUS_ENV" \
        "$APPS/usr/bin/firefox-esr" -no-remote about:blank >"$RTD/ff.log" 2>&1 &
else
    env "${FF_ENV[@]}" \
        "$APPS/usr/bin/firefox-esr" -no-remote about:blank >"$RTD/ff.log" 2>&1 &
fi
FF_PID=$!; PIDS+=("$FF_PID")
echo "firefox pid: $FF_PID — waiting for its window ..."

# wait for the window to appear in the compositor's model
FF_GEOM=""
for _ in $(seq 1 240); do
    FF_GEOM="$(grep "wm: window.*MAPPED.*app 'firefox" "$RTD/compositor.log" 2>/dev/null | tail -1 | grep -oE '[0-9]+x[0-9]+\+[0-9]+\+[0-9]+')"
    [ -n "$FF_GEOM" ] && break
    alive || break
    sleep 0.5
done
[ -n "$FF_GEOM" ] || fail "firefox window never mapped"
W="${FF_GEOM%%x*}"; REST="${FF_GEOM#*x}"
H="${REST%%+*}"; REST="${REST#*+}"
WX="${REST%%+*}"; WY="${REST#*+}"
echo "firefox window mapped: ${W}x${H}+${WX}+${WY}"

# backend confirmation via socket inodes (readlink gives socket:[ino])
WL_INO="$(stat -c %i "$RTD/$SOCK" 2>/dev/null)"
FF_BACKEND="none"
for fd in /proc/$FF_PID/fd/*; do
    lnk="$(readlink "$fd" 2>/dev/null)"
    case "$lnk" in
        "socket:[$WL_INO]") FF_BACKEND="wayland (direct)"; break ;;
    esac
done
echo "firefox backend: $FF_BACKEND"

# 3. coordinates: model geometry == global (Xvfb) coordinates, since
#    the compositor window sits at 0,0 filling the Xvfb screen
CX=$((WX + W / 3)); CY=$((WY + H / 3))
echo "click target: $CX,$CY"
sleep 3   # let it settle (paint, first frame callbacks)

# 4. the interaction battery
echo "=== interactions (XTEST through the compositor's real input chain) ==="

echo "step 1: hover/motion across the window"
for dx in 5 30 70 120 200; do
    env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((CX + dx)) $((CY + 15)) >/dev/null 2>&1
    sleep 0.15
done
step_check "hover"

echo "step 2: click 1 (press+release) in the content area"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove "$CX" "$CY" >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 0.6
step_check "click 1"

echo "step 3: click 2"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 0.6
step_check "click 2"

echo "step 4: wheel down x4 (wl_pointer axis -> XI2/scroll in Firefox)"
for _ in 1 2 3 4; do
    env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 5 >/dev/null 2>&1
    sleep 0.25
done
step_check "wheel"

echo "step 5: keyboard 'a' + BackSpace"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" key a >/dev/null 2>&1
sleep 0.3
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" key BackSpace >/dev/null 2>&1
sleep 0.5
step_check "keyboard"

echo "step 6: focus loss + regain (click desktop, then the window)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove 1195 5 >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 0.5
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove "$CX" "$CY" >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 0.6
step_check "focus loss/regain"

echo "step 7: window drag via the title bar (interactive move)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + W / 2)) $((WY + 12)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousedown 1 >/dev/null 2>&1
sleep 0.3
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + W / 2 + 60)) $((WY + 52)) >/dev/null 2>&1
sleep 0.3
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mouseup 1 >/dev/null 2>&1
sleep 0.6
step_check "title-bar drag"

echo ""
echo "RESULT: firefox SURVIVED the whole interaction battery."
grep -c "error in client communication" "$RTD/compositor.log" 2>/dev/null | sed 's/^/libwayland client-communication errors: /'
echo "ff.log lines: $(grep -c . "$RTD/ff.log" 2>/dev/null || echo 0)"
exit 0
