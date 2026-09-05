#!/bin/sh
# test-session-trace.sh — the SESSION-level trace-observational
# regression: XW_INPUT_TRACE / XW_GEOMETRY_TRACE must never change
# logout behavior when the REAL supervisor chain runs the compositor.
#
# This is the harness the corrected physical procedure demanded: the
# earlier comparison ran a BARE `xw-compositor -B drm` (whose
# graphical logout cannot even work — xw-exit needs the session
# manager's control socket), and the in-suite white-box regression
# storms a compositor in-process with no supervisor at all. Neither
# exercises the real chain:
#
#   xw-session -> compositor child -> logout request (ctl socket)
#     -> SIGTERM -> compositor clean exit -> session cleanup -> exit
#
# What this script does, per permutation of the trace variables
# (none / XW_INPUT_TRACE=1 / XW_GEOMETRY_TRACE=1 / both):
#
#   * a real xw-session (headless backend, no panel/autostart/dbus/
#     xwayland — the legs under test are supervision + shutdown)
#   * the session's stderr is a STALLED PIPE: a fifo whose read end
#     a sleeping holder keeps open but never reads. This is the one
#     sink class where diagnostic writes block (the physical box
#     hits it with `2>&1 | tee ...` and similar capture setups).
#     The compositor inherits stderr from the session, so the trace
#     flood and the session's own log lines share the stalled sink.
#   * build/tests/geomstorm floods the geometry-trace volume through
#     a real client connection (alternating buffer sizes).
#   * logout goes through the ctl socket exactly like xw-exit.
#   * PASS = the session exits promptly with code 0 and its sockets
#     are cleaned up. A session that never exits IS the reported
#     "logout impossible": on failure the /proc syscall/wchan of BOTH
#     the session and its compositor child are printed, so the wedge
#     is attributed, not guessed.
#
# One extra control: both traces + a FILE sink (2> trace.log) — the
# safe way to capture physical runs — must also logout cleanly, and
# the trace log must contain the compositor child's [geom] lines,
# which is the empirical proof that xw-session propagates the trace
# variables to the compositor.
#
# Usage: scripts/test-session-trace.sh [bin-dir]   (default build/bin)
# Run from the repository root after `make all`.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/bin}"

. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

if [ ! -x "$BIN/xw-session" ] || [ ! -x "$BIN/xw-compositor" ] ||
    [ ! -x "$BIN/xw-session-ctl" ] ||
    [ ! -x "$ROOT/build/tests/geomstorm" ]; then
    echo "test-session-trace: build first (make all)" >&2
    exit 2
fi
STORM="$ROOT/build/tests/geomstorm"

pass=0
fail=0
check() {
    if [ "$2" -eq 0 ]; then
        pass=$((pass + 1))
        echo "ok   $1"
    else
        fail=$((fail + 1))
        echo "FAIL $1"
    fi
}

# /proc diagnostics: WHERE is a process stuck? (syscall + wchan)
dump_wedge() { # pid label
    pid="$1"
    label="$2"
    [ -d "/proc/$pid" ] || return 0
    echo "    [wedge $label] syscall: $(cat /proc/$pid/syscall 2>/dev/null)"
    echo "    [wedge $label] wchan:   $(cat /proc/$pid/wchan 2>/dev/null)"
}

# children of a pid, space-separated (direct compositor child lookup)
child_pids() { # pid
    [ -r "/proc/$1/task/$1/children" ] || return 0
    tr ' ' '\n' < "/proc/$1/task/$1/children" 2>/dev/null | rg -v '^$' |
        tr '\n' ' '
}

