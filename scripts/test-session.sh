#!/bin/sh
# test-session.sh — process-level integration test for the M6 session
# manager (complements the in-process suite in tests/):
#
#   * xw-session supervises a real xw-compositor child process
#   * control socket protocol (ping / status / logout)
#   * honest power-action failure when logind/elogind is unavailable
#   * XDG autostart filtering (OnlyShowIn=XFCE runs, NotShowIn= skipped)
#   * session d-bus: started when a TTY login has none, exported to
#     children, live foreign bus reused (never killed), stale address
#     replaced, teardown stops only the daemon the session started
#   * clean logout: exit code 0, sockets removed, no leftover children
#
# Run from the repository root after `make all` (or via `make check`).

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"

# Pick up the optional local sysroot (runtime library paths included)
# so sysroot-built binaries resolve their dependencies. No-op on normal
# distributions.
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

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
echo "== session 1c: session restart (re-exec) =="

# a dedicated ctl name keeps this independent from the defaults
export XW_SESSION_SOCK=xw-restart-test
"$BIN/xw-session" -n -S "$XW_SESSION_SOCK" >"$LOG" 2>&1 &
SESS_PID=$!
check "1c: control socket appears" \
    'wait_for "[ -S \"$RTD/$XW_SESSION_SOCK.sock\" ]"'

reply="$("$BIN/xw-session-ctl" -S "$XW_SESSION_SOCK" restart 2>/dev/null)"
check "1c: restart -> ok restarting session" \
    '[ "$reply" = "ok restarting session" ]'

# re-exec keeps the same pid: the socket is removed, then reappears
check "1c: socket torn down" 'wait_for "[ ! -S \"$RTD/$XW_SESSION_SOCK.sock\" ]"'
check "1c: socket back after re-exec" \
    'wait_for "[ -S \"$RTD/$XW_SESSION_SOCK.sock\" ]"'
check "1c: session process still alive (same pid)" 'kill -0 "$SESS_PID" 2>/dev/null'

reply="$("$BIN/xw-session-ctl" -S "$XW_SESSION_SOCK" status 2>/dev/null)"
check "1c: fresh session runs the compositor again" \
    'case "$reply" in *compositor=running*) true ;; *) false ;; esac'
check "1c: restart counter reset (fresh session state)" \
    'case "$reply" in *restarts=0*) true ;; *) false ;; esac'

# flags survive the re-exec: -n must still hold autostart back
reply="$("$BIN/xw-session-ctl" -S "$XW_SESSION_SOCK" status 2>/dev/null)"
check "1c: no autostart children (flag preserved)" \
    'case "$reply" in *autostart=0*) true ;; *) false ;; esac'

"$BIN/xw-session-ctl" -S "$XW_SESSION_SOCK" logout >/dev/null 2>&1
check "1c: logout after restart exits cleanly" 'wait_pid_exit "$SESS_PID"'
wait "$SESS_PID"
check "1c: exit code 0" '[ "$?" -eq 0 ]'
unset XW_SESSION_SOCK

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
echo "== session 4b: libinput real-input source startup =="

# The udev-seat code path (udev_new -> libinput_udev_create_context ->
# libinput_udev_assign_seat) is exactly the one whose direct libudev
# symbols broke the final link on Arch/Artix; this exercises it for
# real. A machine without a udev seat may refuse the source — that is
# an honest, logged refusal and passes; what must never happen is a
# crash or silence. Skipped honestly when the build has no libinput
# backend (XW_LIBINPUT=0 or dev files absent).
if ldd "$BIN/xw-compositor" 2>/dev/null | grep -q libinput; then
    LOG4B="$RTD/session4b-libinput.log"
    "$BIN/xw-compositor" --backend headless -I libinput -s xw-test-li \
        >"$LOG4B" 2>&1 &
    COMP_LI_PID=$!

    check "libinput: source startup logs its mode or a diagnostic" \
        'wait_for "rg -q \"input:\" \"$LOG4B\" 2>/dev/null"'
    check "libinput: the udev-seat (or path) code path executed" \
        'rg -q "libinput udev mode|libinput path mode|udev unavailable|udev context failed|cannot assign seat|path context failed" "$LOG4B" 2>/dev/null'

    kill -TERM "$COMP_LI_PID" 2>/dev/null
    wait "$COMP_LI_PID" 2>/dev/null
    LI_RC=$?
    if [ "$LI_RC" -eq 0 ]; then
        check "libinput: compositor exits cleanly on SIGTERM (rc=0)" \
            '[ "$LI_RC" -eq 0 ]'
    else
        check "libinput: early exit is an honest input failure (rc=$LI_RC)" \
            'rg -q "input:.*(unavailable|failed|cannot assign|no device)" "$LOG4B" 2>/dev/null'
    fi
    check "libinput: no leaked compositor process" \
        '! kill -0 $COMP_LI_PID 2>/dev/null'
