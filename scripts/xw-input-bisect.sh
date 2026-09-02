#!/bin/sh
# xw-input-bisect.sh — split "cursor does not move" in HALF.
#
# Context (2026-09-02): the seat repair worked — libseat is linked,
# the logind backend opens ALL 18 /dev/input nodes and 8 devices
# (3 pointers). Yet a -v (debug) capture showed ZERO input events
# of any kind in 24 seconds of live windows: not one
# "libinput: POINTER_MOTION", not one KEY, not one BUTTON.
# So the question is no longer "does the compositor get devices"
# (it does) but "do events reach libinput at all" — and this
# script answers exactly that, by testing BOTH halves:
#
#   [1] KERNEL half  — `sudo libinput debug-events` (root, opens
#       /dev/input directly, NO compositor, NO libseat involved).
#       If motion lines appear here, mouse + kernel + libinput are
#       all fine and the problem is on the compositor side.
#       (2026-09-03: if the libinput CLI is missing — it was on the
#       user's machine, same corruption family as the old seatd
#       breakage — the half falls back to a raw `dd` read of the
#       mouse's event node and counts 24-byte evdev records; no
#       package needed)
#   [2] COMPOSITOR half — the bare compositor, -v, with the mouse
#       wiggled the whole window; the script then COUNTS the
#       motion/key/button debug lines and prints a verdict:
#         kernel moves + compositor 0  -> seat-brokered fds inert
#         compositor moves            -> input fine, RENDER bug
#         kernel 0                    -> hardware/driver problem
#       (a keyboard-tap count is included: keys but no mouse
#       motion = a mouse-device-specific issue instead)
#   [3] full session window with the same counts (a working bare
#       window but a dead session window would point at the
#       session wrapper instead)
#
# EVERY count only means something if you actually MOVE THE MOUSE
# (and tap a couple of keys) during the windows — the banners say
# when.
#
# Usage (TTY login, normal user, repo root):
#     ./scripts/xw-input-bisect.sh
#
# Output: xw-input-bisect-<timestamp>.log in the repo root.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 1

TS=$(date +%Y%m%d-%H%M%S)
LOG="$ROOT/xw-input-bisect-$TS.log"
: > "$LOG" || exit 1

say() { printf '%s\n' "$*" >> "$LOG"; printf '%s\n' "$*" >&2; }
banner() { say ""; say "================ $* ================"; }
r() {
    say "\$ $*"
    "$@" >> "$LOG" 2>&1
    say "   [exit $?]"
}
rp() {
    say "\$ $*"
    eval "$*" >> "$LOG" 2>&1
    say "   [exit $?]"
}
ask() {
    _ans=""
    if [ -t 0 ]; then
        printf '%s [y/N] ' "$1" >&2
        read -r _ans || _ans=""
    else
        say "(non-interactive: question skipped: $1)"
        return 1
    fi
    case "$_ans" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}
sudo_log() {
    say "\$ (sudo) $*"
    sudo "$@" >> "$LOG" 2>&1
    _rc=$?
    say "   [exit $_rc]"
    return $_rc
}
count_in_log() {  # count_in_log <pattern>  — occurrences IN THE LOG
    grep -c "$1" "$LOG" 2>/dev/null || true
}

trap 'printf "\n>>> interrupted — the log so far is still valid: %s\n" "$LOG" >&2; exit 130' INT TERM

banner "xw-input-bisect — $TS"
banner "[0] meta"
r date
r git rev-parse --short HEAD
rp "id -nG"
r tty

KMOVES=0; KKEYS=0            # kernel-half counts
KTESTED=0                    # 1 = the kernel half actually ran
CMOVES=0; CKEYS=0; CBUTTONS=0  # compositor-half counts
CTESTED=0                    # 1 = the compositor window actually ran
AMOVES=0                     # session-window count

# ---- [1] KERNEL half: libinput as root, no compositor, no libseat ----
banner "[1] kernel/libinput half (sudo libinput debug-events — no compositor involved)"
if command -v libinput >/dev/null 2>&1; then
    if [ "$(id -u)" -eq 0 ] || ask "run 8 seconds of 'sudo libinput debug-events' now? (mouse must move!)"; then
        say ""
        say ">>> MOVE THE MOUSE CONTINUOUSLY FOR THE NEXT 8 SECONDS <<<"
        sleep 1
        if [ "$(id -u)" -eq 0 ]; then
            r timeout 8 libinput debug-events
        else
            sudo_log timeout 8 libinput debug-events
        fi
        KTESTED=1
        KMOVES=$(grep -c 'POINTER_MOTION' "$LOG" 2>/dev/null) || KMOVES=0
        KKEYS=$(grep -c ' KEY_' "$LOG" 2>/dev/null) || KKEYS=0
        say "CHECK: kernel half: $KMOVES pointer-motion event(s), $KKEYS key event(s) seen by libinput"
        if [ "$KMOVES" -eq 0 ]; then
            say "(zero motion events under ROOT — suspicious. Also dumping the"
            say " device list and recent kernel messages for the mouse:)"
            if [ "$(id -u)" -eq 0 ]; then
                r libinput list-devices
                rp "dmesg | tail -n 30"
            else
                sudo_log libinput list-devices
                sudo_log dmesg
            fi
        fi
    else
        say "(declined — the kernel half stays untested; the verdict below"
        say " will only be able to rule on the compositor half)"
    fi
