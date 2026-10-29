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
P=0; F=0; FAILED=""

run() {                       # run <label> <command...>
    local label="$1"; shift
    local out rc
    out=$("$@" 2>&1); rc=$?
    local last
    last=$(printf '%s\n' "$out" | grep -vE '^\s*$' | tail -1 | cut -c1-96)
    if [ "$rc" = "0" ]; then
        P=$((P+1)); printf '%-26s PASS  %s\n' "$label" "$last"
    else
        F=$((F+1)); FAILED="$FAILED $label"
        printf '%-26s FAIL  rc=%s  %s\n' "$label" "$rc" "$last"
    fi
}

# A zero-warning build is the claim, so the warnings are counted rather than
# trusted to make `make` fail.
build_clean() {
    local n
    n=$(make 2>&1 | grep -cE 'error:|warning:')
    [ "$n" = "0" ] || { echo "$n diagnostics"; return 1; }
    echo "zero-warning build"
}

run make                 build_clean
for t in check-purity check-abi check-locks check-loops check-binary \
         test-unit spec-differential spec-rights spec-prove spec-bridge spec-isabelle \
         spec-theory spec-noninterference spec-exhaustive; do
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
    echo "GATE: $P green, 0 failed"
else
    echo "GATE: $P green, $F FAILED —$FAILED"
fi
exit $([ "$F" = "0" ] && echo 0 || echo 1)
