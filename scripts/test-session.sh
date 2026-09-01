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

# deterministic power environment: a bogus sleep-modes file makes
# suspend/hibernate unavailable on any host (a real loginctl would
# otherwise actually suspend dev machines running the suite)
export XW_POWER_STATE_PATH="$RTD/nonexistent-power-state"

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

# power-status: honest capability report (suspend forced off by the
# bogus state file; loginctl yes/no depends on the host)
reply="$("$BIN/xw-session-ctl" power-status 2>/dev/null)"
check "power-status reports suspend unavailable with reason" \
    'case "$reply" in "ok_loginctl="*"suspend=no:"*) true ;; *) false ;; esac'
check "power-status reports hibernate unavailable with reason" \
    'case "$reply" in "ok_loginctl="*"hibernate=no:"*) true ;; *) false ;; esac'

# power management must fail honestly when unavailable (no pretending)
reply="$("$BIN/xw-session-ctl" suspend 2>/dev/null)"
check "suspend unavailable -> honest error" \
    'case "$reply" in "error power management unavailable"*) true ;; *) false ;; esac'

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
echo "== session 1b: power backend (fake loginctl + fake sleep modes) =="

# a fake loginctl that logs its arguments; hibernate fails with stderr
FBIN="$RTD/fakebin"
mkdir -p "$FBIN"
cat >"$FBIN/loginctl" <<'FAKE'
#!/bin/sh
echo "loginctl $*" >>"$FAKE_LOG"
if [ "$1" = "hibernate" ]; then
    echo "Sleep verb 'hibernate' not supported" >&2
    exit 1
fi
exit 0
FAKE
chmod +x "$FBIN/loginctl"
export FAKE_LOG="$RTD/loginctl-calls.log"
: >"$FAKE_LOG"
printf 'freeze mem disk\n' >"$RTD/power-state"
export XW_POWER_STATE_PATH="$RTD/power-state"

PATH="$FBIN:$PATH" "$BIN/xw-session" -n >"$LOG" 2>&1 &
SESS_PID=$!
check "1b: control socket appears" \
    'wait_for "[ -S \"$RTD/xw-session.sock\" ]"'

reply="$("$BIN/xw-session-ctl" power-status 2>/dev/null)"
check "1b: power-status sees working loginctl" \
    'case "$reply" in "ok_loginctl=yes_suspend=yes_"*) true ;; *) false ;; esac'
check "1b: power-status suspend=yes" \
    'case "$reply" in *"suspend=yes_hibernate="*) true ;; *) false ;; esac'
check "1b: power-status hibernate=yes" \
    'case "$reply" in *"hibernate=yes_poweroff="*) true ;; *) false ;; esac'
check "1b: power-status poweroff=yes" \
    'case "$reply" in *"poweroff=yes_reboot="*) true ;; *) false ;; esac'

reply="$("$BIN/xw-session-ctl" suspend 2>/dev/null)"
check "1b: suspend -> ok (fake backend)" '[ "$reply" = "ok suspending" ]'
check "1b: fake loginctl was invoked with suspend" \
    'rg -q "^loginctl suspend$" "$FAKE_LOG"'

reply="$("$BIN/xw-session-ctl" hibernate 2>/dev/null)"
check "1b: hibernate failure carries backend stderr" \
    'case "$reply" in "error power management unavailable: loginctl: Sleep verb"*) true ;; *) false ;; esac'

# session still alive; clean logout
check "1b: session alive after power round-trip" 'kill -0 "$SESS_PID" 2>/dev/null'
"$BIN/xw-session-ctl" logout >/dev/null 2>&1
check "1b: logout -> session exits" 'wait_pid_exit "$SESS_PID"'
wait "$SESS_PID"
check "1b: logout exit code 0" '[ "$?" -eq 0 ]'
check "1b: no unexpected errors in log" '! log_has_unexpected_error'
unset FAKE_LOG
export XW_POWER_STATE_PATH="$RTD/nonexistent-power-state"

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
echo "== session 4: X11 nested backend (Xvfb) =="

if command -v Xvfb >/dev/null 2>&1 && [ -x "$ROOT/build/tests/x11probe" ]; then
    rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
    Xvfb :99 -screen 0 800x600x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1

    LOG4="$RTD/session4-x11.log"
    DISPLAY=:99 "$BIN/xw-compositor" --backend x11 -s xw-test-x11 \
        >"$LOG4" 2>&1 &
    COMP_X11_PID=$!

    check "x11: compositor starts under Xvfb" \
        'wait_for "kill -0 $COMP_X11_PID 2>/dev/null && [ -S \"$RTD/xw-test-x11\" ]"'

    PROBE="$ROOT/build/tests/x11probe"
    out="$("$PROBE" :99 2>&1)"
    check "x11: rendered pixels visible in the X window" \
        '[ "$out" = "probe: background pixels OK" ]'

    out="$("$PROBE" :99 inject 2>&1)"
    check "x11: XTEST input injection accepted" \
        'case "$out" in *"injected Ctrl+Alt+D"*) true ;; *) false ;; esac'
    check "x11: shortcut engine consumed the injected key" \
        'wait_for "rg -q \"shortcut: action\" \"$LOG4\" 2>/dev/null"'

    kill -TERM "$COMP_X11_PID" 2>/dev/null
    check "x11: compositor exits cleanly" 'wait_pid_exit "$COMP_X11_PID"'
    wait "$COMP_X11_PID" 2>/dev/null
    check "x11: exit code 0" '[ "$?" -eq 0 ]'
    kill "$XVFB_PID" 2>/dev/null
    rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