else
    say "(no libinput CLI on PATH — the compositor links libinput.so"
    say " fine, so this is another damaged package like seatd was;"
    say " restore it later with: sudo pacman -S libinput)"
    say "(kernel half falls back to a RAW evdev read — no package needed)"
    MOUSENODE=$(awk -v RS='' '/H: Handlers=.*mouse/ {
        match($0, /event[0-9]+/)
        if (RSTART > 0) { print "/dev/input/" substr($0, RSTART, RLENGTH); exit }
    }' /proc/bus/input/devices 2>/dev/null)
    if [ -z "$MOUSENODE" ]; then
        say "(no pointer event node found in /proc/bus/input/devices —"
        say " the kernel half cannot run at all; the verdict below can"
        say " only rule on the compositor half)"
    elif [ "$(id -u)" -eq 0 ] || ask "read raw events from $MOUSENODE for 8s as root? (mouse must move!)"; then
        say ""
        say ">>> MOVE THE MOUSE CONTINUOUSLY FOR THE NEXT 8 SECONDS <<<"
        sleep 1
        say "\$ (sudo) timeout 8 dd if=$MOUSENODE bs=24 2>/dev/null | wc -c"
        if [ "$(id -u)" -eq 0 ]; then
            EVBYTES=$(timeout 8 dd if="$MOUSENODE" bs=24 2>/dev/null | wc -c) || EVBYTES=0
        else
            EVBYTES=$(sudo timeout 8 dd if="$MOUSENODE" bs=24 2>/dev/null | wc -c) || EVBYTES=0
        fi
        say "   raw evdev bytes read in 8s from $MOUSENODE: $EVBYTES"
        KTESTED=1
        KMOVES=$((EVBYTES / 24))
        say "CHECK: kernel half (raw): $KMOVES evdev record(s) of ANY kind"
        say "(motion+key+button together — 24 bytes per kernel event)"
        if [ "$KMOVES" -eq 0 ]; then
            say "(zero raw events under ROOT — below the compositor entirely."
            say " dmesg tail:"
            if [ "$(id -u)" -eq 0 ]; then
                rp "dmesg | tail -n 30"
            else
                sudo_log dmesg
            fi
        fi
    else
        say "(declined — the kernel half stays untested; the verdict below"
        say " will only be able to rule on the compositor half)"
    fi
fi

# ---- [2] COMPOSITOR half -----------------------------------------------------
banner "[2] compositor half (bare compositor, -v, through libseat/logind)"
LIVE=1
[ "$(id -u)" -eq 0 ] && { say "(running as root — compositor window skipped)"; LIVE=0; }
[ -e /dev/dri ] || { say "(/dev/dri missing — compositor window skipped)"; LIVE=0; }
[ -x build/bin/xw-compositor ] || { say "(build/bin/xw-compositor missing — run make first)"; LIVE=0; }
command -v timeout >/dev/null 2>&1 || { say "(timeout missing — compositor window skipped)"; LIVE=0; }

