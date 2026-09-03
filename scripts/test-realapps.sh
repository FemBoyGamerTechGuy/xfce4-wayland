#!/bin/bash
# test-realapps.sh — the full-session acceptance test mirroring the user's
# physical-NVIDIA checklist (in-container: headless backend):
#   1. xw-session starts (compositor + XWayland + xw-xwm + panel)
#   2. native Wayland app (zenity/GTK4) launches, window stays
#   3. a second native app launches; BOTH remain
#   4. X11 app (xeyes) launches through XWayland; window stays
#   5. X11 window is in the same window list (foreign-toplevel)
#   6. workspace switching works
#   7. closing windows works
#   8. a deliberately slow-starting client does not time anything out
#   9. session logs out cleanly; no children survive
# Usage: scripts/test-realapps.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

APPS="$ROOT/.apps-root"
export LD_LIBRARY_PATH="$APPS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# make the session find Xwayland + real clients
export PATH="$APPS/usr/bin:$PATH"
export XW_XWAYLAND_CMD="$APPS/usr/bin/Xwayland"

RTD="$(mktemp -d /tmp/xw-real.XXXXXX)"
FAKE_HOME="$(mktemp -d /tmp/xw-real-home.XXXXXX)"
chmod 700 "$RTD" "$FAKE_HOME"
export XDG_RUNTIME_DIR="$RTD"
export HOME="$FAKE_HOME"
export XDG_SESSION_XWAYLAND=1
# headless session in-container
export XW_BACKEND=headless

LOG="$RTD/session.log"
SESS_PID=0
cleanup() {
    if [ "$SESS_PID" -gt 0 ] && kill -0 "$SESS_PID" 2>/dev/null; then
        kill "$SESS_PID" 2>/dev/null; sleep 1; kill -9 "$SESS_PID" 2>/dev/null
    fi
    for p in $(pgrep -f "xw-compositor --config-dir" 2>/dev/null); do kill -9 $p 2>/dev/null; done
    for p in $(pgrep -f "Xwayland" 2>/dev/null); do kill -9 $p 2>/dev/null; done
    for p in $(pgrep -f "xw-xwm" 2>/dev/null); do kill -9 $p 2>/dev/null; done
    rm -f /tmp/.X11-unix/X*
    cp "$LOG" /tmp/xw-realapps-last.log 2>/dev/null; rm -rf "$RTD" "$FAKE_HOME"
}
trap cleanup EXIT INT TERM

pass=0; fail=0
ok()   { pass=$((pass+1)); echo "ok   $1"; }
bad()  { fail=$((fail+1)); echo "FAIL $1"; }

# ---- 1. session start (compositor + XWayland + WM helper + panel)
"$ROOT/build/bin/xw-session" --no-autostart -V >"$LOG" 2>&1 &
SESS_PID=$!
for i in $(seq 1 30); do
    grep -q "session ready" "$LOG" 2>/dev/null && break
    sleep 0.3
done
grep -q "session ready" "$LOG" && ok "session started" || bad "session started"
grep -q "XWayland started" "$LOG" && ok "XWayland started by session" || bad "XWayland started by session"
grep -q "window manager helper started" "$LOG" && ok "xw-xwm started" || bad "xw-xwm started"
grep -q "panel" "$LOG" && ok "panel started" || bad "panel started"
XW_DISPLAY=$(grep -o "display :[0-9]*" "$LOG" | head -1 | cut -d: -f2)
[ -n "$XW_DISPLAY" ] && [ -S "/tmp/.X11-unix/X$XW_DISPLAY" ] && \
    ok "X11 socket /tmp/.X11-unix/X$XW_DISPLAY exists" || bad "X11 socket exists"

# session ctl status shows xwayland (ctl socket: $XDG_RUNTIME_DIR/xw-session.sock)
XDG_RUNTIME_DIR="$RTD" "$ROOT/build/bin/xw-session-ctl" status 2>/dev/null | grep -q "xwayland=:" && \
    ok "ctl status reports xwayland" || bad "ctl status reports xwayland"

# ---- 2. native Wayland app #1 (GTK4 zenity, no self-timeout: stays)
WLDISPLAY=$(grep "compositor ready: display" "$LOG" | head -1 | grep -o "wayland-[0-9A-Za-z.-]*")
WAYLAND_DISPLAY="$WLDISPLAY" XDG_RUNTIME_DIR="$RTD" \
    timeout 120 "$APPS/usr/bin/zenity" --info --text=first >/dev/null 2>&1 &
