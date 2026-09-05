#!/bin/sh
# pixman-bug-count.sh — the session-level RED for the region16 boundary
# round. Counts pixman "*** BUG *** Invalid rectangle passed" blocks in
# the compositor's stderr while the geometry storm runs, through the
# REAL session chain (xw-session headless + geomstorm + ctl-socket
# logout, exactly the test-session-trace.sh environment minus the trace
# variables — the pixman spam is trace-independent).
#
# Pre-fix: every storm commit carries damage(0, 0, INT32_MAX, INT32_MAX)
# (tests/geomstorm.c) whose x2 truncates into the int16 region16 domain
# as -1 — an inverted box — so pixman logs one BUG block per commit and
# drops the rect. Post-fix the count must be 0.
#
# usage: scripts/pixman-bug-count.sh [storm-seconds]   (default 1)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

SECONDS_RUN="${1:-1}"

RTD="$(mktemp -d /tmp/xw-pixbug.XXXXXX)"
FAKE_HOME="$(mktemp -d /tmp/xw-pixbug-home.XXXXXX)"
SESS=0
STORM=0
cleanup() {
    [ "$STORM" -gt 0 ] && kill -9 "$STORM" 2>/dev/null
    if [ "$SESS" -gt 0 ]; then
        if [ -r "/proc/$SESS/task/$SESS/children" ]; then
            for c in $(tr ' ' '\n' < "/proc/$SESS/task/$SESS/children"); do
                kill -9 "$c" 2>/dev/null
            done
        fi
        kill -9 "$SESS" 2>/dev/null
    fi
    rm -rf "$RTD" "$FAKE_HOME"
}
trap cleanup EXIT INT TERM
chmod 700 "$RTD" "$FAKE_HOME"

export XDG_RUNTIME_DIR="$RTD"
export HOME="$FAKE_HOME"
export XW_SESSION_DBUS=0
export XW_SESSION_XWAYLAND=0
export XW_PANEL_CMD=none
unset XW_INPUT_TRACE XW_GEOMETRY_TRACE

"$ROOT/build/bin/xw-session" -B headless -S pixbug 2> "$RTD/sink" &
SESS=$!

i=0
while [ $i -lt 50 ]; do
    [ -S "$RTD/pixbug.sock" ] && [ -S "$RTD/wayland-0" ] && break
    kill -0 "$SESS" 2>/dev/null || break
    sleep 0.1
    i=$((i + 1))
done
if [ ! -S "$RTD/pixbug.sock" ]; then
    echo "pixman-bug-count: session did not come up"
    exit 1
fi

"$ROOT/build/tests/geomstorm" wayland-0 12 > "$RTD/storm.out" 2>&1 &
STORM=$!
sleep "$SECONDS_RUN"
timeout 8 "$ROOT/build/bin/xw-session-ctl" -S pixbug logout > /dev/null 2>&1
wait "$SESS"
STORM=0

BUGS=$(rg -c "Invalid rectangle" "$RTD/sink" 2>/dev/null || echo 0)
echo "pixman-bug-count: $BUGS BUG blocks in the session's stderr"
[ "$BUGS" -eq 0 ] || exit 1
