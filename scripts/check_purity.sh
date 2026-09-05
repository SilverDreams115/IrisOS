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
# And a second, different check: whether any slab-allocating function is named
# by a syscall handler at all.  The counts above froze a number; this asks the
# question the charter actually asks, which is who can reach it.
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

# ── Reachability: can ring 3 make the kernel allocate? ──────────────────────
#
# The count above is not the property the charter is about.  Charter M3 says
# the kernel does not implicitly allocate memory on somebody's behalf, and a
# per-file occurrence count cannot see WHO can reach an allocation — it stayed
# green while a new syscall put `kslab_alloc` one call away from ring 3, which
# is exactly the thing it exists to prevent.
#
# So: for every function that calls `kslab_alloc`, is that function called
# directly from a syscall handler?  One level of indirection, which is what the
# real case looked like (`sys_ioport_control_narrow` → `kbootcap_alloc_ports`).
# It is an approximation and it is stated as one: a deeper chain slips through,
# and the answer to that is to keep the chains shallow rather than to build a
# call-graph analyser in bash.
#
# The exceptions are the object families the ledger still records as
# kslab-backed, each of which IS reachable from ring 3 and has a row that says
# so.  A new name here needs a ledger row in the same commit; the row is named
# beside it so the exception cannot outlive its justification.
#
#   kvmo_alloc           ledger: "KVmo is fabricated, not retyped" (D-5)
#   kframe_alloc         ledger: "KFrame header sidecar (kslab)"
#   kframe_alloc_vmo_page  ditto — VMO pages have no Untyped to charge
#   kuntyped_create      ledger D-9: reached only through the DEVICE branch of
#                        retype, which nothing can reach — no device Untyped can
#                        exist, because the only site that would set the flag
#                        already requires one.  The exception is here so the
#                        gate stays honest about WHY, not because the code is
#                        acceptable — it is unreachable, which is different
KSLAB_RING3_OK="kvmo_alloc kframe_alloc kframe_alloc_vmo_page kuntyped_create"

mapfile -t ALLOC_FNS < <(
    for f in kernel/new_core/src/*.c kernel/core/**/*.c kernel/mm/**/*.c; do
        [ -f "$f" ] || continue
        awk '/^[a-zA-Z_].*\(/ { fn = $0 }
             /kslab_alloc/ { if (fn != "") print fn }' "$f"
    done | grep -oE '\b[a-z_][a-z0-9_]*\(' | tr -d '(' | sort -u
)

for fn in "${ALLOC_FNS[@]}"; do
    case " $KSLAB_RING3_OK " in *" $fn "*) continue ;; esac
    callers=$(grep -rlE "\b$fn\b" kernel/core/syscall/*.c 2>/dev/null || true)
    if [ -n "$callers" ]; then
        echo "[purity] FAIL: '$fn' allocates from the kernel slab and is named by"
        echo "         a syscall handler:"
        echo "$callers" | sed 's/^/           /'
        echo "         Charter M3: ring 3 cannot be given a way to spend the"
        echo "         kernel's memory.  Carve the object from an Untyped the"
        echo "         caller NAMES (see kbootcap_alloc_from / kioport_alloc_from),"
        echo "         or add the name to KSLAB_RING3_OK with a ledger row in the"
        echo "         same commit saying why it is allowed to stay."
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "[purity] RESULT: FAIL"
    exit 1
fi
echo "[purity] RESULT: OK (allowlist respected$( [ $progress -eq 1 ] && echo '; progress pending consolidation' ))"
