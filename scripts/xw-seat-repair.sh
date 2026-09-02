#!/bin/sh
# xw-seat-repair.sh — repair the CORRUPTED seatd package installation
# that the last two logs proved:
#   * /usr/lib/libseat.so.1, /usr/lib/pkgconfig/libseat.pc AND
#     /usr/bin/seatd are ZERO-BYTE files on this machine,
#   * the pacman local-database entry for seatd is empty
#     (pacman -Ql lists nothing, pacman -Qi shows all "None"),
#   * therefore every "reinstall" was cosmetic and the seatd daemon
#     could never start (its binary is an empty file).
#
# What this script does (every step logged to ONE file you hand back):
#   [1] disk space — a full / or /usr is the classic cause of exactly
#       this corruption; the script STOPS if a partition is >= 99%
#   [2] before-evidence of the corruption (file sizes, DB entries)
#   [3] the repair: drop the broken DB entry, delete the zero-byte
#       leftovers, FRESH install from the repos (all pacman output
#       captured into the log this time), verify real files landed
#   [4] rebuild the compositor and verify libseat is finally linked
#   [5] two live test windows (move the mouse!) — only if linked
#   [6] if the install still failed: the guaranteed input-group
#       fallback (one gpasswd + one relogin — no package needed)
#
# Privileged steps are printed first and asked y/N; pacman runs with
# --noconfirm so its own prompts cannot hang the script. This script
# never chmods devices and never runs the compositor as root.
#
# Usage (TTY login, normal user, repo root):
#     ./scripts/xw-seat-repair.sh
#
# Output: xw-seat-repair-<timestamp>.log in the repo root.

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 1

TS=$(date +%Y%m%d-%H%M%S)
LOG="$ROOT/xw-seat-repair-$TS.log"
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
# privileged command WITH its output captured into the log
sudo_log() {
    say "\$ (sudo) $*"
    sudo "$@" >> "$LOG" 2>&1
    _rc=$?
    say "   [exit $_rc]"
    return $_rc
}
disk_full() {   # disk_full <mountpoint> -> 0 if >= 99% used
    df -P "$1" 2>/dev/null | awk 'NR==2{gsub("%","",$5); if ($5+0>=99) exit 0; exit 1}'
}

trap 'printf "\n>>> interrupted — the log so far is still valid: %s\n" "$LOG" >&2; exit 130' INT TERM

banner "xw-seat-repair — $TS"
banner "[0] meta"
r date
r git rev-parse --short HEAD
r git status --short

# ---- [1] disk space (the likely original cause of the corruption) ----
banner "[1] disk space (a full partition corrupts package installs exactly like this)"
r df -h / /usr /var /run /tmp /home
r df -i /
r du -sh /var/cache/pacman/pkg
if disk_full / || disk_full /usr; then
    say "CHECK: STOP — the root (or /usr) partition is >= 99% full."
    say "A full disk is what produces zero-byte library files and an"
    say "empty pacman database entry. Free space FIRST, then re-run:"
    say "    sudo pacman -Sc          # clear the package cache"
    say "    du -xh --max-depth=1 / 2>/dev/null | sort -h | tail -20"
    say "  (find the big directories, clean them, then re-run this script)"
    banner "DONE (stopped: disk full)"
    say "log file: $LOG"
    exit 1
fi
say "CHECK: disk space looks OK — continuing with the repair."

# ---- [2] before-evidence --------------------------------------------------
banner "[2] the corruption, before the repair"
HAVE_PACMAN=; PC_OK=; LINKED=
command -v pacman >/dev/null 2>&1 && HAVE_PACMAN=y
command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libseat 2>/dev/null && PC_OK=y
ldd build/bin/xw-compositor 2>/dev/null | grep -q libseat && LINKED=y
r pacman -Qi seatd
rp "pacman -Ql seatd | wc -l"
r ls -l /usr/bin/seatd /usr/lib/libseat.so /usr/lib/libseat.so.1 /usr/lib/pkgconfig/libseat.pc /usr/include/seat.h
rp "wc -c /usr/bin/seatd 2>/dev/null"
r cat build/.features
[ -n "$LINKED" ] && say "CHECK: xw-compositor IS linked with libseat" \
                   || say "CHECK: xw-compositor is NOT linked with libseat"
[ -n "$PC_OK" ]   && say "CHECK: pkg-config sees libseat dev files" \
                   || say "CHECK: pkg-config does NOT see libseat dev files"

# ---- [3] repair ------------------------------------------------------------
banner "[3] decision"
if [ -n "$LINKED" ]; then
    say "Route 0: xw-compositor already has libseat — nothing to repair."
    say "Run ./scripts/xw-tty-capture.sh to test, and send that log."
    say "log file: $LOG"
    exit 0
fi

