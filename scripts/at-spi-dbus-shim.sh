#!/bin/bash
# at-spi-dbus-shim.sh — stand-in for /usr/bin/dbus-daemon inside the
# repro user namespace: at-spi-bus-launcher spawns dbus-daemon with a
# compile-time config path (/usr/share/defaults/at-spi2/... or
# /etc/at-spi2/...) that only exists in the .apps-root extraction, so
# this shim rewrites every --config-file to the apps-root copy and
# execs the real daemon. Bound over /usr/bin/dbus-daemon by
# scripts/repro-browser-crash.sh's a11y leg.
APPS="/home/z/my-project/.apps-root"
CONF="$APPS/usr/share/defaults/at-spi2/accessibility.conf"
new=()
for a in "$@"; do
    case "$a" in
        --config-file=*) new+=("--config-file=$CONF") ;;
        *) new+=("$a") ;;
    esac
done
exec "$APPS/usr/bin/dbus-daemon" "${new[@]}"