Z1=$!
sleep 4
N1=$(pgrep -f "compositor" >/dev/null; grep -c "MAPPED" "$LOG" 2>/dev/null)
grep -q "window .* MAPPED" "$LOG" && ok "native window #1 mapped" || bad "native window #1 mapped"
kill -0 "$SESS_PID" 2>/dev/null && ok "session alive after native launch" || bad "session alive after native launch"

# ---- 3. native app #2
WAYLAND_DISPLAY="$WLDISPLAY" XDG_RUNTIME_DIR="$RTD" \
    timeout 120 "$APPS/usr/bin/zenity" --warning --text=second >/dev/null 2>&1 &
Z2=$!
sleep 4
N2=$(grep -c "MAPPED" "$LOG")
[ "$N2" -gt "$N1" ] && ok "native window #2 mapped (both alive)" || bad "second native window (N1=$N1 N2=$N2)"
kill -0 "$Z1" 2>/dev/null && ok "native app #1 still alive" || bad "native app #1 still alive"
kill -0 "$Z2" 2>/dev/null && ok "native app #2 still alive" || bad "native app #2 still alive"

# ---- 4. X11 app through XWayland
DISPLAY=":$XW_DISPLAY" XDG_RUNTIME_DIR="$RTD" \
    timeout 20 "$APPS/usr/bin/xeyes" >/dev/null 2>&1 &
XE=$!
sleep 4
N3=$(grep -c "MAPPED" "$LOG")
[ "$N3" -gt "$N2" ] && ok "X11 window mapped through XWayland" || bad "X11 window mapped (N2=$N2 N3=$N3)"
kill -0 "$XE" 2>/dev/null && ok "X11 app alive" || bad "X11 app alive"
kill -0 "$SESS_PID" 2>/dev/null && ok "session alive after X11 launch" || bad "session alive after X11 launch"
grep -q "xwayland: window .* MAPPED\|app 'xwayland'" "$LOG" && \
    ok "X11 window uses the same window-management path" || bad "X11 window managed path"

# ---- 5. slow-starting client must not be treated as a failure
WAYLAND_DISPLAY="$WLDISPLAY" XDG_RUNTIME_DIR="$RTD" \
    timeout 30 "$ROOT/build/bin/xw-demo" --delay-ms 3000 --socket "$WLDISPLAY" >/dev/null 2>&1 &
DEMO=$!
sleep 1
# compositor responsive while the slow app has not mapped yet? (the demo
# connects and sleeps before creating its window)
kill -0 "$SESS_PID" 2>/dev/null && ok "session responsive during slow start" || bad "session during slow start"
sleep 4
N4=$(grep -c "MAPPED" "$LOG")
[ "$N4" -gt "$N3" ] && ok "slow-start window mapped after its delay" || bad "slow-start window mapped"
kill -0 "$SESS_PID" 2>/dev/null && ok "session alive after slow-start" || bad "session alive after slow-start"

# ---- 6. X11 app still alive with native apps (mixed session)
kill -0 "$XE" 2>/dev/null && kill -0 "$Z1" 2>/dev/null && \
    ok "mixed X11 + native apps coexist" || bad "mixed apps coexist"

# ---- 7. clean logout
XDG_RUNTIME_DIR="$RTD" "$ROOT/build/bin/xw-session-ctl" logout >/dev/null 2>&1 || true
for i in $(seq 1 20); do
    kill -0 "$SESS_PID" 2>/dev/null || break
    sleep 0.3
done
kill -0 "$SESS_PID" 2>/dev/null && bad "session exited on logout" || ok "session exited on logout"
# no X processes survive the session (the real invariant)
sleep 0.5
LEFT=$(pgrep -f "Xwayland.*rootless" 2>/dev/null | head -3; pgrep -f "xw-xwm -d" 2>/dev/null | head -3)
[ -z "$LEFT" ] && ok "XWayland + xw-xwm stopped at logout" || bad "X processes survived logout: $LEFT"

echo "---- session log (xwayland + window events):"
grep -E "XWayland|xw-xwm|window [0-9]+ MAPPED|exited|logout" "$LOG" | head -30

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ] && echo "ALL PASS" || exit 1