wait_gone() { # pid timeout-in-0.1s-steps
    i=0
    while [ $i -lt "$2" ]; do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# ---------------------------------------------------------------- matrix
# run_variant <name> <input-trace 0|1> <geometry-trace 0|1> <sink-mode>
#   sink-mode: stall  — stderr is a fifo a holder keeps open, never reads
#              file  — stderr is a regular file (the safe capture)
run_variant() {
    VNAME="$1"
    VIN="$2"
    VGEOM="$3"
    VSINK="$4"

    RTD="$(mktemp -d /tmp/xw-strace.XXXXXX)"
    FAKE_HOME="$(mktemp -d /tmp/xw-strace-home.XXXXXX)"
    chmod 700 "$RTD" "$FAKE_HOME"
    CTL_NAME="tstrace-$$-$VNAME"
    HOLDER=0
    STORM_PID=0
    SESS_PID=0
    LOG="$RTD/session-stderr"

    cleanup() {
        # the compositor child must die with the session: on the wedge
        # path the session is SIGKILLed here, which would ORPHAN the
        # compositor (and the storm parks in POLLOUT against it)
        if [ "$SESS_PID" -gt 0 ]; then
            for c in $(child_pids "$SESS_PID"); do
                kill -9 "$c" 2>/dev/null
            done
        fi
        [ "$STORM_PID" -gt 0 ] && kill -9 "$STORM_PID" 2>/dev/null
        [ "$SESS_PID" -gt 0 ] && kill -9 "$SESS_PID" 2>/dev/null
        [ "$HOLDER" -gt 0 ] && kill -9 "$HOLDER" 2>/dev/null
        rm -rf "$RTD" "$FAKE_HOME"
    }
    trap cleanup EXIT INT TERM

    # the environment a physical run would have: trace vars exported,
    # everything else minimal and deterministic
    export XDG_RUNTIME_DIR="$RTD"
    export HOME="$FAKE_HOME"
    export XW_SESSION_DBUS=0
    export XW_SESSION_XWAYLAND=0
    export XW_PANEL_CMD=none
    if [ "$VIN" -eq 1 ]; then
        export XW_INPUT_TRACE=1
    else
        unset XW_INPUT_TRACE
    fi
    if [ "$VGEOM" -eq 1 ]; then
        export XW_GEOMETRY_TRACE=1
    else
        unset XW_GEOMETRY_TRACE
    fi

    # the sink
    if [ "$VSINK" = stall ]; then
        # a fifo whose read end a sleeping holder keeps open forever
        # and never reads: writes block once its 64K buffer fills —
        # the stalled-stderr condition, now through the real session
        mkfifo "$RTD/stall"
        sleep 600 < "$RTD/stall" &
        HOLDER=$!
        SINK="$RTD/stall"
    else
        SINK="$LOG"
    fi

    # the real session (headless: the supervision + shutdown legs under
    # test are backend-independent; DRM adds only the VT restore, which
    # the physical run checks)
    "$BIN/xw-session" -B headless -S "$CTL_NAME" -n 2> "$SINK" &
    SESS_PID=$!

    # wait for readiness (ctl socket + wayland socket)
    i=0
    while [ $i -lt 50 ]; do
        [ -S "$RTD/$CTL_NAME.sock" ] && [ -S "$RTD/wayland-0" ] && break
        kill -0 "$SESS_PID" 2>/dev/null || break
        sleep 0.1
        i=$((i + 1))
    done
    if [ ! -S "$RTD/$CTL_NAME.sock" ]; then
        echo "    $VNAME: session did not come up"
        check "$VNAME: session starts" 1
        cleanup
        trap - EXIT INT TERM
        return
    fi
    check "$VNAME: session starts" 0

    # flood the trace volume through a real client (geometry storm)
    if [ "$VGEOM" -eq 1 ]; then
        "$STORM" wayland-0 12 > "$RTD/storm.out" 2>&1 &
        STORM_PID=$!
        sleep 2 # enough to fill the stalled sink many times over
    else
        # control / input-only: run the storm anyway (it must be inert
        # without XW_GEOMETRY_TRACE — proving the storm itself changes
        # nothing when the instrument is off)
        "$STORM" wayland-0 12 > "$RTD/storm.out" 2>&1 &
        STORM_PID=$!
        sleep 1
    fi

    # logout — exactly the xw-exit path: the control-socket line.
    # Bounded: the ctl client reads until the session closes the
    # connection; a wedged session never closes it (and never sends
    # the SIGTERM) — the timeout is what turns that into a failure
    # instead of a hung test.
    T0=$(date +%s%N 2>/dev/null || date +%s)
    timeout 8 "$BIN/xw-session-ctl" -S "$CTL_NAME" logout \
        > "$RTD/logout.out" 2>&1
    CTL_RC=$?

    # the assertion: the session must end (a wedged session IS the
    # physical "logout impossible" — no SIGTERM was ever delivered)
    if wait_gone "$SESS_PID" 60; then
        wait "$SESS_PID"
        RC=$?
        T1=$(date +%s%N 2>/dev/null || date +%s)
        MS=$(( (T1 - T0) / 1000000 ))
        check "$VNAME: logout ends the session (${MS}ms)" 0
        check "$VNAME: session exit code 0 (rc=$RC)" $([ $RC -eq 0 ]; echo $?)
        check "$VNAME: ctl socket cleaned up" \
            $([ ! -e "$RTD/$CTL_NAME.sock" ]; echo $?)
    else
        check "$VNAME: logout ends the session" 1
        echo "    $VNAME: THE SESSION NEVER EXITED — logout impossible."
        echo "    (ctl client rc=$CTL_RC, reply: $(cat "$RTD/logout.out"))"
        echo "    attribution (/proc):"
        dump_wedge "$SESS_PID" "xw-session"
        for c in $(child_pids "$SESS_PID"); do
            CMDNAME=$(tr '\0' ' ' < "/proc/$c/cmdline" 2>/dev/null)
            case "$CMDNAME" in
            *xw-compositor*) dump_wedge "$c" "xw-compositor" ;;
            esac
        done
    fi

    # storm liveness under this permutation (documentation: a wedged
    # compositor stops consuming — the heartbeat freezes)
    if [ -s "$RTD/storm.out" ]; then
        echo "    storm: $(tail -n 1 "$RTD/storm.out")"
    fi

    [ "$STORM_PID" -gt 0 ] && kill -9 "$STORM_PID" 2>/dev/null
    STORM_PID=0
    [ "$HOLDER" -gt 0 ] && kill -9 "$HOLDER" 2>/dev/null
    HOLDER=0

    # the file-sink control additionally proves trace propagation:
    # [geom] lines in the SESSION's stderr file can only come from the
    # compositor child (the session manager never prints them)
    if [ "$VSINK" = file ]; then
        N=$(rg -c 'xdg-commit-size' "$LOG" 2>/dev/null || echo 0)
        check "$VNAME: trace propagated to compositor child ($N lines)" \
            $([ "${N:-0}" -gt 1000 ]; echo $?)
    fi

    cleanup
    trap - EXIT INT TERM
}

echo "== session-trace matrix: the real supervisor chain (stalled pipe) =="
run_variant no-trace-stall 0 0 stall
run_variant input-trace-stall 1 0 stall
run_variant geometry-trace-stall 0 1 stall
run_variant both-traces-stall 1 1 stall

echo "== control: both traces into a file (the safe physical capture) =="
run_variant both-traces-file 1 1 file

echo
echo "test-session-trace: $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
