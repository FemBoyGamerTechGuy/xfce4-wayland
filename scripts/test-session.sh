#!/bin/sh
# test-session.sh — process-level integration test for the M6 session
# manager (complements the in-process suite in tests/):
#
#   * xw-session supervises a real xw-compositor child process
#   * control socket protocol (ping / status / logout)
#   * honest power-action failure when logind/elogind is unavailable
#   * XDG autostart filtering (OnlyShowIn=XFCE runs, NotShowIn= skipped)
#   * clean logout: exit code 0, sockets removed, no leftover children
#
# Run from the repository root after `make all` (or via `make check`).

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"

if [ ! -x "$BIN/xw-session" ] || [ ! -x "$BIN/xw-compositor" ] ||
   [ ! -x "$BIN/xw-session-ctl" ]; then
    echo "test-session: build first (make all)" >&2
    exit 2
fi

RTD="$(mktemp -d /tmp/xw-session-test.XXXXXX)"
FAKE_HOME="$(mktemp -d /tmp/xw-session-home.XXXXXX)"
chmod 700 "$RTD"
LOG="$RTD/session.log"
SESS_PID=0

export XDG_RUNTIME_DIR="$RTD"
export HOME="$FAKE_HOME"

cleanup() {
    if [ "$SESS_PID" -gt 0 ] && kill -0 "$SESS_PID" 2>/dev/null; then
        kill "$SESS_PID" 2>/dev/null
        sleep 1
        kill -9 "$SESS_PID" 2>/dev/null
    fi
    rm -rf "$RTD" "$FAKE_HOME"
}
trap cleanup EXIT INT TERM

pass=0
fail=0
check() {
    if eval "$2"; then
        pass=$((pass + 1))
        echo "ok   $1"
    else
        fail=$((fail + 1))
        echo "FAIL $1"
    fi
}

