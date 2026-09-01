#!/bin/sh
# scripts/dev-session.sh — run a full development session (headless):
# session manager + compositor + panel (M7) + exit dialog, with an
# isolated runtime dir. Usage: scripts/dev-session.sh [--logout]
#
# The session ends via the control socket (xw-session-ctl logout) or
# Ctrl+C on the manager.
#
# Fail-fast policy: the script refuses to launch anything unless every
# binary it needs was successfully built. It never "continues" past a
# failed or absent build (regression tested in
# scripts/test-build-regressions.sh).
#
# Portability: plain POSIX sh; verified under dash, bash and zsh. All
# test operands are quoted so an empty variable can never turn a
# numeric test into a parser error under zsh.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Pick up the optional local sysroot (runtime library paths included:
# a sysroot-built libinput pulls its own dependencies from there). On
# normal distributions with system-wide packages this is a no-op.
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

BIN="$ROOT/build/bin"

# ---- preflight: refuse to run on an incomplete build ------------------
# Literal list (not a variable): zsh does not word-split unquoted
# parameter expansions, so a $LIST here would break under zsh.
for b in xw-compositor xw-session xw-session-ctl xw-exit xw-panel; do
    if [ ! -x "$BIN/$b" ]; then
        echo "dev-session: $BIN/$b does not exist or is not executable." >&2
        echo "  The build did not complete (or was never run)." >&2
        echo "  Fix the build first:  . scripts/env.sh && make" >&2
        echo "  This script never launches binaries that were not built." >&2
        exit 1
    fi
done

RT=/tmp/xw-devsession-$$
mkdir -p "$RT"
chmod 700 "$RT"
export XDG_RUNTIME_DIR="$RT"
export XW_COMPOSITOR="$BIN/xw-compositor"
export XW_EXIT_CMD="$BIN/xw-exit"

# autostart the panel from an isolated HOME (full session demo)
FAKE_HOME="$RT/home"
mkdir -p "$FAKE_HOME/.config/autostart"
cat >"$FAKE_HOME/.config/autostart/xw-panel.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=xw-panel
Exec=$BIN/xw-panel
OnlyShowIn=XFCE;
EOF
export HOME="$FAKE_HOME"

SESSION=""
cleanup() {
    if [ -n "$SESSION" ] && kill -0 "$SESSION" 2>/dev/null; then
        kill "$SESSION" 2>/dev/null || true
    fi
    rm -rf "$RT"
}
trap cleanup EXIT INT TERM

echo "== starting session (runtime dir $RT)"
"$BIN/xw-session" &
SESSION=$!

# wait for the control socket, but fail fast (with a diagnostic) if the
# manager dies or the socket never appears
i=0
while [ ! -S "$RT/xw-session.sock" ]; do
    if ! kill -0 "$SESSION" 2>/dev/null; then
        wait "$SESSION" || rc=$?
        echo "dev-session: the session manager exited early (rc=${rc:-?})" >&2
        echo "  Run it in the foreground to see the error:" >&2
        echo "    XDG_RUNTIME_DIR=$RT $BIN/xw-session" >&2
        SESSION=""
        exit 1
    fi
    if [ "$i" -ge 100 ]; then
        echo "dev-session: no control socket at $RT/xw-session.sock after 5s" >&2
        echo "  Run the manager in the foreground to see the error:" >&2
        echo "    XDG_RUNTIME_DIR=$RT $BIN/xw-session" >&2
        kill "$SESSION" 2>/dev/null || true
        SESSION=""
        exit 1
    fi
    i=$((i + 1))
    sleep 0.05
done

echo "== status:"
"$BIN/xw-session-ctl" status
# The panel must actually RUN, not merely be spawned: an autostart entry
# that dies instantly (bad Exec, missing libs) previously looked exactly
# like a working one here. The session manager now logs every child exit
# (see the log above); this check closes the loop from the script side.
sleep 1
if command -v pgrep >/dev/null 2>&1; then
    if pgrep -x xw-panel >/dev/null 2>&1; then
        echo "== panel running (workspaces, tasklist, clock, exit button)"
    else
        echo "dev-session: WARNING: xw-panel is NOT running" >&2
        echo "  Check the session log above for an exit line (a 127 exit" >&2
        echo "  means the Exec= path is wrong; other codes mean a startup" >&2
        echo "  failure — run it by hand to see the error)." >&2
    fi
else
    echo "== panel autostarted; child exits are logged by the session manager"
fi

rc=0
if [ "${1:-}" = "--logout" ]; then
    echo "== requesting logout through the exit dialog path"
    # simulate the dialog's wire command directly (dialog needs a
    # running compositor; in CI we validate the protocol)
    "$BIN/xw-session-ctl" logout
    wait "$SESSION" || rc=$?
    echo "== session exited (rc=$rc)"
    [ "$rc" -eq 0 ] || exit "$rc"
else
    echo "== session running; control socket: $RT/xw-session.sock"
    echo "   try: XDG_RUNTIME_DIR=$RT $BIN/xw-session-ctl status"
    echo "   panel exit button: XDG_RUNTIME_DIR=$RT $BIN/xw-session-ctl exit-dialog"
    wait "$SESSION" || rc=$?
    exit "$rc"
fi
