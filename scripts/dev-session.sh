#!/bin/sh
# scripts/dev-session.sh — run a full development session (headless):
# session manager + compositor + panel (M7) + exit dialog, with an
# isolated runtime dir. Usage: scripts/dev-session.sh [--logout]
#
# The session ends via the control socket (xw-session-ctl logout) or
# Ctrl+C on the manager.
set -e
cd "$(dirname "$0")/.."

RT=/tmp/xw-devsession-$$
mkdir -p "$RT"
chmod 700 "$RT"
export XDG_RUNTIME_DIR="$RT"
export XW_COMPOSITOR="$PWD/build/bin/xw-compositor"
export XW_EXIT_CMD="$PWD/build/bin/xw-exit"

# autostart the panel from an isolated HOME (full session demo)
FAKE_HOME="$RT/home"
mkdir -p "$FAKE_HOME/.config/autostart"
cat >"$FAKE_HOME/.config/autostart/xw-panel.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=xw-panel
Exec=$PWD/build/bin/xw-panel
OnlyShowIn=XFCE;
EOF
export HOME="$FAKE_HOME"

cleanup() {
    rm -rf "$RT"
}
trap cleanup EXIT INT TERM

echo "== starting session (runtime dir $RT)"
./build/bin/xw-session &
SESSION=$!

# wait for the control socket
i=0
while [ ! -S "$RT/xw-session.sock" ] && [ $i -lt 100 ]; do
    i=$((i+1))
    sleep 0.05
done

echo "== status:"
./build/bin/xw-session-ctl status
echo "== panel autostarted (workspaces, tasklist, clock, exit button)"

if [ "$1" = "--logout" ]; then
    echo "== requesting logout through the exit dialog path"
    # simulate the dialog's wire command directly (dialog needs a
    # running compositor; in CI we validate the protocol)
    ./build/bin/xw-session-ctl logout
    wait $SESSION
    echo "== session exited cleanly (rc=$?)"
else
    echo "== session running; control socket: $RT/xw-session.sock"
    echo "   try: XDG_RUNTIME_DIR=$RT ./build/bin/xw-session-ctl status"
    echo "   panel exit button: XDG_RUNTIME_DIR=$RT ./build/bin/xw-session-ctl exit-dialog"
    wait $SESSION
fi
