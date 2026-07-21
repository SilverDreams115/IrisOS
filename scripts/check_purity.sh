#!/usr/bin/env bash
#
# check_purity.sh — executable guard for the seL4 purity charter.
#
# Freezes the legacy authority-by-handle consumers/producers and the kslab
# uses in PRODUCTIVE code (kernel/ + services/, excluding test-only code).
# The allowlist (scripts/purity_allowlist.txt) sets the maximum number of
# occurrences allowed per file for each frozen identifier; this gate FAILS if:
#
#   1. a file not in the allowlist contains a frozen identifier;
#   2. a file exceeds the frozen count for an identifier.
#
# Lowering a count is progress (the gate reports it; update the allowlist
# downward in the same change). RAISING it requires amending the charter and
# the ledger in the same commit, with a written technical justification
# (charter §3).
#
# Frozen identifiers:
#   handle_table_insert        — handle producers (incl. _badged/_derived)
#   handle_table_get_object    — handle consumers
#   cspace_or_handle_resolve_  — dual CPtr/handle resolution
#   kslab_alloc                — kernel objects born from the global heap
#
# Test-only (excluded from the scan): services/iris_test, services/lifecycle_probe,
# tests/. The charter forbids new PRODUCTIVE paths; the tests deliberately
# exercise the legacy semantics for as long as they exist.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

ALLOWLIST="scripts/purity_allowlist.txt"
PATTERNS=(handle_table_insert handle_table_get_object cspace_or_handle_resolve_ kslab_alloc)

if [ ! -f "$ALLOWLIST" ]; then
    echo "[purity] FAIL: allowlist $ALLOWLIST does not exist"
    exit 1
fi

# Productive files: kernel/ and services/ minus the test-only ones.
mapfile -t FILES < <(find kernel services \
    \( -path services/iris_test -o -path services/lifecycle_probe \) -prune -o \
    \( -name '*.c' -o -name '*.h' \) -print | sort)

fail=0
progress=0

count_in_file() {
    # grep -c counts lines; -o counts occurrences — we use -o | wc -l.
    # (grep exits 1 with no matches: neutralized for set -e/pipefail.)
    { grep -oE "$2" "$1" 2>/dev/null || true; } | wc -l
}

allow_for() {
    # allowlist format: <file> <pattern> <max-count>
    awk -v f="$1" -v p="$2" '!/^#/ && $1 == f && $2 == p { print $3; found=1 } END { if (!found) print "0" }' "$ALLOWLIST" | head -1
}

for f in "${FILES[@]}"; do
    for p in "${PATTERNS[@]}"; do
        n=$(count_in_file "$f" "$p")
        [ "$n" -eq 0 ] && continue
        max=$(allow_for "$f" "$p")
        if [ "$n" -gt "$max" ]; then
            echo "[purity] FAIL: $f uses '$p' $n times (frozen at: $max)."
            echo "         Charter §3: adding handle producers/consumers or kslab"
            echo "         uses is forbidden. If this is a legitimate reduction from"
            echo "         elsewhere, the allowlist is NOT edited; if it is a new use,"
            echo "         the change must be rejected (or amend charter+ledger with a"
            echo "         justification in the same commit)."
            fail=1
        elif [ "$n" -lt "$max" ]; then
            echo "[purity] progress: $f '$p' $n < $max — consider lowering the allowlist"
            progress=1
        fi
    done
done

# Allowlist entries whose file no longer uses the pattern (or is gone): a
# cleanup reminder, not a failure.
while read -r f p max; do
    case "$f" in \#*|"") continue ;; esac
    if [ ! -f "$f" ]; then
        echo "[purity] note: orphan allowlist entry ($f) — remove it"
        continue
    fi
    n=$(count_in_file "$f" "$p")
    if [ "$n" -eq 0 ] && [ "$max" -gt 0 ]; then
        echo "[purity] progress: $f no longer uses '$p' — remove it from the allowlist"
        progress=1
    fi
done < "$ALLOWLIST"

if [ "$fail" -ne 0 ]; then
    echo "[purity] RESULT: FAIL"
    exit 1
fi
echo "[purity] RESULT: OK (allowlist respected$( [ $progress -eq 1 ] && echo '; progress pending consolidation' ))"
