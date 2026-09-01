#!/bin/sh
# test-link-deps.sh — link-line dependency audit (static, no execution).
#
# Catches the "undefined reference / DSO missing from command line"
# failure class before it reaches users on other distributions:
#
#   every symbol referenced (undefined) by the objects and archive
#   members on a final link command must be provided by a library or
#   object on that SAME command line — or by the C runtime the
#   compiler driver always links — or by a sanitizer runtime.
#
# Background: xw-input-libinput.o calls udev_new() directly; if the
# final link carries only -linput (whose indirect DT_NEEDED is
# libudev.so.1), modern ld (--no-copy-dt-needed-entries) refuses with
# "DSO missing from command line" — exactly the failure reported on
# Arch/Artix where libinput.pc is upstream-shaped. This audit detects
# the class for EVERY final executable, not just the one that broke.
#
# Method: for each final target, the exact link command is taken from
# `make -n` (never executed, tree untouched), parsed into objects,
# archives, -l flags and -L dirs; undefined symbols of the inputs are
# compared against the union of defined symbols from all inputs, all
# resolved -l libraries and the C runtime libraries.
#
# The check is deliberately strict (it audits ALL archive members,
# including ones the linker might not pull): a hit here means either a
# genuinely missing -l flag or dead code that should not be in the
# archive — both worth fixing, neither silently linkable on some
# other distro.
#
# Run after `make all` (standalone or via make check /
# scripts/test-build-regressions.sh R6).

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

TMP="$(mktemp -d /tmp/xw-linkdeps.XXXXXX)"
chmod 700 "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

pass=0
fail=0

if [ ! -f build/lib/libxw.a ]; then
    echo "test-link-deps: no build tree (run make first)" >&2
    exit 1
fi

# final executables produced by the Makefile's link rules (only the
# ones this tree actually built — optional bins self-select here the
# same way CLIENT_BINS/SESSION_BINS do in the Makefile)
TARGETS=""
for t in build/bin/xw-compositor build/bin/xw-session \
build/bin/xw-session-ctl build/bin/xw-panel build/bin/xw-exit \
build/bin/xw-demo build/tests/run-tests build/tests/x11probe; do
    [ -e "$t" ] && TARGETS="$TARGETS $t"
done

# C runtime the compiler driver links implicitly. Audited symbols may
# resolve there; if one of these files is missing on this machine it
# simply contributes nothing (strictness never drops below libc).
SYSROOT_LIB=""
[ -n "${XW_SYSROOT:-}" ] && [ -d "$XW_SYSROOT/usr/lib/x86_64-linux-gnu" ] \
    && SYSROOT_LIB="$XW_SYSROOT/usr/lib/x86_64-linux-gnu"
DEFAULT_DIRS="/usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib /lib"
[ -n "$SYSROOT_LIB" ] && DEFAULT_DIRS="$SYSROOT_LIB $DEFAULT_DIRS"
RUNTIME_LIBS=""
for l in libc.so.6 libm.so.6 libgcc_s.so.1 libpthread.so.0 libdl.so.2 \
         librt.so.1 ld-linux-x86-64.so.2; do
    for d in $DEFAULT_DIRS; do
        if [ -e "$d/$l" ]; then RUNTIME_LIBS="$RUNTIME_LIBS $d/$l"; break; fi
    done
done

# symbols the linker itself provides / sanitizer runtimes (asan
# profile) — allowed without a library on the line
ALLOW_RE='^(__asan_|__ubsan_|__sanitizer_|__tsan_|__lsan_)|^(__bss_start|_edata|_end|_init|_fini|_start|__data_start)$'

# nm output -> "name" for defined symbols that can satisfy references
# (global/weak/indirect). NOTE: plain nm is required for .o/.a inputs:
# `nm -D` succeeds with EMPTY output on relocatable objects (exit 0),
# so an `||` fallback never fires. glibc ifunc symbols print as
# lowercase 'i' (strcmp, memcpy, ...) — they resolve at load time and
# must count as provided.
collect_defined() { # file... -> appends to $TMP/defined
    for f in "$@"; do
        case "$f" in
        *.o|*.a)
            nm --defined-only "$f" 2>/dev/null ;;
        *)
            out=$(nm -D --defined-only "$f" 2>/dev/null)
            if [ -n "$out" ]; then printf '%s\n' "$out"
            else nm --defined-only "$f" 2>/dev/null; fi ;;
        esac | awk '
            NF >= 2 && $1 !~ /:$/ {
                type = (NF == 3) ? $2 : $1
                name = (NF == 3) ? $3 : $2
                sub(/@.*/, "", name)
                if ((type ~ /^[A-Z]$/ && type != "N" && type != "A" && type != "U") || type == "i")
                    print name
            }'
    done >> "$TMP/defined"
}