else
    echo "SKIP session 4 (Xvfb or x11probe not available)"
fi

echo
echo "== session 5: xw-session --nested (auto x11 under Xvfb) =="

if command -v Xvfb >/dev/null 2>&1 && [ -x "$ROOT/build/tests/x11probe" ]; then
    rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
    Xvfb :99 -screen 0 800x600x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1

    LOG5="$RTD/session5-nested.log"
    # no WAYLAND_DISPLAY, DISPLAY=:99 -> auto-selects the x11 backend
    env -u WAYLAND_DISPLAY DISPLAY=:99 XDG_RUNTIME_DIR="$RTD" \
        HOME="$FAKE_HOME" "$BIN/xw-session" --nested -n -S xw-nest \
        >"$LOG5" 2>&1 &
    SESS_PID=$!

    check "nested: session starts" \
        'wait_for "[ -S \"$RTD/xw-nest.sock\" ]"'
    check "nested: compositor runs with the x11 backend" \
        'wait_for "rg -q \"nested session: backend x11\" \"$LOG5\" 2>/dev/null"'

    PROBE="$ROOT/build/tests/x11probe"
    check "nested: desktop window visible in Xvfb" \
        'wait_for "\"$PROBE\" :99 2>/dev/null | rg -q \"background pixels OK\""'

    XDG_RUNTIME_DIR="$RTD" "$BIN/xw-session-ctl" -S xw-nest logout \
        >/dev/null 2>&1
    check "nested: logout exits cleanly" 'wait_pid_exit "$SESS_PID"'
    wait "$SESS_PID"
    check "nested: exit code 0" '[ "$?" -eq 0 ]'
    kill "$XVFB_PID" 2>/dev/null
    rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
else
    echo "SKIP session 5 (Xvfb not available)"
fi

echo
echo "== session 6: nested Wayland backend (two real compositor processes) =="

if [ -x "$BIN/xw-panel" ]; then
    LOG6A="$RTD/session6-parent.log"
    LOG6B="$RTD/session6-child.log"

    "$BIN/xw-compositor" -s xw-nest-parent -o 480x360 >"$LOG6A" 2>&1 &
    PARENT_PID=$!
    check "wnested: parent compositor starts" \
        'wait_for "[ -S \"$RTD/xw-nest-parent\" ]"'

    WAYLAND_DISPLAY=xw-nest-parent "$BIN/xw-compositor" \
        --backend nested --parent-display xw-nest-parent -s xw-nest-child \
        -o 240x180 >"$LOG6B" 2>&1 &
    CHILD_PID=$!
    check "wnested: nested compositor starts" \
        'wait_for "rg -q \"nested backend: window 240x180\" \"$LOG6B\" 2>/dev/null && [ -S \"$RTD/xw-nest-child\" ]"'
    # "window 240x180" only logs after the parent answered the xdg
    # configure: it proves the full parent handshake happened

    # a real panel client inside the NESTED compositor
    XDG_RUNTIME_DIR="$RTD" WAYLAND_DISPLAY=xw-nest-child \
        "$BIN/xw-panel" >/dev/null 2>&1 &
    PANEL6_PID=$!
    check "wnested: panel runs inside the nested compositor" \
        'wait_for "pgrep -x xw-panel >/dev/null 2>/dev/null"'

    kill -TERM "$PANEL6_PID" "$CHILD_PID" "$PARENT_PID" 2>/dev/null
    check "wnested: nested compositor exits cleanly" \
        'wait_pid_exit "$CHILD_PID"'
    check "wnested: parent compositor exits cleanly" \
        'wait_pid_exit "$PARENT_PID"'
    wait "$CHILD_PID" 2>/dev/null
    CHILD_RC=$?
    check "wnested: child exit code 0 (got $CHILD_RC)" '[ "$CHILD_RC" -eq 0 ]'
    wait "$PARENT_PID" 2>/dev/null
    check "wnested: parent exit code 0" '[ "$?" -eq 0 ]'
    check "wnested: panel process gone" \
        'wait_for "[ -z \"$(pgrep -x xw-panel 2>/dev/null)\" ]"'
else
    echo "SKIP session 6 (xw-panel not built)"
fi

echo
echo "test-session: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
