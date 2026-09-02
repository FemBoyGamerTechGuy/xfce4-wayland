#!/bin/sh
# xw-seat-fix.sh — diagnose AND fix the situation the capture log
# exposed: the seatd package is installed but carries no libseat
# development files, so the compositor keeps building without libseat
# and the elogind/logind seat path stays compiled out (frozen input).
#
# Everything it does — every check, every command, every result — is
# written into ONE log file you hand back afterwards:
#     xw-seat-fix-<timestamp>.log   (repo root; *.log is git-ignored)
#
# Decision order:
#   0. compositor already linked with libseat -> nothing to do.
#   1. libseat dev files reachable   -> make clean && make, verify.
#   2. dev files absent but pacman present ->
#         reinstall seatd from the repos (sudo, you confirm at the
#         prompts), re-check, rebuild if the files appeared.
#   3. still absent -> seatd DAEMON route: 'seat' group + service
#      enable/start per your init (or a manual start for today) —
#      needs ONE log-out/log-in. The 'input' group one-liner is
#      printed as the documented simple fallback either way.
#
# Privileged commands are ALWAYS printed before running and asked
# y/N first (skipped automatically when stdin is not a terminal).
# This script never chmods devices, never relaxes permissions on
# /dev/input, and never runs the compositor as root.
#
# Usage (from the TTY login, as your normal user, in the repo root):
#     ./scripts/xw-seat-fix.sh
#
# Afterwards: ./scripts/xw-tty-capture.sh  — move the mouse in both
# windows, then send BOTH .log files back.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 1

TS=$(date +%Y%m%d-%H%M%S)
LOG="$ROOT/xw-seat-fix-$TS.log"
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
# ask "question"  -> returns 0 if the user answered y (interactive only)
ask() {
    _ans=""
    if [ -t 0 ]; then
        printf '%s [y/N] ' "$1" >&2
        read -r _ans || _ans=""
    else
        say "(non-interactive run: question skipped: $1)"
        return 1
    fi
    case "$_ans" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}
# one privileged command, printed first, run on the terminal
sudo_run() {
    say "\$ (sudo) $*"
    sudo "$@"
    say "   [exit $?]"
}

trap 'printf "\n>>> interrupted — the log so far is still valid: %s\n" "$LOG" >&2; exit 130' INT TERM

# ---------------------------------------------------------------- facts
banner "xw-seat-fix — $TS"
banner "[0] meta"
r date
r git rev-parse --short HEAD
r git status --short

banner "[1] seatd package and libseat files"
HAVE_PACMAN=; HAVE_SEATD_BIN=; PC_OK=; LINKED=
command -v pacman >/dev/null 2>&1 && HAVE_PACMAN=y
command -v seatd  >/dev/null 2>&1 && HAVE_SEATD_BIN=y
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libseat 2>/dev/null; then
    PC_OK=y
fi
if ldd build/bin/xw-compositor 2>/dev/null | grep -q libseat; then
    LINKED=y
fi

[ -n "$HAVE_PACMAN" ] && { r pacman -Qi seatd; r pacman -Si seatd; }
[ -n "$HAVE_PACMAN" ] && r pacman -Ql seatd
r ls -l /usr/lib/pkgconfig/libseat.pc /usr/include/seat.h
r ls -l /usr/lib/libseat.so /usr/lib/libseat.so.1
[ -n "$HAVE_SEATD_BIN" ] && r seatd -h
r ls -l build/bin/xw-compositor
r cat build/.features
if [ -n "$PC_OK" ]; then
    say "CHECK: pkg-config sees libseat dev files"
else
    say "CHECK: pkg-config does NOT see libseat dev files"
fi
if [ -n "$LINKED" ]; then
    say "CHECK: xw-compositor IS linked with libseat"
else
    say "CHECK: xw-compositor is NOT linked with libseat"
fi

banner "[2] init system, seatd service files, groups"
INIT=unknown
if [ -d /run/runit/service ] || command -v runsvdir >/dev/null 2>&1; then
    INIT=runit
elif command -v rc-service >/dev/null 2>&1; then
    INIT=openrc
elif command -v dinitctl >/dev/null 2>&1; then
    INIT=dinit
elif command -v s6-rc >/dev/null 2>&1; then
    INIT=s6
elif command -v systemctl >/dev/null 2>&1; then
    INIT=systemd
fi
say "detected init: $INIT"
r ls -ld /etc/runit/sv/seatd /etc/init.d/seatd /etc/dinit.d/seatd
r ls -l /usr/lib/systemd/system/seatd.service
rp "grep '^seat:' /etc/group || true"
r id -nG
IN_SEAT=
case " $(id -nG 2>/dev/null) " in *" seat "*) IN_SEAT=y ;; esac
[ -n "$IN_SEAT" ] && say "CHECK: this login is in the 'seat' group" \
                   || say "CHECK: this login is NOT in the 'seat' group (note: a just-added group only shows after the next login)"

# ---------------------------------------------------------------- route 0
banner "[3] decision"
if [ -n "$LINKED" ]; then
    say "Route 0: xw-compositor already has libseat — nothing to fix here."
    say "Run ./scripts/xw-tty-capture.sh to test the live machine."
    say "log file: $LOG"
    exit 0
fi

# ---------------------------------------------------------------- route 1+2
REBUILT=
if [ -n "$PC_OK" ]; then
    say "Route 1: libseat dev files are reachable — rebuilding now."
