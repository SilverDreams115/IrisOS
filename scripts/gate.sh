#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# gate.sh — the seventeen lanes AGENTS.md lists, in one command.
#
# Two rules, both learned the hard way in one session:
#
#   1. CLASSIFY BY EXIT CODE.  A hand-written classifier that greps for a
#      phrase lied three times here — it marked passing selftest lanes FAIL
#      because the harness prints `[headless] ...` and not the SUITE line, it
#      PASSED an smp-2 run that had failed because a diagnostic happens to
#      echo the suite's verdict, and it marked a 53-of-53 `spec-prove` FAIL
#      because the number 47 was hardcoded in it.  A lane's own `$?` is the
#      only thing that cannot drift.
#   2. ONE QEMU AT A TIME, AND NO BUILD WHILE ONE RUNS.  They share
#      build/iris-disk.img, and a `make` from elsewhere rewrites it under a
#      live boot — one -smp 4 verdict had to be discarded for exactly that.
#      So this script is strictly sequential and nothing else may run beside
#      it.
#
# Usage:  bash scripts/gate.sh            (the full seventeen)
#         FAST=1 bash scripts/gate.sh     (skip the QEMU lanes)
set -u
cd "$(dirname "$0")/.."
P=0; F=0; TOTAL=0; FAILED=""

# ── where each lane's evidence goes ─────────────────────────────────────────
#
# Twice in one session a failing lane's reason was destroyed before it could be
# read.  First the three selftest lanes shared one serial log, so the smp-4 run
# overwrote the smp-2 failure; that was fixed by naming the log per processor
# count.  Then `smoke-stage10` failed and the rerun investigating it overwrote
# the same file again — per CONFIGURATION is not per RUN.
#
# So every lane's full output is saved here, and any serial log the lane wrote
# is snapshotted beside it under the lane's name.  A later run cannot erase
# what a gate already saw.
GLOG="${GATE_LOGS:-build/gate-logs}"
rm -rf "$GLOG"; mkdir -p "$GLOG"

# The SUMMARY line is CHOSEN, not taken.
#
# It used to be "the last non-empty line", which for a failing `make` is
# `Error 1` — so the gate printed the one line that says nothing and threw away
# the `[headless] ...` line that says why.  And for a GROUPED lane the last
# line is whatever the last script happened to print: `make check` summarised
# itself with a fragment of ld's help text.
summary() {                   # summary <rc> <file>
    local rc="$1" f="$2" n
    if [ "$rc" != "0" ]; then
        # The harness states its own complaint as `[headless] <reason>`, and
        # that is the line worth printing — `Error 1` is make relaying a status
        # and says nothing.  `[headless] log:` is the trailing pointer, not a
        # reason, so it is excluded.  This ordering is tested: a canned failure
        # whose reason reads "a file did not round-trip" matched none of the
        # keyword patterns tried first and fell through to `Error 1`, which is
        # exactly the defect being fixed.
        grep -m1 -E "^\[headless\] " "$f" | grep -v "^\[headless\] log:" && return 0
        grep -m1 -E "FAIL|refused|cannot|missing|error:" "$f" && return 0
        grep -m1 -E "Error [0-9]+" "$f" && return 0
        grep -vE "^[[:space:]]*$" "$f" | tail -1
        return 0
    fi
    n=$(grep -cE "RESULT:|^result:" "$f")
    if [ "${n:-0}" -gt 1 ]; then echo "$n verdicts, all OK"; return 0; fi
    grep -E "RESULT:|^result:|traces|states|applications|obligations|pairs|points|slice held|signature observed|SUITE" "$f" | tail -1 && return 0
    grep -vE "^[[:space:]]*$" "$f" | tail -1
}

run() {                       # run <label> <command...>
    local label="$1"; shift
    local slug rc t0 started secs f
    slug=$(printf '%s' "$label" | tr ' /' '__')
    f="$GLOG/$slug.out"
    started=$(date +%s)
    t0=$SECONDS
    "$@" > "$f" 2>&1; rc=$?
    secs=$(( SECONDS - t0 ))
    find build -maxdepth 1 -name 'qemu-*.log' -newermt "@$started" 2>/dev/null \
        | while read -r l; do cp "$l" "$GLOG/$slug.$(basename "$l")" 2>/dev/null; done
    if [ "$rc" = "0" ]; then
        P=$((P+1))
        printf '%-26s PASS %5ss  %s\n' "$label" "$secs" "$(summary 0 "$f" | cut -c1-92)"
    else
        F=$((F+1)); FAILED="$FAILED $label"
        printf '%-26s FAIL %5ss rc=%s  %s\n' "$label" "$secs" "$rc" "$(summary "$rc" "$f" | cut -c1-92)"
        printf '%-26s      evidencia: %s\n' "" "$f"
    fi
    TOTAL=$(( TOTAL + secs ))
}


# A zero-warning build is the claim, so the warnings are counted rather than
# trusted to make `make` fail.
build_clean() {
    local n
    n=$(make 2>&1 | grep -cE 'error:|warning:')
    [ "$n" = "0" ] || { echo "FAIL: $n diagnostics (error:/warning:)"; return 1; }
    echo "RESULT: zero-warning build"
}

run make                 build_clean
for t in check test-unit spec-differential spec-rights spec-prove spec-bridge \
         spec-abstract spec-noninterference spec-exhaustive; do
    run "$t" make "$t"
done

if [ "${FAST:-0}" != "1" ]; then
    for n in 1 2 4; do
        run "selftests smp$n" env ENABLE_RUNTIME_SELFTESTS=1 IRIS_QEMU_SMP=$n \
            IRIS_QEMU_TIMEOUT_SECS=$((300 + 200 * n)) \
            make smoke-runtime-selftests
    done
    run smoke-stage10 env ENABLE_RUNTIME_SELFTESTS=1 make smoke-stage10
    run smoke-verified make smoke-verified
fi

echo
if [ "$F" = "0" ]; then
    echo "GATE: $P green, 0 failed   (${TOTAL}s; evidencia en $GLOG)"
else
    echo "GATE: $P green, $F FAILED —$FAILED   (${TOTAL}s; evidencia en $GLOG)"
fi
exit $([ "$F" = "0" ] && echo 0 || echo 1)
