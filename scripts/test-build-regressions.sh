#!/bin/sh
# test-build-regressions.sh — regression suite for the build system and
# the dev-session entry point. Covers the "clean distro build" failure
# class reported on Artix/XLibre (zsh default shell, Arch-family font
# layout):
#
#   R1  font generation: bundled asset, determinism, structure,
#       precise diagnostics when the asset is missing
#   R2  build failure handling: a failing compile must fail `make`
#       immediately; the documented quick-start flow must not
#       "continue" past a failed or PARTIAL build
#   R3  clean build with ALL system fonts hidden (mount namespace):
#       the build must succeed using only the bundled font, and the
#       resulting session must actually run
#   R4  dev-session.sh behavior when compilation failed: refuse to
#       launch, clear diagnostic, non-zero exit, no stray processes
#   R5  zsh execution: syntax, sourcing and full runs of the shell
#       entry points under zsh (plus dash/bash syntax checks)
#   R6  linker dependency propagation: upstream-shaped libinput.pc
#       (no -ludev handed out) must still link; XW_LIBINPUT=1 fails
#       clearly without libudev dev files; auto degrades with a
#       notice; =0 builds without the backend; feature switching
#       refuses stale trees; symbol-coverage audit of every final
#       link command
#
# Skips are explicit and honest (with the reason); a skip never passes
# a check silently. Run via `make check` or directly.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Optional local sysroot (runtime lib paths + pkg-config) — no-op on
# normal distributions, required in locked-down containers so that
# copied trees can build.
. "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true

TMP="$(mktemp -d /tmp/xw-buildregress.XXXXXX)"
chmod 700 "$TMP"

pass=0
fail=0
skip=0