collect_undef() { # file... -> appends to $TMP/undef
    for f in "$@"; do
        nm "$f" 2>/dev/null | awk '
            NF == 2 && $1 == "U" { sub(/@.*/, "", $2); print $2 }'
    done >> "$TMP/undef"
}

resolve_lib() { # -l name -> path on stdout or nothing
    name="$1"
    for d in $LDIRS $DEFAULT_DIRS; do
        if [ -e "$d/lib$name.so" ]; then echo "$d/lib$name.so"; return 0; fi
        if [ -e "$d/lib$name.a" ];  then echo "$d/lib$name.a";  return 0; fi
    done
    for d in $LDIRS $DEFAULT_DIRS; do
        for f in "$d/lib$name.so."*; do
            if [ -e "$f" ]; then echo "$f"; return 0; fi
        done
    done
    return 1
}

total_targets=0
for T in $TARGETS; do
    total_targets=$((total_targets + 1))
    : > "$TMP/defined"
    : > "$TMP/undef"

    # exact link command (dry run; the tree is never touched)
    cmd=$(make -n --always-make "$T" 2>/dev/null | grep -F -- " -o $T " | head -1)
    if [ -z "$cmd" ]; then
        echo "FAIL link-deps: $T: could not retrieve the link command"
        fail=$((fail + 1))
        continue
    fi

    objs=$(printf '%s\n' "$cmd" | tr ' ' '\n' | grep '\.o$' | tr '\n' ' ')
    arcs=$(printf '%s\n' "$cmd" | tr ' ' '\n' | grep '\.a$' | tr '\n' ' ')
    lnames=$(printf '%s\n' "$cmd" | tr ' ' '\n' | sed -n 's/^-l//p' | tr '\n' ' ')
    LDIRS=$(printf '%s\n' "$cmd" | tr ' ' '\n' | sed -n 's/^-L//p' | tr '\n' ' ')

    # 1. symbols provided by the inputs themselves
    for f in $objs; do collect_defined "$f"; done
    for f in $arcs; do collect_defined "$f"; done

    # 2. symbols required by the inputs
    for f in $objs; do collect_undef "$f"; done
    for f in $arcs; do collect_undef "$f"; done

    # 3. symbols provided by every -l library on the line
    missing_libs=""
    for n in $lnames; do
        lib=$(resolve_lib "$n")
        if [ -n "$lib" ]; then
            collect_defined "$lib"
        else
            missing_libs="$missing_libs -l$n"
        fi
    done

    # 4. C runtime
    for l in $RUNTIME_LIBS; do collect_defined "$l"; done

    sort -u "$TMP/defined" > "$TMP/defined.s"
    sort -u "$TMP/undef"  > "$TMP/undef.s"
    comm -23 "$TMP/undef.s" "$TMP/defined.s" > "$TMP/missing"
    grep -Ev "$ALLOW_RE" "$TMP/missing" > "$TMP/missing2" || true
    nmiss=$(grep -c . "$TMP/missing2" 2>/dev/null)
    nmiss=${nmiss:-0}

    if [ -n "$missing_libs" ]; then
        echo "FAIL link-deps: $T: libraries on the link line not found:$missing_libs"
        fail=$((fail + 1))
    elif [ "$nmiss" -gt 0 ]; then
        echo "FAIL link-deps: $T: $nmiss symbol(s) not provided by anything on the link line:"
        sed 's/^/       /' "$TMP/missing2" | head -15
        echo "       -> add the missing -l flag (pkg-config) to $T's link rule in the Makefile"
        fail=$((fail + 1))
    else
        echo "ok   link-deps: $T ($(grep -c . "$TMP/undef.s") undefined symbols covered)"
        pass=$((pass + 1))
    fi
done

echo
echo "test-link-deps: $pass passed, $fail failed ($total_targets targets)"
[ "$fail" -eq 0 ] || exit 1
exit 0