else
    echo "SKIP session 4b (compositor built without the libinput backend)"
fi

echo
echo "== session 5: nested x11 session under a reparenting WM =="
echo "   (panel regression: window resize + layer reconfigure + crashers)"

# Full reproduction of the nested-session panel-invisibility class:
#  - a real WM reparents the compositor window and RESIZES it
#  - the panel autostarts as a real Wayland layer-shell client
#  - the X server defers the structure events (large PutImage present)
#  - panelprobe verifies pixels: bar visible, spanning the resized
#    width, background below, and the compositor's SOFTWARE cursor at
#    a warp point (the X cursor is invisible -> the visible cursor can
#    only come through the compositor input path)
if command -v Xvfb >/dev/null 2>&1 && [ -x "$ROOT/build/tests/panelprobe" ]; then
    rm -f /tmp/.X99-lock /tmp/.X11-unix/X99
    Xvfb :99 -screen 0 800x600x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1

    # the reparenting WM: forces the client to 700x450 (from 400x300)
    if [ -x "$ROOT/build/tests/miniwm" ]; then
        "$ROOT/build/tests/miniwm" :99 700 450 >/dev/null 2>&1 &
        MINIWM_PID=$!
        sleep 0.5
    else
        MINIWM_PID=0
    fi

    # autostart the real panel from a dedicated HOME (the shared
    # FAKE_HOME carries session-2 filtering entries whose commands exit
    # immediately, which would legitimately light up the exit log)
    NEST_HOME="$RTD/home5"
    mkdir -p "$NEST_HOME/.config/autostart"
    cat >"$NEST_HOME/.config/autostart/xw-panel.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=xw-panel
Exec=$ROOT/build/bin/xw-panel
OnlyShowIn=XFCE;
DESKTOP

    LOG5="$RTD/session5-nested.log"
    # no WAYLAND_DISPLAY, DISPLAY=:99 -> auto-selects the x11 backend
    env -u WAYLAND_DISPLAY DISPLAY=:99 XDG_RUNTIME_DIR="$RTD" \
        HOME="$NEST_HOME" "$BIN/xw-session" --nested -S xw-nest \
        >"$LOG5" 2>&1 &
    SESS_PID=$!

    check "nested: session starts" \
        'wait_for "[ -S \"$RTD/xw-nest.sock\" ]"'
    check "nested: compositor runs with the x11 backend" \
        'wait_for "rg -q \"compositor backend: x11\" \"$LOG5\" 2>/dev/null"'
    check "nested: panel autostarted" \
        'wait_for "rg -q \"autostart .*xw-panel\" \"$LOG5\" 2>/dev/null"'
    # the panel must RUN, not merely be spawned (regression: it used to
    # segfault on the resize-reconfigure race and die silently)
    check "nested: panel process alive" \
        'wait_for "pgrep -x xw-panel >/dev/null 2>/dev/null"'
    check "nested: no autostart child died" \
        '! rg -q "exited:" "$LOG5" 2>/dev/null'

    # output resize assertion: the compositor runs with -q (no INFO
    # logs) — panelprobe proves the resize via the full-extent
    # background and the bar covering the right edge of the WM geometry

    PROBE="$ROOT/build/tests/panelprobe"
    check "nested: panel visible, spans resized width, cursor verified" \
        'wait_for "\"$PROBE\" :99 cursor 2>/dev/null | rg -q \"PASS.*VERIFIED\""'
    check "nested: no crash lines in the log" \
        '! rg -q "Segmentation|AddressSanitizer|abort" "$LOG5" 2>/dev/null'

    # panel interaction through the compositor input path: XTEST click
    # on workspace button 2 switches workspaces (visible in the pixels
    # of the switcher) — keep it to the probe's PASS for now; the full
    # interaction matrix is covered by the in-process panel tests.

    XDG_RUNTIME_DIR="$RTD" "$BIN/xw-session-ctl" -S xw-nest logout \
        >/dev/null 2>&1
    check "nested: logout exits cleanly" 'wait_pid_exit "$SESS_PID"'
    wait "$SESS_PID"
    check "nested: exit code 0" '[ "$?" -eq 0 ]'
    check "nested: panel gone with the session" \
        'wait_for "[ -z \"$(pgrep -x xw-panel 2>/dev/null)\" ]"'
    if [ "$MINIWM_PID" -gt 0 ]; then
        kill "$MINIWM_PID" 2>/dev/null
    fi
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
echo "== session 7: autostart child exits are reported, never silent =="

