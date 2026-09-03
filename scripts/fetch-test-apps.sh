#!/bin/bash
# fetch-test-apps.sh — download real client applications (foot, zenity,
# xterm, xeyes, Xwayland) plus their recursive runtime dependencies as
# .debs WITHOUT root, and extract them into a local prefix.
#
# The prefix mirrors the sysroot pattern: ./apps-root (gitignored),
# provides bin/ + lib/. Applications run with LD_LIBRARY_PATH pointed
# at the extracted libs. This gives the compositor REAL toolkit
# clients (GTK4 zenity, foot's custom toolkit, real X11 clients and a
# real Xwayland server) for integration testing in unprivileged
# containers.
#
# Usage: scripts/fetch-test-apps.sh [pkg ...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$ROOT/.apps-root"
CACHE="$PREFIX/debs"
mkdir -p "$PREFIX" "$CACHE"

PKGS="${*:-foot zenity xterm x11-apps xwayland}"

# 1. recursive dependency closure of the requested packages
DEPLIST="$(apt-cache depends --recurse --no-recommends --no-suggests \
    --no-conflicts --no-breaks --no-replaces --no-enhances \
    --no-pre-depends $PKGS 2>/dev/null |
    grep -E '^\w' | grep -Ev '^<|virtual|^$' | sort -u)"

# 2. what is already installed (those are satisfied by the system)
INSTALLED="$(dpkg-query -W -f '${binary:Package}\n' 2>/dev/null | sed 's/:amd64$//' | sort -u)"

# 3. download the missing ones
MISSING="$(comm -23 <(printf '%s\n' "$DEPLIST" | sed 's/:amd64$//') <(printf '%s\n' "$INSTALLED"))"
echo "packages to fetch: $(printf '%s' "$MISSING" | grep -c . || true)"

cd "$CACHE" || exit 1
for p in $MISSING; do
    # skip packages that cannot be downloaded (virtual or local)
    apt-get download "$p" 2>/dev/null || echo "  (skip $p: not downloadable)"
done

# 4. extract every downloaded deb into the prefix
cd "$ROOT" || exit 1
N=0
for deb in "$CACHE"/*.deb; do
    [ -e "$deb" ] || continue
    dpkg-deb -x "$deb" "$PREFIX" || echo "  (extract failed: $deb)"
    N=$((N + 1))
done
echo "extracted $N debs into $PREFIX"

# 5. summary of what we care about
for b in foot footclient zenity xterm xeyes Xwayland; do
    if [ -e "$PREFIX/usr/bin/$b" ]; then
        echo "ok: $PREFIX/usr/bin/$b"
    fi
done