else
    say "Route 2: dev files absent — the installed seatd package does not carry them."
    if [ -n "$HAVE_PACMAN" ]; then
        if ask "reinstall seatd from your repos now (sudo pacman -S seatd; answer 'y' at pacman's own reinstall prompt)?"; then
            sudo_run pacman -S seatd
            r pacman -Qi seatd
            r pacman -Ql seatd
            r ls -l /usr/lib/pkgconfig/libseat.pc /usr/include/seat.h /usr/lib/libseat.so
            if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libseat 2>/dev/null; then
                PC_OK=y
                say "CHECK: after the reinstall, pkg-config sees libseat dev files"
            else
                say "CHECK: after the reinstall, pkg-config STILL does not see libseat dev files"
            fi
        else
            say "(reinstall declined — continuing without it)"
        fi
    else
        say "(pacman not found — cannot reinstall the package here)"
    fi
fi

if [ -n "$PC_OK" ]; then
    say "\$ make clean && make   (a minute or two; output follows)"
    ( make clean 2>&1 && make 2>&1; echo "MAKE_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
    r cat build/.features
    r ldd build/bin/xw-compositor
    if ldd build/bin/xw-compositor 2>/dev/null | grep -q libseat; then
        REBUILT=y
        say "CHECK: SUCCESS — xw-compositor is now linked with libseat."
        say "The elogind provider will engage on the next run; your login"
        say "session is already active, so NO log-out is needed."
    else
        say "CHECK: rebuild finished but libseat is STILL not linked —"
        say "the make output above (warning box included) explains why."
    fi
else
    say "(no rebuild attempted: libseat dev files are not reachable, make"
    say " would only re-print its warning box — see the capture log instead)"
fi

# ---------------------------------------------------------------- route 3
if [ -z "$REBUILT" ]; then
    banner "[3b] fallback: the seatd daemon route (no libseat needed)"
    say "The compositor's built-in seatd client works without libseat:"
    say "enable the seatd daemon and join the 'seat' group; device fds"
    say "are then granted per active session (the proper, secure path)."
    say ""
    say "Documented simple alternative (direct-VT machines, single-user):"
    say "    sudo gpasswd -a $USER input    # then log out and back in"
    say "The direct provider then opens the devices with your own group"
    say "membership. This bypasses the seat manager for input devices."
    say ""
    if [ -z "$HAVE_SEATD_BIN" ]; then
        say "seatd binary not found — install it first:"
        say "    sudo pacman -S seatd"
        say "then run this script again."
    else
        if ask "add $USER to the 'seat' group now (sudo gpasswd)?"; then
            sudo_run gpasswd -a "$USER" seat
        else
            say "(run it yourself: sudo gpasswd -a $USER seat)"
        fi
        SVC_DONE=
        if [ "$INIT" = runit ] && [ -d /etc/runit/sv/seatd ]; then
            if ask "enable + start the seatd runit service now (ln -s into /run/runit/service)?"; then
                sudo_run ln -s /etc/runit/sv/seatd /run/runit/service/
                SVC_DONE=y
            fi
        elif [ "$INIT" = openrc ] && [ -e /etc/init.d/seatd ]; then
            if ask "enable + start the seatd OpenRC service now (rc-update + rc-service)?"; then
                sudo_run rc-update add seatd default
                sudo_run rc-service seatd start
                SVC_DONE=y
            fi
        elif [ "$INIT" = dinit ] && [ -e /etc/dinit.d/seatd ]; then
            if ask "enable + start the seatd dinit service now (dinitctl)?"; then
                sudo_run dinitctl enable seatd
                sudo_run dinitctl start seatd
                SVC_DONE=y
            fi
        elif [ "$INIT" = systemd ]; then
            if ask "enable + start seatd now (systemctl enable --now)?"; then
                sudo_run systemctl enable --now seatd
                SVC_DONE=y
            fi
        else
            say "no service file found for init '$INIT' in the standard paths;"
            if [ -n "$HAVE_PACMAN" ]; then
                say "your distro ships them as split packages — try:"
                say "    sudo pacman -S seatd-$INIT"
            fi
            if ask "start seatd manually for TODAY instead (survives until reboot)?"; then
                sudo_run sh -c 'nohup seatd </dev/null >/dev/null 2>&1 &'
                SVC_DONE=y
            fi
        fi
        sleep 1
        r ls -l /run/seatd.sock
        if [ -S /run/seatd.sock ]; then
            say "CHECK: seatd daemon is running (/run/seatd.sock exists)"
        else
            say "CHECK: /run/seatd.sock not there (yet) — see the log lines above"
        fi
        [ -n "$SVC_DONE" ] || say "(service not started — run the printed commands yourself)"
    fi
    banner "[4] IMPORTANT — one log-out, then test"
    say "The 'seat' (or 'input') group only takes effect at your NEXT login:"
    say "  1. log out of this TTY and log back in"
    say "  2. run: ./scripts/xw-tty-capture.sh   (move the mouse in both windows)"
    say "  3. send back BOTH files: this fix log AND the new capture log"
fi

banner "DONE"
say "log file: $LOG"
printf '\n============================================================\n' >&2
printf 'DONE. The deliverable is this file:\n  %s\n' "$LOG" >&2
printf '(send it back together with a fresh capture log if asked)\n' >&2
printf '============================================================\n\n' >&2