# A dying autostart entry must be logged with its exit status (127 gets
# the Exec= hint). Regression: the reap loop used to discard statuses,
# so a crashed panel looked exactly like a working one.
LOG7="$RTD/session7-exitlog.log"
BAD_HOME="$RTD/home7"
mkdir -p "$BAD_HOME/.config/autostart"
cat >"$BAD_HOME/.config/autostart/xw-fail.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=xw-fail
Exec=/nonexistent/xw-definitely-not-here
OnlyShowIn=XFCE;
DESKTOP
env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RTD" HOME="$BAD_HOME" \
    "$BIN/xw-session" -S xw-fail >"$LOG7" 2>&1 &
SESS7_PID=$!
check "exitlog: session starts" 'wait_for "[ -S \"$RTD/xw-fail.sock\" ]"'
check "exitlog: child exit logged with status" \
    'wait_for "rg -q \"autostart .xw-fail.*exited: exit status 127\" \"$LOG7\" 2>/dev/null"'
check "exitlog: 127 hint mentions the Exec line" \
    'rg -q "Exec= line" "$LOG7" 2>/dev/null'
XDG_RUNTIME_DIR="$RTD" "$BIN/xw-session-ctl" -S xw-fail logout \
    >/dev/null 2>&1
check "exitlog: logout exits cleanly" 'wait_pid_exit "$SESS7_PID"'


echo
echo "== session 8: DRM backend selection (seat providers, honest failures) =="
# The container has no /dev/dri: every DRM attempt must fail HONESTLY
# (distinct diagnostics, exit 1, never a silent fallback to another
# backend) and the seat acquisition itself must work through the real
# seatd wire protocol against the mock server.

LOG8="$RTD/session8-drm.log"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RTD" \
    "$BIN/xw-session" --backend=drm -n -S xw-drm >"$LOG8" 2>&1 &
SESS8_PID=$!
check "drm: explicit backend fails without KMS (no silent fallback)" \
    'wait_pid_exit "$SESS8_PID"'
wait "$SESS8_PID"; DR_RC=$?
check "drm: session exit code is 1" '[ "$DR_RC" -eq 1 ]'
check "drm: the compositor's reason is visible" \
    'rg -q "no DRM subsystem|no /dev/dri" "$LOG8" 2>/dev/null'
check "drm: the session reports the failure" \
    'rg -q "cannot start the compositor" "$LOG8" 2>/dev/null'
check "drm: no restart loop was attempted" \
    '! rg -q "restarting" "$LOG8" 2>/dev/null'

LOG8B="$RTD/session8-auto.log"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RTD" \
    "$BIN/xw-session" -n -S xw-auto >"$LOG8B" 2>&1 &
SESS8B_PID=$!
check "drm-auto: TTY without KMS starts headless" \
    'wait_for "[ -S \"$RTD/xw-auto.sock\" ]"'
check "drm-auto: the headless downgrade is explained, not silent" \
    'rg -q "no /dev/dri KMS hardware" "$LOG8B" 2>/dev/null'
XDG_RUNTIME_DIR="$RTD" "$BIN/xw-session-ctl" -S xw-auto logout \
    >/dev/null 2>&1
check "drm-auto: logout exits cleanly" 'wait_pid_exit "$SESS8B_PID"'

check "drm: bogus backend name is rejected" \
    '! "$BIN/xw-session" --backend=sidecar >/dev/null 2>&1'
check "drm: compositor rejects bogus backend names" \
    '! "$BIN/xw-compositor" -B sidecar >/dev/null 2>&1'
check "drm: compositor rejects bogus seat providers" \
    '! "$BIN/xw-compositor" -B drm -P gnome >/dev/null 2>&1'

# --- seat acquisition through the REAL protocol (mock seatd) ---
MOCK_SOCK="$RTD/seatd-test.sock"
MOCK_LOG="$RTD/mockseatd.log"
"$ROOT/build/tests/mockseatd" "$MOCK_SOCK" 20 >"$MOCK_LOG" 2>&1 &
MOCK_PID=$!
check "drm: mock seatd is listening" \
    'wait_for "[ -S \"$MOCK_SOCK\" ]"'