if [ -z "$PC_OK" ] && [ -n "$HAVE_PACMAN" ]; then
    say "Route 2: repair the corrupted seatd package install."
    if ask "purge the broken seatd install and reinstall it fresh from the repos now?"; then
        # a) drop the corrupted DB entry (its file list is EMPTY, so
        #    pacman -R removes nothing from disk — exactly what we want)
        if ! sudo_log pacman -Rdd --noconfirm seatd; then
            say "(pacman -Rdd could not process the corrupted entry —"
            say " removing the database directory directly instead)"
            sudo_log rm -rf /var/lib/pacman/local/seatd-*
        fi
        # b) delete the zero-byte leftovers by hand (they are unowned
        #    now; leaving them would make the fresh install conflict)
        sudo_log rm -f /usr/bin/seatd /usr/bin/seatd-launch \
                        /usr/lib/libseat.so /usr/lib/libseat.so.1 \
                        /usr/lib/pkgconfig/libseat.pc /usr/include/seat.h
        sudo_log rm -rf /etc/runit/sv/seatd
        # c) fresh install (the service symlink in /run/runit/service
        #    stays and picks the new binary up on its own)
        if ! sudo_log pacman -S --noconfirm seatd; then
            say "(plain install failed — one retry: refresh the sync"
            say " databases and overwrite any straggling paths)"
            sudo_log pacman -Syy --noconfirm --overwrite 'usr/*' --overwrite 'etc/runit/*' seatd
        fi
        # d) verify: REAL files this time
        r pacman -Qi seatd
        rp "pacman -Ql seatd | wc -l"
        r ls -l /usr/bin/seatd /usr/lib/libseat.so /usr/lib/libseat.so.1 /usr/lib/pkgconfig/libseat.pc /usr/include/seat.h
        if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libseat 2>/dev/null; then
            PC_OK=y
            r pkg-config --modversion libseat
            say "CHECK: SUCCESS — real libseat development files are installed."
        else
            say "CHECK: the reinstall STILL did not produce libseat dev files —"
            say "the captured pacman output above (error text included) says why."
            say "Likely disk space or keyring/mirror trouble; read that output."
        fi
    else
        say "(repair declined — the input-group fallback at the end still applies)"
    fi
elif [ -z "$PC_OK" ]; then
    say "Route 3: no pacman here — manual repair instructions only:"
    say "    sudo pacman -Syy seatd     # refresh + fresh install"
    say "    ls -l /usr/lib/pkgconfig/libseat.pc   # must be non-zero"
    say "then re-run this script to rebuild and test."
    say "The input-group fallback at the end needs no package at all."
fi

# ---- [4] rebuild ------------------------------------------------------------
REBUILT=
if [ -n "$PC_OK" ]; then
    banner "[4] rebuild"
    say "\$ make clean && make   (a minute or two; output follows)"
    ( make clean 2>&1 && make 2>&1; echo "MAKE_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
    r cat build/.features
    rp "ldd build/bin/xw-compositor | grep -n libseat"
    if ldd build/bin/xw-compositor 2>/dev/null | grep -q libseat; then
        REBUILT=y
        say "CHECK: SUCCESS — xw-compositor is now linked with libseat."
        say "The elogind provider will engage right away; your login"
        say "session is already active — NO log-out needed."
    else
        say "CHECK: rebuild finished but libseat is STILL not linked —"
        say "the make output above (warning box included) explains why."
    fi
fi

# ---- [5] live test windows (only if linked) ---------------------------------
if [ -n "$REBUILT" ]; then
    LIVE=1
    [ "$(id -u)" -eq 0 ] && { say "(running as root — live windows skipped)"; LIVE=0; }
    [ -e /dev/dri ]      || { say "(/dev/dri missing — live windows skipped)"; LIVE=0; }
    command -v timeout >/dev/null 2>&1 || { say "(timeout missing — live windows skipped)"; LIVE=0; }
    if [ "$LIVE" -eq 1 ]; then
        banner "[5] live test 1: bare compositor, 12s — MOVE THE MOUSE NOW"
        say "\$ timeout 12 build/bin/xw-compositor -B drm -v"
        ( timeout 12 build/bin/xw-compositor -B drm -v 2>&1; echo "WINDOW1_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
        sleep 2
        banner "[5] live test 2: full session, 12s — MOVE THE MOUSE NOW"
        say "\$ timeout 12 build/bin/xw-session --backend=drm --verbose"
        ( timeout 12 build/bin/xw-session --backend=drm --verbose 2>&1; echo "WINDOW2_EXIT_CODE=$?" ) | tee -a "$LOG" >&2
        say "(exit code 124 = window ended by timeout — that is normal)"
    fi
    say ""
    say "Also checking the seatd daemon (informational):"
    _i=0
    while [ $_i -lt 10 ]; do [ -S /run/seatd.sock ] && break; sleep 1; _i=$((_i+1)); done
    r ls -l /run/seatd.sock
    [ -S /run/seatd.sock ] && say "CHECK: seatd daemon is up (socket exists)" \
                            || say "(socket still absent — fine: the elogind/libseat path does not need the daemon)"
fi

# ---- [6] fallback ------------------------------------------------------------
if [ -z "$REBUILT" ]; then
    banner "[6] fallback — works with NO package at all"
    say "Your compositor's direct provider can open the input devices"
    say "with your own group membership — one command + one relogin:"
    if ask "add $USER to the 'input' group now (sudo gpasswd)?"; then
        sudo_log gpasswd -a "$USER" input
    else
        say "(run it yourself: sudo gpasswd -a $USER input)"
    fi
    say ""
    say "THEN: log out of the TTY and log back in, and run:"
    say "    ./scripts/xw-tty-capture.sh    (move the mouse in both windows)"
    say "and send BOTH log files back. Note: the 'seat' group added in"
    say "the previous round also only takes effect after that relogin."
fi

# ---- [7] digest ---------------------------------------------------------------
banner "[7] digest (last interesting lines)"
rp "grep -nEi 'error|warn|fail|denied|refus|seat:|input|device|cursor|pointer|motion|CHECK:|EXIT_CODE' '$LOG' | tail -n 120"

banner "DONE"
say "log file: $LOG"
printf '\n============================================================\n' >&2
printf 'DONE. The deliverable is this file:\n  %s\n' "$LOG" >&2
printf '(if the mouse moved in the live windows, you are done — otherwise\n send this file back)\n' >&2
printf '============================================================\n\n' >&2
