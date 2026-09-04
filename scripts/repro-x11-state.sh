#!/bin/bash
# repro-x11-state.sh — real-client X11/Wayland integration recon.
#
# Starts the in-tree compositor + real Xwayland (rootless) + xw-xwm +
# real X11 clients, then dumps BOTH sides of the world:
#   - X side: xprop (root + every client window), xwininfo -tree,
#     wmctrl -l (the EWMH taskbar view), xdpyinfo focus
#   - compositor side: window map/title/focus log lines
#
# Usage: scripts/repro-x11-state.sh [client-binary ...]
#   default clients: xeyes xterm
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

APPS="$ROOT/.apps-root"
export LD_LIBRARY_PATH="$APPS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
for b in xprop xwininfo wmctrl xdpyinfo xdotool xeyes xterm Xwayland; do
    [ -x "$APPS/usr/bin/$b" ] || { echo "missing probe: $b (fetch-test-apps.sh)"; exit 1; }
done

RTD="$(mktemp -d /tmp/xw-repro.XXXXXX)"
export XDG_RUNTIME_DIR="$RTD"
D=:7
SOCK=xwx

CLIENTS=("$@")
[ ${#CLIENTS[@]} -eq 0 ] && CLIENTS=(xeyes xterm)

COMPID=""; XPID=""; WMPID=""; CLPIDS=()
cleanup() {
    for p in "${CLPIDS[@]:-}" "$WMPID" "$XPID" "$COMPID"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
    sleep 0.3
    for p in "${CLPIDS[@]:-}" "$WMPID" "$XPID" "$COMPID"; do
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    done
    rm -rf "$RTD" /tmp/.X11-unix/X7
}
trap cleanup EXIT

"$ROOT/build/bin/xw-compositor" -s "$SOCK" -v > "$RTD/comp.log" 2>&1 &
COMPID=$!
sleep 0.7
kill -0 $COMPID 2>/dev/null || { echo "FAIL: compositor died"; cat "$RTD/comp.log"; exit 1; }

WAYLAND_DISPLAY=$SOCK "$APPS/usr/bin/Xwayland" "$D" -rootless -noreset > "$RTD/xw.log" 2>&1 &
XPID=$!
sleep 2

XDG_RUNTIME_DIR=$RTD DISPLAY=$D "$ROOT/build/bin/xw-xwm" -d "$D" -w "$SOCK" -v > "$RTD/wm.log" 2>&1 &
WMPID=$!
sleep 1

for c in "${CLIENTS[@]}"; do
    DISPLAY=$D timeout 25 "$APPS/usr/bin/$c" > "$RTD/$c.log" 2>&1 &
    CLPIDS+=($!)
done
sleep 3

XENV="DISPLAY=$D LD_LIBRARY_PATH=$LD_LIBRARY_PATH XDG_RUNTIME_DIR=$RTD"
echo "================ X side: window tree ================"
eval "$XENV $APPS/usr/bin/xwininfo -root -tree" 2>&1 | head -40
echo
echo "================ X side: wmctrl -l (EWMH taskbar) ================"
eval "$XENV $APPS/usr/bin/wmctrl -l" 2>&1 | head -15
echo
echo "================ X side: root properties ================"
eval "$XENV $APPS/usr/bin/xprop -root" 2>&1 | grep -vE "^$|command timeout|XKB rules" | head -30
echo
echo "================ X side: per-client windows ================"
for wid in $(eval "$XENV $APPS/usr/bin/xlsclients -l" 2>/dev/null | grep -oE "0x[0-9a-f]+" | sort -u); do
    echo "--- window $wid:"
    eval "$XENV $APPS/usr/bin/xprop -id $wid" 2>&1 | grep -E "WM_NAME|WM_CLASS|WM_STATE|WM_PROTOCOLS|WM_NORMAL_HINTS|WM_TRANSIENT|WM_HINTS|_NET_WM|window state" | head -12
done
echo
echo "================ X side: input focus ================"
eval "$XENV $APPS/usr/bin/xdpyinfo" 2>&1 | grep -A2 "focus:"
echo
echo "================ compositor side: windows ================"
grep -E "MAPPED|UNMAPPED|focus|title|serial" "$RTD/comp.log" | tail -25
echo
echo "================ xwm side (tail) ================"
grep -vE "debug" "$RTD/wm.log" | tail -20
echo
echo "(logs kept in $RTD)"