if [ "$LIVE" -eq 1 ]; then
    say ""
    say ">>> WIGGLE THE MOUSE FOR THE WHOLE 12 SECONDS, AND TAP A FEW KEYS <<<"
    sleep 1
    CTESTED=1
    say "\$ timeout 12 build/bin/xw-compositor -B drm -v"
    ( timeout 12 build/bin/xw-compositor -B drm -v 2>&1; echo "WINDOW1_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
    START=$(grep -n 'WINDOW1_EXIT_CODE' "$LOG" | tail -1 | cut -d: -f1)
    PRE=$(grep -n 'live window\|\[2\] compositor half' "$LOG" | tail -1 | cut -d: -f1)
    # count only within this window's section of the log
    WSEC=$(sed -n "${PRE},${START}p" "$LOG" 2>/dev/null)
    CMOVES=$(printf '%s\n' "$WSEC" | grep -c 'libinput: POINTER_MOTION\|xw-input: pointer motion\|compositor: cursor position updated') || CMOVES=0
    CKEYS=$(printf '%s\n' "$WSEC" | grep -c 'libinput: KEY ') || CKEYS=0
    CBUTTONS=$(printf '%s\n' "$WSEC" | grep -c 'libinput: BUTTON') || CBUTTONS=0
    say "CHECK: compositor half: $CMOVES motion line(s), $CKEYS key line(s), $CBUTTONS button line(s)"
fi

# ---- [3] full session window --------------------------------------------------
if [ "$LIVE" -eq 1 ] && [ -x build/bin/xw-session ]; then
    banner "[3] session half (xw-session --backend=drm --verbose)"
    say ""
    say ">>> WIGGLE THE MOUSE AGAIN FOR 12 SECONDS <<<"
    sleep 1
    say "\$ timeout 12 build/bin/xw-session --backend=drm --verbose"
    ( timeout 12 build/bin/xw-session --backend=drm --verbose 2>&1; echo "WINDOW2_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
    START2=$(grep -n 'WINDOW2_EXIT_CODE' "$LOG" | tail -1 | cut -d: -f1)
    PRE2=$(grep -n 'session half' "$LOG" | tail -1 | cut -d: -f1)
    WSEC2=$(sed -n "${PRE2},${START2}p" "$LOG" 2>/dev/null)
    AMOVES=$(printf '%s\n' "$WSEC2" | grep -c 'libinput: POINTER_MOTION\|xw-input: pointer motion\|compositor: cursor position updated') || AMOVES=0
    say "CHECK: session half: $AMOVES motion line(s)"
fi

# ---- [4] verdict ---------------------------------------------------------------
banner "[4] verdict"
say "kernel-half motion events:      $KMOVES"
say "compositor-half motion events:  $CMOVES"
say "compositor-half key events:     $CKEYS"
say "compositor-half button events:  $CBUTTONS"
say "session-half motion events:     $AMOVES"
say ""
if [ "$KTESTED" -eq 1 ] && [ "$KMOVES" -eq 0 ]; then
    say "VERDICT: NO motion events even as root (libinput debug-events),"
    say "with the compositor completely out of the picture. This is below"
    say "the compositor: hardware, USB port, receiver, or driver. Check"
    say "the dmesg tail above, try another port, and compare with another"
    say "mouse if possible."
elif [ "$KTESTED" -eq 1 ] && [ "$KMOVES" -gt 0 ] && [ "$CTESTED" -eq 1 ] && [ "$CMOVES" -eq 0 ] && [ "$CKEYS" -eq 0 ]; then
    say "VERDICT: the mouse, kernel and libinput all work (root test saw"
    say "motion), but ZERO events of any kind arrive through the"
    say "compositor's libseat/logind-opened fds. The seat-brokered file"
    say "descriptors are inert. SEND THIS LOG BACK — the next fix opens"
    say "the input nodes on a fallback path (or repairs the paused state)."
elif [ "$KTESTED" -eq 1 ] && [ "$KMOVES" -gt 0 ] && [ "$CTESTED" -eq 1 ] && [ "$CMOVES" -eq 0 ]; then
    say "VERDICT: keys DO arrive through the seat broker, but mouse MOTION"
    say "does not — a mouse-device-specific problem (or the mouse was still"
    say " during the window: re-run to be sure). SEND THIS LOG BACK."
elif [ "$CMOVES" -gt 0 ] && [ "$AMOVES" -eq 0 ] && [ "$CTESTED" -eq 1 ] && [ -x build/bin/xw-session ]; then
    say "VERDICT: the BARE compositor receives motion, but the full"
    say "session does NOT. Something in the session wrapper interferes"
    say "with input. SEND THIS LOG BACK."
elif [ "$CMOVES" -gt 0 ]; then
    say "VERDICT: the compositor DOES receive motion events — the input"
    say "pipeline works. The frozen picture is a RENDER/presentation"
    say "problem (damage, repaint or page flip). SEND THIS LOG BACK —"
    say "the next fix targets the DRM present path."
else
    say "VERDICT: incomplete data (a window was skipped, sudo was declined,"
    say "or the mouse was not moved). Re-run, MOVE THE MOUSE in every"
    say "window, tap some keys, then send the log back."
fi
say ""
say "(if any count is 0 only because you did not actually move the"
say " mouse / tap keys during a window, the verdict is void — re-run)"

banner "DONE"
say "log file: $LOG"
printf '\n============================================================\n' >&2
printf 'DONE. The deliverable is this file:\n  %s\n' "$LOG" >&2
printf '(send it back — the [4] verdict section says what happens next)\n' >&2
printf '============================================================\n\n' >&2
