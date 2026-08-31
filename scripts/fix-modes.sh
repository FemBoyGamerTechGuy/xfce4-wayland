#!/bin/bash
# fix-modes.sh — restore working-tree file modes to match HEAD.
#
# The container snapshot machinery periodically flips every tracked file to
# mode 100755.  Content is untouched, but `git status` fills with noise and
# milestone commits would swallow the pollution.  This script walks every
# tracked file and chmods it back to the mode recorded in HEAD.  Untracked
# files are left alone.  Run it before committing.
#
# Usage: bash scripts/fix-modes.sh   (from anywhere; paths resolved via git)

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

fixed=0
while read -r mode type hash path; do
    [ "$type" = "blob" ] || continue
    want=${mode: -3}          # git modes are 100644/100755/120000
    [ "$want" = "000" ] && continue   # symlink: chmod would dereference
    have=$(stat -c %a "$path" 2>/dev/null || continue)
    if [ "$have" != "$((8#$want))" ]; then
        chmod "$want" "$path"
        fixed=$((fixed + 1))
    fi
done < <(git ls-tree -r HEAD)

echo "fix-modes: normalized $fixed file mode(s) to HEAD"