LOG8C="$RTD/session8-seatd.log"
SEATD_SOCK="$MOCK_SOCK" env -u DISPLAY -u WAYLAND_DISPLAY \
    XDG_RUNTIME_DIR="$RTD" \
    "$BIN/xw-compositor" -B drm -P seatd -c "$RTD/empty-conf" \
    >"$LOG8C" 2>&1
RC8C=$?
check "drm+seatd: compositor acquired the seat via the real protocol" \
    'rg -q "seat: opened through seatd" "$LOG8C" 2>/dev/null'
check "drm+seatd: seat name from the mock server" \
    'rg -q "seat seat-mock" "$LOG8C" 2>/dev/null'
check "drm+seatd: then fails honestly (no /dev/dri in this container)" \
    'rg -q "no DRM subsystem" "$LOG8C" 2>/dev/null'
check "drm+seatd: exit code 1" '[ "$RC8C" -eq 1 ]'
check "drm+seatd: the mock saw the seat open" \
    'rg -q "seat-opened seat-mock" "$MOCK_LOG" 2>/dev/null'
wait "$MOCK_PID" 2>/dev/null

LOG8D="$RTD/session8-direct.log"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RTD" \
    "$BIN/xw-compositor" -B drm -P direct -c "$RTD/empty-conf" \
    >"$LOG8D" 2>&1
RC8D=$?
check "drm+direct: no VT here -> honest diagnostic" \
    'rg -q "not a virtual terminal|cannot be taken" "$LOG8D" 2>/dev/null'
check "drm+direct: exit code 1" '[ "$RC8D" -eq 1 ]'

# --- elogind provider: accepted, pinned to libseat's logind backend,
# and fails honestly in this container (no registered session, no
# d-bus) instead of silently falling back to another provider ---
LOG8E="$RTD/session8-elogind.log"
env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_SESSION_ID \
    XDG_RUNTIME_DIR="$RTD" \
    "$BIN/xw-compositor" -B drm -P elogind -c "$RTD/empty-conf" \
    >"$LOG8E" 2>&1
RC8E=$?
LOG8E2="$RTD/session8-elogind-alias.log"
env -u DISPLAY -u WAYLAND_DISPLAY -u XDG_SESSION_ID \
    XDG_RUNTIME_DIR="$RTD" \
    "$BIN/xw-compositor" -B drm -P logind -c "$RTD/empty-conf" \
    >"$LOG8E2" 2>&1
check "drm+elogind: provider names elogind/logind accepted (not 'unknown')" \
    '! rg -q "unknown seat provider" "$LOG8E" "$LOG8E2" 2>/dev/null'
check "drm+elogind: libseat pinned to the logind backend" \
    'rg -q "pinned to its .logind. backend" "$LOG8E" 2>/dev/null'
check "drm+elogind: failure is the logind diagnostic, not a fallback" \
    'rg -q "elogind/logind requested" "$LOG8E" 2>/dev/null'
check "drm+elogind: exit code 1" '[ "$RC8E" -eq 1 ]'
check "drm+elogind: never fell back to seatd/direct silently" \
    '! rg -q "opened through seatd|direct VT session on" "$LOG8E" 2>/dev/null'

# --- session 9: the session d-bus (a TTY login provides no bus; the
# session manager must start one, export it to children, reuse a live
# bus, replace a stale address, and stop only its own daemon) ---

echo
echo "== session 9: session d-bus (start, export, reuse, stale, teardown) =="

if ! command -v dbus-daemon >/dev/null 2>&1; then
    echo "SKIP session 9 (dbus-daemon not available)"
else
    # a bus address leaking in from the host session would flip the
    # test into the reuse path; a leftover socket from an earlier
    # session would be adopted — both must be neutralized
    unset DBUS_SESSION_BUS_ADDRESS
    rm -f "$RTD/bus" "$RTD/foreign-bus"

    # an autostart child proves the address is EXPORTED (not just
    # started): its environment must carry the exact bus address
    cat >"$FAKE_HOME/.config/autostart/xw-dbus-probe.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Bus probe
