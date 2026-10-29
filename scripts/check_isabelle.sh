#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The abstract specification, PROVED — `spec/isabelle/IRIS_Authority.thy`.
#
# Same convention as `spec-prove` has for z3: a machine with no prover has
# MISSING EVIDENCE, not a failure, so this reports that it checked nothing and
# returns 0.  What it must never do is report success without a prover.
#
# Two things are checked when one is found: the session builds, and the theory
# contains no `oops` and no `sorry` as proof commands.  The build alone is not
# enough — `oops` abandons a goal and lets the session finish, which is exactly
# how this file sat unproved AND unloadable for as long as it did.
set -u
cd "$(dirname "$0")/.."

ISA="${ISABELLE:-}"
[ -n "$ISA" ] || ISA=$(command -v isabelle 2>/dev/null || true)
[ -n "$ISA" ] || for c in "$HOME"/opt/Isabelle*/bin/isabelle /opt/Isabelle*/bin/isabelle; do
    [ -x "$c" ] && ISA="$c" && break
done

if [ -z "$ISA" ] || [ ! -x "$ISA" ]; then
    echo "[isabelle] no prover found — CHECKED NOTHING (set ISABELLE=/path/to/isabelle)."
    echo "[isabelle] A machine with no prover has missing evidence, not a failure;"
    echo "           the theory's own text says what each lemma rests on."
    exit 0
fi

# `oops`/`sorry` as COMMANDS, not as the word in prose.
bad=$(grep -cE '^[[:space:]]*(oops|sorry)([[:space:]]|$)' spec/isabelle/IRIS_Authority.thy || true)
if [ "$bad" != "0" ]; then
    echo "[isabelle] FAIL: $bad abandoned proof(s) — oops/sorry in the theory:"
    grep -nE '^[[:space:]]*(oops|sorry)([[:space:]]|$)' spec/isabelle/IRIS_Authority.thy
    exit 1
fi

out=$("$ISA" build -D spec/isabelle 2>&1); rc=$?
if [ "$rc" != "0" ]; then
    echo "[isabelle] FAIL: the session did not build"
    printf '%s\n' "$out" | tail -20
    exit 1
fi

n=$(grep -cE '^(lemma|theorem) ' spec/isabelle/IRIS_Authority.thy)
echo "[isabelle] RESULT: OK ($n lemmas, no oops and no sorry, session built by" \
     "$("$ISA" version 2>/dev/null || echo isabelle))"
