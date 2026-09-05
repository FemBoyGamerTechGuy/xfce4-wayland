#!/bin/bash
# repro-firefox.sh — the physical LibreWolf-crash reproduction, in
# container: a REAL Firefox (firefox-esr, the LibreWolf upstream) as a
# native Wayland client of a headless xw-session, with its own stderr
# captured separately from the session log (on the physical box both
# went to one sink, which is why the death looked silent).
#
# PASS bar (the fix is in): firefox stays alive 30s+, maps a window,
# no wl_display error, no "error in client communication" from the
# server, clean logout afterwards.
#
# Usage: scripts/repro-firefox.sh [seconds]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

APPS="$ROOT/.apps-root"
export LD_LIBRARY_PATH="$APPS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$APPS/usr/bin:$PATH"
export XW_XWAYLAND_CMD="$APPS/usr/bin/Xwayland"
SECS="${1:-40}"

RTD="$(mktemp -d /tmp/xw-ff.XXXXXX)"
FAKE_HOME="$(mktemp -d /tmp/xw-ff-home.XXXXXX)"
PROFILE="$(mktemp -d /tmp/xw-ff-prof.XXXXXX)"
chmod 700 "$RTD" "$FAKE_HOME" "$PROFILE"
export XDG_RUNTIME_DIR="$RTD"
export HOME="$FAKE_HOME"
export XDG_SESSION_XWAYLAND=1
export XW_BACKEND=headless
# firefox must take the WAYLAND path, not X11 (the physical LibreWolf
# was a direct Wayland client: pid-visible in the server's client-death
# line, xdg-activation on the wire)
export MOZ_ENABLE_WAYLAND=1
unset DISPLAY 2>/dev/null
unset MOZ_HEADLESS 2>/dev/null
# container-friendliness (content sandbox needs namespaces the
# container may not grant; none of this changes the wl client path)
export MOZ_DISABLE_CONTENT_SANDBOX=1
export MOZ_DISABLE_GPU_SANDBOX=1

LOG="$RTD/session.log"
FF_LOG="$RTD/firefox.log"
SESS_PID=0
FF_PID=0
cleanup() {
    [ "$FF_PID" -gt 0 ] && kill "$FF_PID" 2>/dev/null
    if [ "$SESS_PID" -gt 0 ] && kill -0 "$SESS_PID" 2>/dev/null; then
        kill "$SESS_PID" 2>/dev/null; sleep 1; kill -9 "$SESS_PID" 2>/dev/null
    fi
    for p in $(pgrep -f "xw-compositor --config-dir" 2>/dev/null); do kill -9 $p 2>/dev/null; done
    for p in $(pgrep -f "Xwayland" 2>/dev/null); do kill -9 $p 2>/dev/null; done
    for p in $(pgrep -f "firefox" 2>/dev/null); do kill -9 $p 2>/dev/null; done
    echo "--- last session log lines ---"
    tail -12 "$LOG" 2>/dev/null
    echo "--- last firefox stderr lines ---"
    tail -20 "$FF_LOG" 2>/dev/null
    rm -rf "$RTD" "$FAKE_HOME" "$PROFILE"
}
trap cleanup EXIT INT TERM

# 1. headless session (compositor + XWayland + xw-xwm + panel)
"$ROOT/build/bin/xw-session" --no-autostart -V >"$LOG" 2>&1 &
SESS_PID=$!
for i in $(seq 1 30); do
    grep -q "session ready" "$LOG" 2>/dev/null && break
    sleep 0.3
done
grep -q "session ready" "$LOG" || { echo "FAIL: session never became ready"; exit 1; }
WLDISPLAY=$(grep "compositor ready: display" "$LOG" | head -1 | grep -o "wayland-[0-9A-Za-z.-]*")
echo "session up (wayland display: $WLDISPLAY)"

# 2. firefox as a native wayland client, fresh profile, own stderr
WAYLAND_DISPLAY="$WLDISPLAY" XDG_RUNTIME_DIR="$RTD" \
    "$APPS/usr/bin/firefox" --no-remote -profile "$PROFILE" \
    about:blank >"$FF_LOG" 2>&1 &
FF_PID=$!

# 3. watch it: alive at the end? window mapped? errors?
MAPPED=no
for i in $(seq 1 "$SECS"); do
    if grep -q "window .* MAPPED" "$LOG" 2>/dev/null; then MAPPED=yes; fi
    if ! kill -0 "$FF_PID" 2>/dev/null; then
        wait "$FF_PID" 2>/dev/null; RC=$?
        echo "FAIL: firefox DIED after ~${i}s (exit $RC)"
        echo "exit code: $RC (139=SIGSEGV, 134=SIGABRT, 137=SIGKILL, 0=clean exit)"
        exit 2
    fi
    sleep 1
done

echo "firefox alive after ${SECS}s (pid $FF_PID)"
echo "window mapped: $MAPPED"
if rg -q "error in client communication" "$LOG"; then
    echo "server saw a client connection die: $(rg -n 'client communication' "$LOG" | head -3)"
else
    echo "server saw NO client connection death"
fi
if rg -q "invalid or used token" "$LOG"; then
    echo "NOTE: activation rejections present: $(rg -c 'invalid or used token' "$LOG")"
else
    echo "no activation rejections"
fi
if rg -q "xdg-activation" "$FF_LOG"; then
    echo "--- firefox stderr mentions activation ---"
    rg "activation" "$FF_LOG" | head -5
fi

# 4. clean logout with the browser still running
XDG_RUNTIME_DIR="$RTD" "$ROOT/build/bin/xw-session-ctl" logout 2>/dev/null
for i in $(seq 1 20); do
    kill -0 "$SESS_PID" 2>/dev/null || break
    sleep 0.5
done
if kill -0 "$SESS_PID" 2>/dev/null; then
    echo "FAIL: session did not exit on logout (firefox still connected?)"
    exit 3
fi
echo "session logged out cleanly with firefox running"
echo "PASS: firefox survived as a native wayland client of xw"
exit 0