cleanup() {
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

check() { # name, command
    if eval "$2"; then
        pass=$((pass + 1))
        echo "ok   $1"
    else
        fail=$((fail + 1))
        echo "FAIL $1"
    fi
}

note() { # informational line inside a section
    echo "     $1"
}

copy_repo() { # dest — working tree without build artifacts
    mkdir -p "$1"
    tar -C "$ROOT" \
        --exclude=build --exclude=dist --exclude=.git \
        --exclude=.toolchain --exclude=config.local.mk \
        -cf - . | tar -C "$1" -xf -
}

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python

# =========================================================== R1: genfont
echo "== R1: font generation (bundled asset) =="

"$PY" tools/genfont.py -o "$TMP/f1.h" >"$TMP/genfont1.log" 2>&1
rc=$?
check "R1: genfont succeeds from a clean checkout tree (rc=$rc)" \
    '[ "$rc" -eq 0 ]'

# run again under a stripped environment and a fixed locale
env -i PATH="$PATH" LC_ALL=C "$PY" "$ROOT/tools/genfont.py" -o "$TMP/f2.h" \
    >"$TMP/genfont2.log" 2>&1
rc=$?
check "R1: genfont works with a stripped environment (no env deps)" \
    '[ "$rc" -eq 0 ]'
cmp -s "$TMP/f1.h" "$TMP/f2.h"
rc=$?
check "R1: output is deterministic (two runs identical)" '[ "$rc" -eq 0 ]'

nglyphs=$(grep -c '^static const uint8_t xw_bits_' "$TMP/f1.h" 2>/dev/null || echo 0)
check "R1: header defines all 95 ASCII glyphs (found $nglyphs)" \
    '[ "$nglyphs" -eq 95 ]'
grep -q '^#define XW_FONT_LINE_H' "$TMP/f1.h"
rc=$?
check "R1: header carries font metrics" '[ "$rc" -eq 0 ]'
grep -q 'DejaVuSans-ascii.ttf (bundled asset' "$TMP/f1.h"
rc=$?
check "R1: header records the bundled font provenance" '[ "$rc" -eq 0 ]'

# missing asset -> immediate, precise diagnostic
mkdir -p "$TMP/noasset/tools" "$TMP/noasset/build/gen"
cp tools/genfont.py "$TMP/noasset/tools/"
"$PY" "$TMP/noasset/tools/genfont.py" -o "$TMP/noasset/out.h" \
    >"$TMP/noasset.log" 2>&1
rc=$?
check "R1: missing bundled asset fails (rc=$rc)" '[ "$rc" -ne 0 ]'
grep -q "bundled font asset is missing" "$TMP/noasset.log"
rc=$?
check "R1: missing asset diagnostic names the expected path" \
    '[ "$rc" -eq 0 ]'

# explicit --font to a nonexistent file
"$PY" tools/genfont.py -o "$TMP/f3.h" --font "$TMP/nonexistent.ttf" \
    >"$TMP/genfont3.log" 2>&1
rc=$?
check "R1: --font nonexistent fails cleanly (rc=$rc)" '[ "$rc" -ne 0 ]'
grep -q "nonexistent.ttf" "$TMP/genfont3.log"
rc=$?
check "R1: --font error names the requested file" '[ "$rc" -eq 0 ]'

# ============================================== R2: build failure handling
echo
echo "== R2: build failure handling =="

# Late-stage compile failure: everything up to the session binaries is
# built, xw-panel fails — the exact "partial build" trap where the old
# dev-session.sh launched binaries that existed while others did not.
BROKEN="$TMP/broken"
copy_repo "$BROKEN"
printf '\n#error deliberate regression-test compile failure\n' \
    >> "$BROKEN/src/clients/xw-panel.c"

( cd "$BROKEN" && make -s all ) >"$TMP/broken-make.log" 2>&1
rc=$?
check "R2: broken source fails the build immediately (rc=$rc)" \
    '[ "$rc" -ne 0 ]'
grep -q "xw-panel.c" "$TMP/broken-make.log"
rc=$?
check "R2: failing file named in the error output" '[ "$rc" -eq 0 ]'
check "R2: the failed target produced no binary" \
    '[ ! -e "$BROKEN/build/bin/xw-panel" ]'

# The documented quick-start flow after a failed build: dev-session.sh
# must refuse to launch anything — even though a PARTIAL build left
# some binaries behind (the reported Artix behavior: it "incorrectly
# continues and attempts to execute ./build/bin/xw-session").
( cd "$BROKEN" && ./scripts/dev-session.sh --logout ) \
    >"$TMP/broken-devsess.log" 2>&1
rc=$?
check "R2: quick-start after partial failed build fails fast (rc=$rc)" \
    '[ "$rc" -ne 0 ]'
grep -q "xw-panel does not exist or is not executable" \
    "$TMP/broken-devsess.log"
rc=$?
check "R2: refusal names the missing binary" '[ "$rc" -eq 0 ]'
grep -q "starting session" "$TMP/broken-devsess.log"
if [ "$?" -ne 0 ]; then
    pass=$((pass + 1)); echo "ok   R2: no session launch was attempted"
else
    fail=$((fail + 1)); echo "FAIL R2: no session launch was attempted"
fi
check "R2: no leftover processes from the broken tree" \
    '! pgrep -f "$BROKEN/build/bin" >/dev/null 2>&1'

# ================================== R3: clean build, system fonts hidden
echo
echo "== R3: clean build with no system fonts (the Artix regression) =="

if unshare -rm true >/dev/null 2>&1; then
    FONTSLESS="$TMP/fontless"
    copy_repo "$FONTSLESS"
    mkdir -p "$TMP/fontless-empty"
    export XW_REGR_COPY="$FONTSLESS"
    export XW_REGR_EMPTY="$TMP/fontless-empty"
    export XW_REGR_COUNT="$TMP/fontcount"
    unshare -rm sh -c '
        # hide every system font directory (the bind mounts exist only
        # inside this namespace)
        for d in /usr/share/fonts /usr/local/share/fonts; do
            if [ -d "$d" ]; then
                mount --bind "$XW_REGR_EMPTY" "$d" 2>/dev/null || exit 91
            fi
        done
        ls -A /usr/share/fonts 2>/dev/null | wc -l > "$XW_REGR_COUNT"
        cd "$XW_REGR_COPY" || exit 92
        make -s all && ./scripts/dev-session.sh --logout
    ' >"$TMP/fontless.log" 2>&1
    rc=$?
    hidden="$(cat "$TMP/fontcount" 2>/dev/null || echo '?')"
    check "R3: build + session succeed with fonts hidden ($hidden files left visible)" \
        '[ "$rc" -eq 0 ]'
    grep -q "genfont: 95 glyphs" "$TMP/fontless.log"
    rc=$?
    check "R3: font rasterized from the bundled asset" '[ "$rc" -eq 0 ]'
    grep -q "session exited (rc=0)" "$TMP/fontless.log"
    rc=$?
    check "R3: the built session actually ran and logged out cleanly" \
        '[ "$rc" -eq 0 ]'
    check "R3: compositor binary was produced" \
        '[ -x "$FONTSLESS/build/bin/xw-compositor" ]'
else
    skip=$((skip + 1))
    note "SKIP R3: unprivileged user namespaces unavailable on this kernel"
    note "     (the bundled-asset build is still covered by R1 + the normal build)"
fi

# =========================== R4: dev-session failure modes (fast checks)
echo
echo "== R4: dev-session fail-fast behavior =="

EMPTY="$TMP/empty"
copy_repo "$EMPTY"
( cd "$EMPTY" && ./scripts/dev-session.sh --logout ) \
    >"$TMP/empty-devsess.log" 2>&1
rc=$?
check "R4: unbuilt tree -> refuse to run (rc=$rc)" '[ "$rc" -ne 0 ]'
grep -q "does not exist or is not executable" "$TMP/empty-devsess.log"
rc=$?
check "R4: refusal message present" '[ "$rc" -eq 0 ]'
grep -q "starting session" "$TMP/empty-devsess.log"
if [ "$?" -ne 0 ]; then
    pass=$((pass + 1)); echo "ok   R4: nothing was launched"
else
    fail=$((fail + 1)); echo "FAIL R4: nothing was launched"
fi

# early-crashing session manager: stub tree where xw-session exits 1
STUB="$TMP/stub"
copy_repo "$STUB"
mkdir -p "$STUB/build/bin"
for b in xw-compositor xw-session xw-session-ctl xw-exit xw-panel; do
    printf '#!/bin/sh\nexit 0\n' > "$STUB/build/bin/$b"
    chmod +x "$STUB/build/bin/$b"
done
printf '#!/bin/sh\necho "stub session manager: deliberate crash" >&2\nexit 1\n' \
    > "$STUB/build/bin/xw-session"
chmod +x "$STUB/build/bin/xw-session"
( cd "$STUB" && timeout 30 ./scripts/dev-session.sh --logout ) \
    >"$TMP/stub-devsess.log" 2>&1
rc=$?
check "R4: crashing session manager fails the script (rc=$rc)" \
    '[ "$rc" -ne 0 ]'
grep -q "exited early" "$TMP/stub-devsess.log"
rc=$?
check "R4: crash reported with foreground-debug hint" '[ "$rc" -eq 0 ]'
check "R4: no stub session processes leaked" \
    '! pgrep -f "$STUB/build/bin" >/dev/null 2>&1'

# ========================================================= R5: zsh / shells
echo
echo "== R5: shell compatibility (zsh / bash / dash) =="

ZSH=""
if command -v zsh >/dev/null 2>&1; then
    ZSH="$(command -v zsh)"
elif [ -x "$ROOT/.toolchain/zsh-root/usr/bin/zsh" ]; then
    # dev-container convenience: rootless-extracted zsh under .toolchain
    ZSH="$ROOT/.toolchain/zsh-root/usr/bin/zsh"
else
    skip=$((skip + 1))
    note "SKIP R5-zsh: zsh is not installed here (installing zsh keeps"
    note "     the zsh-execution checks active on dev machines and CI)"
fi

# literal list: zsh does not word-split unquoted expansions
ZSH_NOISE='number expected|unknown condition|no matches found|parse error|bad math'

for sh in sh bash; do
    if command -v "$sh" >/dev/null 2>&1; then
        bad=0
        for s in scripts/env.sh scripts/dev-session.sh \
                 scripts/test-session.sh scripts/run-asan.sh \
                 scripts/bootstrap-sysroot.sh \
                 scripts/test-build-regressions.sh \
                 scripts/test-link-deps.sh; do
            "$sh" -n "$ROOT/$s" >/dev/null 2>&1 || bad=$((bad + 1))
        done
        check "R5: $sh -n syntax of all entry scripts ($bad failures)" \
            '[ "$bad" -eq 0 ]'
    fi
done

if [ -n "$ZSH" ]; then
    bad=0
    for s in scripts/env.sh scripts/dev-session.sh \
             scripts/test-session.sh scripts/run-asan.sh \
             scripts/bootstrap-sysroot.sh \
             scripts/test-build-regressions.sh \
             scripts/test-link-deps.sh; do
        "$ZSH" -n "$ROOT/$s" >/dev/null 2>&1 || bad=$((bad + 1))
    done
    check "R5: zsh -n syntax of all entry scripts ($bad failures)" \
        '[ "$bad" -eq 0 ]'

    # sourcing env.sh under zsh (the first quick-start line)
    "$ZSH" -f -c ". \"$ROOT/scripts/env.sh\"; [ -n \"\$XW_ROOT\" ]" \
        >"$TMP/zsh-env.log" 2>&1
    rc=$?
    check "R5: env.sh sources under zsh, XW_ROOT resolves (rc=$rc)" \
        '[ "$rc" -eq 0 ]'
    check "R5: no zsh noise while sourcing env.sh" \
        "! grep -qE \"$ZSH_NOISE\" \"\$TMP/zsh-env.log\""

    # dev-session.sh under zsh on an unbuilt tree -> clean refusal
    ( cd "$EMPTY" && "$ZSH" -f scripts/dev-session.sh --logout ) \
        >"$TMP/zsh-devsess-empty.log" 2>&1
    rc=$?
    check "R5: zsh dev-session on unbuilt tree refuses (rc=$rc)" \
        '[ "$rc" -ne 0 ]'
    grep -q "does not exist or is not executable" \
        "$TMP/zsh-devsess-empty.log"
    rc=$?
    check "R5: zsh refusal diagnostic present" '[ "$rc" -eq 0 ]'
    check "R5: no zsh parser noise in the refusal" \
        "! grep -qE \"$ZSH_NOISE\" \"\$TMP/zsh-devsess-empty.log\""

    # full session run under zsh (only meaningful with a real build)
    if [ -x "$ROOT/build/bin/xw-session" ]; then
        ( cd "$ROOT" && "$ZSH" -f scripts/dev-session.sh --logout ) \
            >"$TMP/zsh-devsess-full.log" 2>&1
        rc=$?
        check "R5: full dev session runs under zsh (rc=$rc)" \
            '[ "$rc" -eq 0 ]'
        grep -q "session exited (rc=0)" "$TMP/zsh-devsess-full.log"
        rc=$?
        check "R5: zsh session logged out cleanly" '[ "$rc" -eq 0 ]'
        check "R5: no zsh parser noise in the full run" \
            "! grep -qE \"$ZSH_NOISE\" \"\$TMP/zsh-devsess-full.log\""
    else
        skip=$((skip + 1))
        note "SKIP R5-full: build/bin/xw-session not present (run make first)"
    fi
fi

# ========================================= R6: linker dependency propagation
echo
echo "== R6: linker dependency propagation (libudev / -l audit) =="

# The reported Artix failure: xw-input-libinput.o references udev_new
# directly; upstream libinput.pc does not hand out -ludev (only the
# Debian-family sysroot pc used here does, via Requires:). These checks
# exercise the upstream shape explicitly so the sandbox cannot mask it.
if [ -f "$ROOT/build/lib/libxw.a" ] && [ -e "$ROOT/build/bin/xw-compositor" ]; then
    HSYS="$TMP/hsys"
    PC="$HSYS/usr/lib/x86_64-linux-gnu/pkgconfig"
    mkdir -p "$PC"

    # scanner/protocols locations are reused through symlinks when a
    # local sysroot is the provider (on normal distros the system
    # locations serve the fallbacks in the Makefile)
    if [ -n "${XW_SYSROOT:-}" ] && [ -d "$XW_SYSROOT/usr/bin" ]; then
        ln -s "$XW_SYSROOT/usr/bin" "$HSYS/usr/bin"
    fi
    if [ -n "${XW_SYSROOT:-}" ] && [ -d "$XW_SYSROOT/usr/share" ]; then
        ln -s "$XW_SYSROOT/usr/share" "$HSYS/usr/share"
    fi

    # full .pc closure: every directory participating in the current
    # search (local sysroot, PKG_CONFIG_PATH, built-in defaults)
    # contributes ALL of its .pc files except libinput.pc/libudev.pc —
    # the hostile upstream-shaped versions are written below. The
    # closure keeps transitive Requires resolvable (wayland-server ->
    # libffi, x11 -> xcb, ...) while PKG_CONFIG_LIBDIR makes the
    # hostile directory the only search root, so the test cannot be
    # unmasked by system .pc files on real distributions.
    PC_DIRS="$(printf '%s' "${PKG_CONFIG_PATH:-}" | tr ':' ' ') \
/usr/lib/x86_64-linux-gnu/pkgconfig /usr/lib64/pkgconfig \
/usr/share/pkgconfig /usr/lib/pkgconfig"
    if [ -n "${XW_SYSROOT:-}" ] && [ -d "$XW_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig" ]; then
        PC_DIRS="$XW_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig $PC_DIRS"
    fi
    for pd in $PC_DIRS; do
        [ -d "$pd" ] || continue
        for pcf in "$pd"/*.pc; do
            [ -f "$pcf" ] || continue
            case "${pcf##*/}" in
            libinput.pc | libudev.pc) continue ;;
            esac
            cp "$pcf" "$PC/"
        done
    done

    # upstream-shaped libinput.pc (Requires.private, no -ludev out)
    LI_LIBDIR=$(pkg-config --variable=libdir libinput 2>/dev/null || echo /usr/lib)
    LI_INC=$(pkg-config --variable=includedir libinput 2>/dev/null || echo /usr/include)
    LI_VER=$(pkg-config --modversion libinput 2>/dev/null || echo 0)
    LU_LIBDIR=$(pkg-config --variable=libdir libudev 2>/dev/null || echo /usr/lib)
    LU_INC=$(pkg-config --variable=includedir libudev 2>/dev/null || echo /usr/include)
    LU_VER=$(pkg-config --modversion libudev 2>/dev/null || echo 0)
    cat > "$PC/libinput.pc" <<EOF
