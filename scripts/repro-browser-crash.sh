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
#   7. title-bar drag  8. link hover (cursor change + tooltip popup)
#   9. link click (in-page navigation) 10. right-click context menu
#   (xdg_popup + grab) 11. outside-press dismissal (the fixed double
#   popup_done path) 12. Escape dismissal 13. hamburger-menu cycle
#   14. URL bar typing (autocomplete popup + reposition) 15. drag text
#   selection 16. ctrl+a/ctrl+c (wl_data_device set_selection)
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

KEEP=0; WANT_X11=0; WANT_A11Y=0; WANT_WLDBG=0; WANT_WLSRV=0
for a in "$@"; do
    [ "$a" = "--keep" ] && KEEP=1
    [ "$a" = "--x11" ] && WANT_X11=1
    [ "$a" = "--a11y" ] && WANT_A11Y=1
    [ "$a" = "--wl" ] && WANT_WLDBG=1
    [ "$a" = "--wlsrv" ] && WANT_WLSRV=1
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
    [ "$KEEP" = 1 ] && echo "artifacts kept in: $RTD (home: $FAKE_HOME)"
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
        if [ "${WANT_WLDBG:-0}" = 1 ]; then
            echo "--- ff.log protocol-trace tail (last 80 lines, the wire just before death) ---"
            tail -80 "$RTD/ff.log"
        fi
        echo "--- compositor.log tail (libwayland lines + wm) ---"
        grep -n -E 'error|communication|MAPPED|UNMAPPED|destroy' "$RTD/compositor.log" | tail -15
        echo "--- compositor.log last 60 raw lines (server wire trace tail if --wlsrv) ---"
        tail -60 "$RTD/compositor.log"
        [ "${WANT_WLSRV:-0}" = 1 ] && { echo "--- every trace line mentioning object @47 ---"; grep -n '@47[^0-9]' "$RTD/compositor.log" | tail -25; }
        [ "$KEEP" = 1 ] && echo "artifacts kept in: $RTD (home: $FAKE_HOME)"
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
WLDBG_PREFIX=(env)   # never empty: an empty array before an assignment
                      # prefix makes bash re-parse DISPLAY=... as a COMMAND
[ "$WANT_WLSRV" = 1 ] && WLDBG_PREFIX=(env WAYLAND_DEBUG=1) && echo "(server-side wire trace ON — compositor.log is the wire log)"
"${WLDBG_PREFIX[@]}" DISPLAY="$XVFB_DISPLAY" "$ROOT/build/bin/xw-compositor" -B x11 -o 1200x740 \
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
[ "$WANT_WLDBG" = 1 ] && FF_ENV+=(WAYLAND_DEBUG=1) && echo "(libwayland protocol trace ON — ff.log is the wire log)"

# test page with a link, an input and selectable text at KNOWN viewport
# coordinates (position:fixed) so the battery can target them exactly
cat >"$FAKE_HOME/test.html" <<'HTML'
<!doctype html><html><head><meta charset="utf-8">
<style>html,body{margin:0;padding:0;background:#fff}
a#probe{position:fixed;left:40px;top:50px;font:20px sans-serif;padding:12px;display:block;background:#eef}
input#inp{position:fixed;left:40px;top:180px;width:320px;height:34px;font:16px sans-serif}
p#sel{position:fixed;left:40px;top:300px;font:24px/1.6 sans-serif;user-select:text}
</style></head><body>
<a id="probe" href="second.html" title="tooltip text here">HOVER CLICK ME</a>
<input id="inp" placeholder="type here">
<p id="sel">The quick brown fox jumps over the lazy dog 0123456789</p>
</body></html>
HTML
cat >"$FAKE_HOME/second.html" <<'HTML'
<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;background:#dde}</style></head>
<body><h1 style="font:32px sans-serif;margin:60px">SECOND PAGE navigation worked</h1>
<p style="font:20px sans-serif;margin:40px">target text for selection abcdefghijklmnop</p></body></html>
HTML
FF_URL="file://$FAKE_HOME/test.html"

FF_ENV=(WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RTD" HOME="$FAKE_HOME"
        MOZ_WEBRENDER_SOFTWARE=1 LD_LIBRARY_PATH="$LD_LIBRARY_PATH")
if [ "$WANT_X11" = 1 ]; then
    echo "(X11 leg: WAYLAND_DISPLAY removed — not the physical path)"
    env -u WAYLAND_DISPLAY "${FF_ENV[@]:1}" \
        "$APPS/usr/bin/firefox-esr" -no-remote "$FF_URL" >"$RTD/ff.log" 2>&1 &
elif [ -n "$DBUS_ENV" ]; then
    echo "(a11y leg: firefox sees the session bus + org.a11y.Bus)"
    env "${FF_ENV[@]}" "$DBUS_ENV" \
        "$APPS/usr/bin/firefox-esr" -no-remote "$FF_URL" >"$RTD/ff.log" 2>&1 &
else
    env "${FF_ENV[@]}" \
        "$APPS/usr/bin/firefox-esr" -no-remote "$FF_URL" >"$RTD/ff.log" 2>&1 &
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

# backend confirmation via ss: the OLD inode compare was bogus (a bound
# path's stat inode is the LISTENER endpoint; client fds carry their own
# endpoint inode — they can never match). ss shows the pids on the socket.
COMP_PID="${PIDS[1]}"
SS_PIDS="$(ss -Hxp 2>/dev/null | grep "$SOCK" | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -un | tr '\n' ' ')"
FF_TREE_OK=0
for p in $SS_PIDS; do
    [ "$p" = "$FF_PID" ] && FF_TREE_OK=1
    [ "$(awk '/^PPid:/{print $2}' /proc/$p/status 2>/dev/null)" = "$FF_PID" ] && FF_TREE_OK=1
done
echo "pids on \"$SOCK\": ${SS_PIDS:-none}  (compositor=$COMP_PID firefox=$FF_PID)"
echo "firefox backend: $([ "$FF_TREE_OK" = 1 ] && echo 'wayland (direct)' || echo 'UNCERTAIN — see pids above')"

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

echo "step 8: link hover — cursor change (wl_pointer.set_cursor) + tooltip"
for dx in 60 100 140 160; do
    env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + dx)) $((WY + 193)) >/dev/null 2>&1
    sleep 0.2