Exec=printenv DBUS_SESSION_BUS_ADDRESS > $RTD/dbus-probe
OnlyShowIn=XFCE;
EOF

    LOG9="$RTD/session9.log"
    "$BIN/xw-session" >"$LOG9" 2>&1 &
    SESS_PID=$!

    check "session d-bus: daemon started as a session child" \
        'wait_for "rg -q \"session d-bus: started dbus-daemon\" \"$LOG9\""'
    check "session d-bus: socket at \$XDG_RUNTIME_DIR/bus" \
        '[ -S "$RTD/bus" ]'

    if command -v dbus-send >/dev/null 2>&1; then
        check "session d-bus: the bus answers a real round trip" \
            'DBUS_SESSION_BUS_ADDRESS="unix:path=$RTD/bus" dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null | rg -q "org.freedesktop.DBus"'
    else
        echo "SKIP session d-bus: round trip (dbus-send not available)"
    fi

    check "session d-bus: autostart children inherit the address" \
        'wait_for "[ -s \"$RTD/dbus-probe\" ]" && [ "$(cat $RTD/dbus-probe)" = "unix:path=$RTD/bus" ]'

    DPID9="$(rg -o "started dbus-daemon \(pid [0-9]+\)" "$LOG9" | rg -o "[0-9]+" | head -1)"

    # the ctl socket only exists once the session reports ready —
    # logging out earlier races the session startup and fails silently
    check "session d-bus: session reports ready" \
        'wait_for "rg -q \"session ready\" \"$LOG9\""'

    "$BIN/xw-session-ctl" logout >/dev/null 2>&1
    check "session d-bus: logout stops the session cleanly" \
        'wait_pid_exit "$SESS_PID"'
    wait "$SESS_PID"
    check "session d-bus: exit code 0" '[ "$?" -eq 0 ]'
    check "session d-bus: teardown stopped the daemon it started" \
        'rg -q "stopping the session d-bus daemon" "$LOG9"'
    check "session d-bus: no orphaned dbus-daemon" \
        '[ -z "$DPID9" ] || ! kill -0 "$DPID9" 2>/dev/null'
    check "session d-bus: no unexpected errors in log" \
        '! rg "\[xw-session error\]" "$LOG9" 2>/dev/null'

    # --- reuse: a live foreign bus is adopted, never killed ---
    rm -f "$RTD/bus"
    dbus-daemon --session --nofork --nopidfile \
        --address="unix:path=$RTD/foreign-bus" &
    FOREIGN_PID=$!
    sleep 0.5

    LOG9R="$RTD/session9-reuse.log"
    DBUS_SESSION_BUS_ADDRESS="unix:path=$RTD/foreign-bus" \
        "$BIN/xw-session" -n >"$LOG9R" 2>&1 &
    SESS_PID=$!
    check "session d-bus: live foreign bus reused" \
        'wait_for "rg -q \"session d-bus: reusing the session bus\" \"$LOG9R\""'
    check "session d-bus: reused-bus session reports ready" \
        'wait_for "rg -q \"session ready\" \"$LOG9R\""'
    "$BIN/xw-session-ctl" logout >/dev/null 2>&1
    check "session d-bus: reused-bus session exits cleanly" \
        'wait_pid_exit "$SESS_PID"'
    wait "$SESS_PID"
    check "session d-bus: a reused bus is never killed by teardown" \
        'kill -0 "$FOREIGN_PID" 2>/dev/null'
    check "session d-bus: reused session started no second daemon" \
        '! rg -q "started dbus-daemon" "$LOG9R"'

    kill "$FOREIGN_PID" 2>/dev/null
    wait "$FOREIGN_PID" 2>/dev/null

    # --- stale: an exported address that does not answer is replaced ---
    unset DBUS_SESSION_BUS_ADDRESS
    rm -f "$RTD/bus"
    LOG9S="$RTD/session9-stale.log"
    DBUS_SESSION_BUS_ADDRESS="unix:path=$RTD/never-existed-bus" \
        "$BIN/xw-session" -n >"$LOG9S" 2>&1 &
    SESS_PID=$!
    check "session d-bus: stale address named, not silently trusted" \
        'wait_for "rg -q \"replacing it with a fresh bus\" \"$LOG9S\""'
    check "session d-bus: fresh bus started at the standard path" \
        'wait_for "rg -q \"session d-bus: started dbus-daemon\" \"$LOG9S\""'
    check "session d-bus: fresh bus socket at \$XDG_RUNTIME_DIR/bus" \
        '[ -S "$RTD/bus" ]'
    check "session d-bus: stale-address session reports ready" \
        'wait_for "rg -q \"session ready\" \"$LOG9S\""'
    "$BIN/xw-session-ctl" logout >/dev/null 2>&1
    check "session d-bus: stale-address session exits cleanly" \
        'wait_pid_exit "$SESS_PID"'
    wait "$SESS_PID"
    rm -f "$RTD/dbus-probe" "$FAKE_HOME/.config/autostart/xw-dbus-probe.desktop"
fi

echo
echo "test-session: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