wait_for() { # condition, description-unused, timeout in 0.1s steps
    i=0
    while [ $i -lt 100 ]; do
        if eval "$1"; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

wait_pid_exit() { # pid
    i=0
    while [ $i -lt 150 ]; do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

log_has_unexpected_error() {
    # error lines not attributable to the expected power-action failure
    rg '\[xw-session error\]' "$LOG" 2>/dev/null | rg -v 'power action' >/dev/null
}

has_wayland_socket() {
    for f in "$RTD"/wayland-*; do
        [ -S "$f" ] && return 0
    done
    return 1
}

echo "== session 1: supervision + ctl protocol + logout =="

"$BIN/xw-session" -n >"$LOG" 2>&1 &
SESS_PID=$!

check "control socket appears" \
    'wait_for "[ -S \"$RTD/xw-session.sock\" ]"'
check "compositor child creates a wayland socket" 'wait_for "has_wayland_socket"'

reply="$("$BIN/xw-session-ctl" ping 2>/dev/null)"
check "ping -> ok pong" '[ "$reply" = "ok pong" ]'

reply="$("$BIN/xw-session-ctl" status 2>/dev/null)"
check "status reports compositor=running" \
    'case "$reply" in *compositor=running*) true ;; *) false ;; esac'

# power management without logind/elogind must fail honestly (no pretending)
reply="$("$BIN/xw-session-ctl" suspend 2>/dev/null)"
check "suspend without logind -> honest error" \
    'case "$reply" in "error power management unavailable"*) true ;; ok*) true ;; *) false ;; esac'

# session manager and compositor still alive after failed power action
check "session survives failed power action" 'kill -0 "$SESS_PID" 2>/dev/null'

"$BIN/xw-session-ctl" logout >/dev/null 2>&1
check "logout -> session exits" 'wait_pid_exit "$SESS_PID"'
wait "$SESS_PID"
rc=$?
check "logout exit code 0" '[ "$rc" -eq 0 ]'
check "control socket removed" '[ ! -e "$RTD/xw-session.sock" ]'
check "wayland socket removed" '! has_wayland_socket'
check "compositor child gone" \
    'wait_for "[ -z \"$(pgrep -f xw-compositor 2>/dev/null)\" ]"'
check "no unexpected errors in log" '! log_has_unexpected_error'

echo
echo "== session 2: XDG autostart filtering =="

mkdir -p "$FAKE_HOME/.config/autostart"
cat >"$FAKE_HOME/.config/autostart/xw-run.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Run me
Exec=touch $RTD/marker-ran
OnlyShowIn=XFCE;
EOF
cat >"$FAKE_HOME/.config/autostart/xw-skip.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Skip me
Exec=touch $RTD/marker-skip
NotShowIn=XFCE;
EOF
cat >"$FAKE_HOME/.config/autostart/xw-hidden.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Hidden
Hidden=true
Exec=touch $RTD/marker-hidden
OnlyShowIn=XFCE;
EOF

LOG2="$RTD/session2.log"
"$BIN/xw-session" >"$LOG2" 2>&1 &
SESS_PID=$!

check "autostart: session starts" \
    'wait_for "[ -S \"$RTD/xw-session.sock\" ]"'
check "autostart: OnlyShowIn=XFCE entry ran" \
    'wait_for "[ -e \"$RTD/marker-ran\" ]"'
check "autostart: NotShowIn=XFCE entry skipped" \
    '[ ! -e "$RTD/marker-skip" ]'
check "autostart: Hidden entry skipped" '[ ! -e "$RTD/marker-hidden" ]'

"$BIN/xw-session-ctl" logout >/dev/null 2>&1
check "autostart: session exits cleanly" 'wait_pid_exit "$SESS_PID"'
wait "$SESS_PID"
check "autostart: exit code 0" '[ "$?" -eq 0 ]'

echo
echo "== session 3: runtime spawns + panel autostart (M8) =="

if [ -x "$BIN/xw-panel" ]; then
    mkdir -p "$FAKE_HOME/.config/autostart"
    cat >"$FAKE_HOME/.config/autostart/xw-panel.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=xw-panel
Exec=$BIN/xw-panel
OnlyShowIn=XFCE;
EOF
    # the exit-dialog command: a marker-writing stand-in for the dialog
    cat >"$RTD/fake-exit" <<EOF
#!/bin/sh
touch "$RTD/dialog-marker"
sleep 30
EOF
    chmod +x "$RTD/fake-exit"
    export XW_EXIT_CMD="$RTD/fake-exit"

    LOG3="$RTD/session3.log"
    PATH="$BIN:$PATH" XDG_RUNTIME_DIR="$RTD" HOME="$FAKE_HOME" \
        "$BIN/xw-session" >"$LOG3" 2>&1 &
    SESS_PID=$!

    check "panel session starts" \
        'wait_for "[ -S \"$RTD/xw-session.sock\" ]"'
    check "panel autostarts and stays alive" \
        'wait_for "pgrep -x xw-panel >/dev/null"'

    reply="$(PATH="$BIN:$PATH" XDG_RUNTIME_DIR="$RTD" \
        "$BIN/xw-session-ctl" run -- touch "$RTD/run-marker" 2>/dev/null)"
    check "ctl run -> ok spawned" '[ "$reply" = "ok spawned" ]'
    check "ctl run executed the command" \
        'wait_for "[ -e \"$RTD/run-marker\" ]"'

    reply="$(PATH="$BIN:$PATH" XDG_RUNTIME_DIR="$RTD" \
        "$BIN/xw-session-ctl" exit-dialog 2>/dev/null)"
    check "ctl exit-dialog -> ok" \
        'case "$reply" in ok*) true ;; *) false ;; esac'
    check "exit dialog process spawned (marker)" \
        'wait_for "[ -e \"$RTD/dialog-marker\" ]"'

    PATH="$BIN:$PATH" XDG_RUNTIME_DIR="$RTD" "$BIN/xw-session-ctl" \
        logout >/dev/null 2>&1
    check "panel session exits cleanly" 'wait_pid_exit "$SESS_PID"'
    wait "$SESS_PID"
    check "panel session exit code 0" '[ "$?" -eq 0 ]'
    check "panel process gone after logout" \
        'wait_for "[ -z \"$(pgrep -x xw-panel 2>/dev/null)\" ]"'
    check "fake exit dialog gone after logout" \
        'wait_for "[ -z \"$(pgrep -f fake-exit 2>/dev/null)\" ]"'
else
    echo "SKIP session 3 (xw-panel not built)"
fi

echo
echo "test-session: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