done
sleep 2.2   # tooltip (title attr) = non-grab xdg_popup after hover delay
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 600)) $((WY + 500)) >/dev/null 2>&1
sleep 0.3
step_check "link hover / cursor change"

echo "step 9: click the link — in-page navigation"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 100)) $((WY + 193)) >/dev/null 2>&1
sleep 0.3
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 2.5   # page load + full repaint of the new document
step_check "link navigation"

echo "step 10: right-click context menu (xdg_popup + pointer grab)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 300)) $((WY + 400)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 3 >/dev/null 2>&1
sleep 0.8
step_check "context menu open"

echo "step 11: outside-press dismissal (the fixed double-popup_done path)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 700)) $((WY + 450)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 0.8
step_check "context menu outside-dismiss"

echo "step 12: right-click again + Escape (keyboard dismissal)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 300)) $((WY + 400)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 3 >/dev/null 2>&1
sleep 0.6
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" key Escape >/dev/null 2>&1
sleep 0.6
step_check "context menu Escape-dismiss"

echo "step 13: hamburger menu cycle (popup open + outside dismiss)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + W - 30)) $((WY + 75)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 1.0
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 300)) $((WY + 450)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 0.8
step_check "hamburger menu cycle"

echo "step 14: URL bar — focus, type, autocomplete dropdown (popup + reposition)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + W / 2)) $((WY + 75)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" click 1 >/dev/null 2>&1
sleep 0.4
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" type --delay 120 second >/dev/null 2>&1
sleep 1.5   # dropdown narrows while typing -> xdg_popup.reposition()
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" key Escape >/dev/null 2>&1
sleep 0.3
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" key Escape >/dev/null 2>&1
sleep 0.5
step_check "URL bar autocomplete"

echo "step 15: drag text selection"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 60)) $((WY + 320)) >/dev/null 2>&1
sleep 0.2
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousedown 1 >/dev/null 2>&1
for dx in 80 160 240 320; do
    env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mousemove $((WX + 60 + dx)) $((WY + 325)) >/dev/null 2>&1
    sleep 0.12
done
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" mouseup 1 >/dev/null 2>&1
sleep 0.5
step_check "drag selection"

echo "step 16: ctrl+a, ctrl+c (wl_data_device.set_selection)"
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" key ctrl+a >/dev/null 2>&1
sleep 0.3
env DISPLAY="$XVFB_DISPLAY" "$XDOTOOL" key ctrl+c >/dev/null 2>&1
sleep 0.8
step_check "clipboard copy"

echo ""
echo "RESULT: firefox SURVIVED the whole interaction battery."
grep -c "error in client communication" "$RTD/compositor.log" 2>/dev/null | sed 's/^/libwayland client-communication errors: /'
echo "popup dismissals (xdg: dismissing popup): $(grep -c 'xdg: dismissing popup' "$RTD/compositor.log" 2>/dev/null || true)"
echo "pointer button events: $(grep -c 'pointer button' "$RTD/compositor.log" 2>/dev/null || true)"
echo "cursor trace lines: $(grep -c 'cursor:' "$RTD/compositor.log" 2>/dev/null || true)"
echo "ff.log lines: $(grep -c . "$RTD/ff.log" 2>/dev/null || echo 0)"
echo "--- ff.log (full) ---"
cat "$RTD/ff.log" 2>/dev/null
exit 0
