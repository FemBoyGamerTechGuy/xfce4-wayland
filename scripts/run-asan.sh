#!/bin/sh
# run-asan.sh — sanitizer regression pass (ASan + UBSan + LeakSanitizer).
#
# Rebuilds everything with sanitizers in place, runs the in-process test
# suite and the process-level session test, then restores the release
# build. Exit non-zero on any failure or sanitizer report.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

. "$(dirname "$0")/env.sh" >/dev/null 2>&1 || true

# Sanitizers are driven by `make PROFILE=asan` (single switch, guarded
# build tree); the runtime options below stay in this script.
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

status=0
LOGDIR="${TMPDIR:-/tmp}"

echo "== asan: building with sanitizers (PROFILE=asan) =="
make clean >/dev/null
if ! make all PROFILE=asan \
        >"$LOGDIR/xw-asan-build.log" 2>&1; then
    rg -v '^(cc|ar|mkdir|/home/.*/wayland-scanner|python3) ' \
        "$LOGDIR/xw-asan-build.log" | head -20
    echo "asan: BUILD FAILED"
    exit 2
fi

echo "== asan: in-process test suite =="
if ! make tests PROFILE=asan \
        >"$LOGDIR/xw-asan-tests.log" 2>&1; then
    status=1
fi
rg 'tests passed|FAIL|FATAL' "$LOGDIR/xw-asan-tests.log" | head -20
if rg -q 'AddressSanitizer|LeakSanitizer|runtime error|FATAL' \
        "$LOGDIR/xw-asan-tests.log"; then
    rg -m5 'AddressSanitizer|LeakSanitizer|runtime error|FATAL' \
        "$LOGDIR/xw-asan-tests.log"
    echo "asan: sanitizer reports in the test suite"
    status=1
fi
if ! rg -q '[0-9]+/[0-9]+ tests passed, 0 failures' \
        "$LOGDIR/xw-asan-tests.log"; then
    echo "asan: test suite failures"
    status=1
fi

echo "== asan: child-process leak check (exit dialog log) =="
if [ -f /tmp/xw-exit-child.log ] &&
   rg -q 'AddressSanitizer|LeakSanitizer' /tmp/xw-exit-child.log 2>/dev/null; then
    head -10 /tmp/xw-exit-child.log
    echo "asan: sanitizer reports in the dialog child"
    status=1
fi

echo "== asan: process-level session test =="
if ! sh scripts/test-session.sh >"$LOGDIR/xw-asan-session.log" 2>&1; then
    status=1
fi
tail -2 "$LOGDIR/xw-asan-session.log"
if rg -q 'AddressSanitizer|LeakSanitizer' "$LOGDIR/xw-asan-session.log"; then
    rg -m5 'AddressSanitizer|LeakSanitizer' "$LOGDIR/xw-asan-session.log"
    echo "asan: sanitizer reports in the session processes"
    status=1
fi

echo "== asan: restoring release build =="
make clean >/dev/null
if ! make all >"$LOGDIR/xw-asan-release.log" 2>&1; then
    echo "asan: release rebuild failed"
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "asan: PASS"
else
    echo "asan: FAIL"
fi
exit $status
