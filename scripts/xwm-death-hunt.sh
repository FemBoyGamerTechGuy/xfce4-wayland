#!/bin/bash
# xwm-death-hunt.sh — hunt the intermittent Xwayland/WM death.
# Runs the exact xwm-configure-mask test repeatedly with verbose
# compositor logs; when the WM reports POLLHUP, reports whether
# XWAYLAND died (and its wait status) or only the WM lost its socket.
cd /home/z/my-project
. ./scripts/env.sh >/dev/null 2>&1
fails=0
for i in $(seq 1 "${1:-8}"); do
    log=/tmp/hunt-$i.log
    XWT_LOG_LEVEL=1 XWT_FILTER=xwm-configure-mask timeout 40 ./build/tests/run-tests > "$log" 2>&1
    rc=$?
    if grep -q "fatal: X server connection lost" "$log" 2>/dev/null || [ $rc -ne 0 ]; then
        fails=$((fails+1))
        echo "=== run $i: rc=$rc"
        grep -E "fatal|FAIL|FATAL" "$log" | head -3
        D=$(ls -dt /tmp/xwt-* | head -1)
        grep -E "fatal" "$D/xwm-0.log" 2>/dev/null
        grep "\[xw-" "$log" | tail -6
    else
        echo "run $i: ok"
    fi
done
echo "failures: $fails"
