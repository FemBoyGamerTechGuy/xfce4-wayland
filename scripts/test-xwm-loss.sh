#!/bin/bash
# test-xwm-loss.sh — the WM helper must die honestly when the compositor
# dies.
#
# Regression for the event-loop gap found while hunting the intermittent
# "xw-xwm exit at stack teardown": the helper's main loop NEVER checked
# the wl fd for POLLHUP/POLLERR and ignored wl_display_dispatch()
# errors. When the compositor died while Xwayland lived, the helper spun
# at 100% CPU on a HUPed-but-not-readable fd (poll never blocks, no
# branch fires) until Xwayland eventually died too — and the failure was
# misreported as "X server connection lost".
#
# Reproduction: freeze Xwayland with SIGSTOP so it cannot notice its own
# dead wl connection (its X socket to the helper stays open), then
# SIGKILL the compositor. The helper must exit within seconds with
# "compositor connection lost" — the OLD code hangs forever, which is
# exactly what this test catches.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

APPS="$ROOT/.apps-root"
[ -x "$APPS/usr/bin/Xwayland" ] || { echo "SKIP: apps-root has no Xwayland"; exit 0; }
export LD_LIBRARY_PATH="$APPS/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

RTD="$(mktemp -d /tmp/xw-loss.XXXXXX)"
export XDG_RUNTIME_DIR="$RTD"
CLOG="$RTD/compositor.log"
XLOG="$RTD/xwayland.log"
WLOG="$RTD/xwm.log"
COMP= XP= XWM=

cleanup() {
    for p in "${COMP:-}" "${XP:-}" "${XWM:-}"; do
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    done
    rm -f /tmp/.X11-unix/X6
    rm -rf "$RTD"
}
trap cleanup EXIT

# 1. compositor
"$ROOT/build/bin/xw-compositor" -s xwxloss -v >"$CLOG" 2>&1 &
COMP=$!
sleep 0.6
kill -0 "$COMP" 2>/dev/null || { echo "FAIL: compositor died at startup"; cat "$CLOG"; exit 1; }

# 2. Xwayland rootless
WAYLAND_DISPLAY=xwxloss "$APPS/usr/bin/Xwayland" :6 -rootless -noreset >"$XLOG" 2>&1 &
XP=$!
sleep 2.5
kill -0 "$XP" 2>/dev/null || { echo "FAIL: Xwayland died at startup"; cat "$XLOG"; exit 1; }

# 3. the WM helper
"$ROOT/build/bin/xw-xwm" -d :6 -w xwxloss -v >"$WLOG" 2>&1 &
XWM=$!
sleep 1.5
kill -0 "$XWM" 2>/dev/null || { echo "FAIL: xw-xwm died at startup"; cat "$WLOG"; exit 1; }
grep -q "xw-xwm ready" "$WLOG" || { echo "FAIL: xw-xwm never became ready"; cat "$WLOG"; exit 1; }

# 4. freeze Xwayland: it must NOT die when the compositor does (that is
#    what makes this a pure wl-loss reproduction — the X side stays up)
kill -STOP "$XP"

# 5. kill the compositor (the helper's wl side dies, its X side lives)
kill -9 "$COMP"

# 6. the helper must exit promptly: poll up to 5s
exited=1
for i in $(seq 1 50); do
    kill -0 "$XWM" 2>/dev/null || { exited=0; break; }
    sleep 0.1
done
if [ "$exited" -ne 0 ]; then
    echo "FAIL: xw-xwm still alive 5s after compositor death"
    echo "(the old event loop spun forever on the HUPed wl fd)"
    echo "--- xwm log tail:"
    tail -5 "$WLOG"
    exit 1
fi

# 7. it must say WHY it died — the honest diagnosis, not an X-side blame
if ! grep -q "compositor connection lost" "$WLOG"; then
    echo "FAIL: xw-xwm exited but not with the compositor-loss diagnosis"
    echo "--- xwm log tail:"
    tail -5 "$WLOG"
    exit 1
fi

echo "xw-xwm triaged the compositor loss correctly:"
grep "compositor connection lost" "$WLOG" | head -1
echo "--- VERDICT: PASS"
exit 0
