#!/bin/sh
# xw-tty-capture.sh — one-shot diagnostic capture for real-TTY runs.
#
# Purpose: run EVERYTHING needed to debug a TTY session in one go and
# record every command and all of its output into ONE log file that
# can simply be handed back for analysis. No root, no device changes,
# no config edits — it only reads state and runs the compositor and
# the session exactly the way a manual test would.
#
# Usage (from the TTY login, as your normal user, in the repo root):
#     ./scripts/xw-tty-capture.sh [seconds-per-window]
# Default: 12 seconds per live window.
#
# It captures, in order:
#   [0] meta        — date, kernel, user, git HEAD, tree cleanliness
#   [1] packages    — pkg-config versions, seatd package, ldd of both
#                     binaries (the "did the rebuild link libseat?"
#                     check lives here), binary timestamps
#   [2] build       — build/.features feature stamp
#   [3] seat/env    — session id, loginctl session state, /dev/dri and
#                     /dev/input permissions, seatd socket, session bus
#   [4] live run 1  — the bare compositor (-B drm -v): isolates the
#                     seat/input path from the session manager
#   [5] live run 2  — the full session (--backend=drm --verbose)
#   [6] digest      — the interesting lines of everything above
#
# In both live windows: MOVE THE MOUSE and press keys as soon as the
# desktop appears. Let each window end by itself (timeout sends
# SIGTERM; the compositor restores the VT cleanly) — avoid Ctrl-C.
#
# Output: xw-tty-capture-YYYYMMDD-HHMMSS.log in the repository root
# (*.log is git-ignored). The path is printed at the end — that file
# is the deliverable.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 1

SECS=${1:-12}
case "$SECS" in ''|*[!0-9]*) SECS=12 ;; esac

TS=$(date +%Y%m%d-%H%M%S)
LOG="$ROOT/xw-tty-capture-$TS.log"
: > "$LOG" || exit 1

# say: banner/instruction -> log AND console (stderr, so it never
# mixes into command output redirections)
say() { printf '%s\n' "$*" >> "$LOG"; printf '%s\n' "$*" >&2; }
banner() { say ""; say "================ $* ================"; }

# r: run ONE command (direct execution, no shell string), recording
# the command line, all output, and the exit code into the log
r() {
    say "\$ $*"
    "$@" >> "$LOG" 2>&1
    say "   [exit $?]"
}

# rp: like r but for a pipeline that needs grep (self-documented)
rp() {
    say "\$ $*"
    eval "$*" >> "$LOG" 2>&1
    say "   [exit $?]"
}

trap 'printf "\n>>> interrupted — the log so far is still valid: %s\n" "$LOG" >&2; exit 130' INT TERM

banner "xw-tty-capture — $TS"
say "seconds per live window: $SECS"

# ---- [0] meta ------------------------------------------------------------
banner "[0] meta"
r date
r uname -a
r id
r tty
r pwd
r git rev-parse --short HEAD
r git log -n 1 --format=%h\ %ci\ %s
r git status --short

# ---- [1] packages and linkage --------------------------------------------
banner "[1] packages and linkage (the libseat link check lives here)"
for p in libseat libinput libdrm libudev xkbcommon wayland-server wayland-client pixman-1; do
    r pkg-config --modversion "$p"
done
command -v pacman >/dev/null 2>&1 && r pacman -Qi seatd
r ls -l build/bin/xw-session build/bin/xw-compositor
r ldd build/bin/xw-compositor
r ldd build/bin/xw-session
if ldd build/bin/xw-compositor 2>/dev/null | grep -qi libseat; then
    say "CHECK: libseat IS linked into xw-compositor"
else
    say "CHECK: libseat is NOT linked into xw-compositor — the rebuild did not pick it up (this alone explains a still-frozen mouse)"
fi

# ---- [2] build state ------------------------------------------------------
banner "[2] build state"
r cat build/.features
if grep -q 'libseat=y' build/.features 2>/dev/null; then
    say "CHECK: build/.features records libseat=y"
else
    say "CHECK: build/.features does NOT record libseat=y"
fi
r stat -c %y\ %n build/bin/xw-session build/bin/xw-compositor

# ---- [3] seat and session environment -------------------------------------
banner "[3] seat and session environment"
say "\$ env | grep -E '^(XDG_|WAYLAND_|DISPLAY|DBUS_|LIBSEAT_|SEATD_)' | sort"
env | grep -E '^(XDG_|WAYLAND_|DISPLAY|DBUS_|LIBSEAT_|SEATD_)' | sort >> "$LOG" 2>&1
say "   [exit $?]"
if command -v loginctl >/dev/null 2>&1 && [ -n "${XDG_SESSION_ID:-}" ]; then
    r loginctl show-session "$XDG_SESSION_ID" -p Active -p State -p Type -p Seat -p Name
else
    say "(loginctl session query skipped: loginctl missing or XDG_SESSION_ID unset)"
fi
r ls -l /dev/dri/
r ls -l /dev/input/
r ls -l /run/seatd.sock
if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    r ls -l "$XDG_RUNTIME_DIR/bus"
else
    say "(XDG_RUNTIME_DIR unset — session bus socket not checked)"
fi

# ---- [4]/[5] live windows -------------------------------------------------
LIVE=1
if [ "$(id -u)" -eq 0 ]; then
    banner "running as ROOT — live windows SKIPPED (never run the session as root)"
    LIVE=0
fi
if [ ! -e /dev/dri ]; then
    banner "/dev/dri missing — live windows SKIPPED (run this from the real TTY login)"
    LIVE=0
fi
if [ ! -x build/bin/xw-session ]; then
    banner "build/bin/xw-session missing — live windows SKIPPED (run make first)"
    LIVE=0
fi
if ! command -v timeout >/dev/null 2>&1; then
    banner "coreutils timeout missing — live windows SKIPPED"
    LIVE=0
fi

if [ "$LIVE" -eq 1 ]; then
    banner "[4] live window 1: bare compositor, ${SECS}s"
    printf '\n>>> WINDOW 1 of 2: bare compositor for %ss — MOVE THE MOUSE and press keys NOW <<<\n\n' "$SECS" >&2
    say "\$ timeout $SECS build/bin/xw-compositor -B drm -v"
    ( timeout "$SECS" build/bin/xw-compositor -B drm -v 2>&1; echo "WINDOW1_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
    say "[4] window 1 ended (exit code above; 124 = ended by timeout, which is normal)"
    sleep 2
    banner "[5] live window 2: full session, ${SECS}s"
    printf '\n>>> WINDOW 2 of 2: full session for %ss — MOVE THE MOUSE again NOW <<<\n\n' "$SECS" >&2
    say "\$ timeout $SECS build/bin/xw-session --backend=drm --verbose"
    ( timeout "$SECS" build/bin/xw-session --backend=drm --verbose 2>&1; echo "WINDOW2_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
    say "[5] window 2 ended (exit code above; 124 = ended by timeout, which is normal)"
    sleep 1
fi

# ---- [6] digest ------------------------------------------------------------
banner "[6] digest (last interesting lines)"
rp "grep -nEi 'error|warn|fail|denied|refus|seat:|input|device|cursor|pointer|motion|CHECK:|EXIT_CODE' '$LOG' | tail -n 120"

banner "DONE"
say "log file: $LOG"
printf '\n============================================================\n' >&2
printf 'DONE. The deliverable is this file:\n  %s\n' "$LOG" >&2
printf '(send it back and it will be read top to bottom)\n' >&2
printf '============================================================\n\n' >&2
ls -l "$LOG" >&2