libdir=$LI_LIBDIR
includedir=$LI_INC

Name: libinput
Description: Input abstraction library (upstream .pc shape)
Version: $LI_VER
Libs: -L\${libdir} -linput
Cflags: -I\${includedir}
Requires.private: libudev
EOF
    cat > "$PC/libudev.pc" <<EOF
libdir=$LU_LIBDIR
includedir=$LU_INC

Name: libudev
Description: udev library (upstream .pc shape)
Version: $LU_VER
Libs: -L\${libdir} -ludev
Cflags: -I\${includedir}
EOF

    # PKG_CONFIG_LIBDIR replaces the built-in default search path, so
    # the hostile directory is the ONLY source on every machine (the
    # system libudev.pc of a real distro must not leak in).
    hlibs=$(env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$PC" \
        pkg-config --libs libinput 2>/dev/null || true)
    check "R6: hostile libinput.pc hands out no -ludev (test self-check)" \
        'case "$hlibs" in *ludev*) false ;; *) true ;; esac'

    # --- R6a: the Artix repro — final link with upstream-shaped pcs
    # (the exe is removed FIRST so `make -n` actually prints the link
    # command instead of reporting the target up to date)
    rm -f "$ROOT/build/bin/xw-compositor"
    line=$( (cd "$ROOT" && env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$PC" \
        make -n XW_LIBINPUT=1 XW_SYSROOT="$HSYS" \
        build/bin/xw-compositor 2>/dev/null) | \
        grep -F -- " -o build/bin/xw-compositor " | head -1)
    pos_arc=$(printf '%s\n' "$line" | tr ' ' '\n' | grep -n 'libxw\.a$' | head -1 | cut -d: -f1)
    pos_in=$(printf '%s\n' "$line" | tr ' ' '\n' | grep -n '^-linput$' | head -1 | cut -d: -f1)
    pos_ud=$(printf '%s\n' "$line" | tr ' ' '\n' | grep -n '^-ludev$' | head -1 | cut -d: -f1)
    pos_arc=${pos_arc:-0}; pos_in=${pos_in:-0}; pos_ud=${pos_ud:-0}
    check "R6: -ludev explicitly on the final link command" '[ "$pos_ud" -gt 0 ]'
    check "R6: -ludev ordered after libxw.a and -linput (static-safe)" \
        '[ "$pos_arc" -gt 0 ] && [ "$pos_in" -gt 0 ] && [ "$pos_ud" -gt "$pos_in" ]'

    rm -f "$ROOT/build/bin/xw-compositor"
    ( cd "$ROOT" && env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$PC" \
        make -s XW_LIBINPUT=1 XW_SYSROOT="$HSYS" \
        build/bin/xw-compositor ) >"$TMP/r6a.log" 2>&1
    rc=$?
    check "R6: upstream-pc final link succeeds (the Artix failure, rc=$rc)" \
        '[ "$rc" -eq 0 ]'
    check "R6: compositor binary produced under hostile pcs" \
        '[ -e "$ROOT/build/bin/xw-compositor" ]'

    # restore the canonical build of the binary
    rm -f "$ROOT/build/bin/xw-compositor"
    ( cd "$ROOT" && make -s build/bin/xw-compositor ) >/dev/null 2>&1

    # --- R6b: XW_LIBINPUT=1 with libudev dev files absent -> clear error
    HSYS_NOUDEV="$TMP/hsys-noudev"
    PC_NOUDEV="$HSYS_NOUDEV/usr/lib/x86_64-linux-gnu/pkgconfig"
    mkdir -p "$PC_NOUDEV"
    cp "$PC"/*.pc "$PC_NOUDEV/"
    rm -f "$PC_NOUDEV/libudev.pc"
    for sub in bin share; do
        if [ -L "$HSYS/usr/$sub" ]; then
            ln -s "$(readlink "$HSYS/usr/$sub")" "$HSYS_NOUDEV/usr/$sub"
        fi
    done
    ( cd "$ROOT" && env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$PC_NOUDEV" \
        make -s XW_LIBINPUT=1 XW_SYSROOT="$HSYS_NOUDEV" \
        build/bin/xw-compositor ) >"$TMP/r6b.log" 2>&1
    rc=$?
    check "R6: XW_LIBINPUT=1 without libudev fails (rc=$rc)" '[ "$rc" -ne 0 ]'
    grep -q "libudev" "$TMP/r6b.log"
    rc=$?
    check "R6: XW_LIBINPUT=1 failure names libudev" '[ "$rc" -eq 0 ]'

    # --- R6c: auto without libudev -> degrade with notice, config "no"
    ( cd "$ROOT" && env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$PC_NOUDEV" \
        make -s XW_SYSROOT="$HSYS_NOUDEV" config ) >"$TMP/r6c.log" 2>&1
    rc=$?
    check "R6: auto without libudev: make config succeeds (rc=$rc)" \
        '[ "$rc" -eq 0 ]'
    grep -qE '^  libinput +no$' "$TMP/r6c.log"
    rc=$?
    check "R6: auto without libudev: config reports the backend off" \
        '[ "$rc" -eq 0 ]'
    grep -q "libudev" "$TMP/r6c.log"
    rc=$?
    check "R6: auto without libudev: the notice names libudev" \
        '[ "$rc" -eq 0 ]'

    # --- R6c2: auto with both -> enabled, versions reported
    ( cd "$ROOT" && env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR="$PC" \
        make -s XW_SYSROOT="$HSYS" config ) >"$TMP/r6c2.log" 2>&1
    grep -qE '^  libinput +yes \(libinput .+ \+ libudev .+\)$' "$TMP/r6c2.log"
    rc=$?
    check "R6: auto with both dev sets: config reports libinput + libudev" \
        '[ "$rc" -eq 0 ]'

    # --- R6d: XW_LIBINPUT=0 -> off
    ( cd "$ROOT" && make -s XW_LIBINPUT=0 config ) >"$TMP/r6d.log" 2>&1
    grep -qE '^  libinput +no$' "$TMP/r6d.log"
    rc=$?
    check "R6: XW_LIBINPUT=0: config reports the backend off" '[ "$rc" -eq 0 ]'

    # --- R6e: feature switching over a populated tree refuses (stale
    # archive members would otherwise reproduce the DSO failure class)
    ( cd "$ROOT" && make -s XW_LIBINPUT=0 \
        build/bin/xw-compositor ) >"$TMP/r6e.log" 2>&1
    rc=$?
    check "R6: switching libinput off on a built tree fails loudly (rc=$rc)" \
        '[ "$rc" -ne 0 ]'
    grep -q "make clean" "$TMP/r6e.log"
    rc=$?
    check "R6: the refusal points at make clean" '[ "$rc" -eq 0 ]'
else
    skip=$((skip + 1))
    note "SKIP R6: no build tree (run make first; make check builds it)"
fi

# --- R6f: XW_LIBINPUT=0 builds fully from a clean checkout even when
# the development files are present (=0 means never build the backend)
NOINPUT="$TMP/nolibinput"
copy_repo "$NOINPUT"
( cd "$NOINPUT" && make -s XW_LIBINPUT=0 all ) >"$TMP/r6f.log" 2>&1
rc=$?
check "R6: XW_LIBINPUT=0 clean build succeeds (rc=$rc)" '[ "$rc" -eq 0 ]'
check "R6: XW_LIBINPUT=0 compositor produced" \
    '[ -x "$NOINPUT/build/bin/xw-compositor" ]'
check "R6: XW_LIBINPUT=0 libxw.a excludes the backend member" \
    '! ar t "$NOINPUT/build/lib/libxw.a" 2>/dev/null | grep -q xw-input-libinput'
nm "$NOINPUT/build/bin/xw-compositor" >"$TMP/r6f.nm" 2>&1 || true
check "R6: XW_LIBINPUT=0 binary references no udev/libinput symbols" \
    '! grep -qE "U (udev_|libinput_)" "$TMP/r6f.nm"'

# --- R6g: symbol-coverage audit of every final link command
if [ -f "$ROOT/build/lib/libxw.a" ]; then
    sh "$ROOT/scripts/test-link-deps.sh" >"$TMP/r6g.log" 2>&1
    rc=$?
    check "R6: link-line symbol audit passes for all executables (rc=$rc)" \
        '[ "$rc" -eq 0 ]'
    grep -q "test-link-deps: .* 0 failed" "$TMP/r6g.log"
    rc=$?
    check "R6: audit summary reports zero failures" '[ "$rc" -eq 0 ]'
else
    skip=$((skip + 1))
    note "SKIP R6g: no build tree for the link audit"
fi

echo
echo "test-build-regressions: $pass passed, $fail failed, $skip skipped"
if [ "${XW_REGR_KEEP:-}" = "1" ]; then
    trap - EXIT INT TERM
    echo "test-build-regressions: TMP kept for inspection: $TMP"
fi
[ "$fail" -eq 0 ] || exit 1
exit 0
