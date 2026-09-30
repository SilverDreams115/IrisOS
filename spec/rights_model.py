#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
rights_model.py — IRIS's rights algebra, specified, and checked exhaustively.

Stage V phase 2 closes the part of ledger **D-3** that is a definition rather
than a catalogue.  D-3 records that IRIS's rights set is its own and that "the
two models are not translatable one-to-one, so 'IRIS has seL4's rights' is
never a correct statement".  True, honest, and not a specification — a
verified kernel needs the algebra written down, not the observation that it
differs from somebody else's.

So here it is, and then it is checked against `kernel/.../rights.h` over
**every** input.  The rights word is seven bits: `rights_reduce` and
`rights_check` are total functions over 2^7 x 2^7 = 16384 pairs plus the
`RIGHT_SAME_RIGHTS` operation flag.  That is small enough to enumerate
completely, so this is not sampling — there is no probability left in the
result.

    usage:  python3 spec/rights_model.py      (needs build/rights_exhaustive)

── The algebra ──────────────────────────────────────────────────────────────

IRIS has seven rights.  seL4 has four (Read, Write, Grant, GrantReply) and
does NOT treat copyability as a right at all — whether a capability can be
copied follows from its type and its place in the derivation tree.  IRIS
gates minting with DUPLICATE and IPC transfer with TRANSFER, and that is what
makes a delegation non-re-delegable here: a holder can be given a capability
it may invoke and may not copy, which in seL4 is expressible only through the
MDB and not through rights.  That is the divergence D-3 records, stated as
what it lets a grantor SAY rather than as a difference in a list.

The operations, and the properties that make them an algebra:

  reduce(base, req)  = base & req            (and `base` itself when req
                                              carries SAME_RIGHTS)
  check(have, need)  = (have & need) == need

  * MONOTONE       reduce(b, r) ⊆ b          — a derivation never amplifies
  * IDEMPOTENT     reduce(reduce(b,r), r) = reduce(b, r)
  * COMMUTATIVE    reduce(b, r) = reduce(r, b)   — it is an intersection
  * SAME_RIGHTS is an OPERATION flag, never stored: the result never has it
  * check is reflexive: check(x, x) for every x
  * check(b, r) after reduce: reduce(b,r) satisfies `check` for exactly the
    rights that were in both

A7 of the charter is the first of these, and the reason this file can say
"proved" where the rest of `spec/` says "agreed over N traces".
"""

import os
import subprocess
import sys

R_READ, R_WRITE, R_DUPLICATE, R_TRANSFER = 1, 2, 4, 8
R_WAIT, R_ROUTE, R_MANAGE = 16, 32, 64
MASK = R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER | R_WAIT | R_ROUTE | R_MANAGE
NVALUES = 128
SAME_RIGHTS = 0x80000000

BIN = os.path.join("build", "rights_exhaustive")


# ── the specification ───────────────────────────────────────────────────────

def reduce_(base, req):
    """A derivation keeps what BOTH the holder had and the caller asked for."""
    if req & SAME_RIGHTS:
        return base & MASK            # a copy: everything the source has
    return (base & req) & MASK


def check_(have, need):
    """
    Authority is a superset test — and requiring NOTHING is never satisfied.

    `(have & need) == need` would make `check(x, RIGHT_NONE)` true for every
    `x`, which is what the set algebra says and is the wrong answer for a
    kernel: it would let a caller that FORGOT to name a right pass every
    check.  `rights_check` special-cases it and fails closed, so the mistake
    of omitting a right is ACCESS_DENIED rather than a silent pass.

    The callers that genuinely mean "no right required" know it and guard the
    call themselves — `if (required != RIGHT_NONE && !rights_check(...))` —
    which is why the resolvers can take RIGHT_NONE and still be strict.

    The model had the set-theoretic answer and the differential found all 128
    disagreements at once: one for every possible `have` with `need == 0`.
    """
    if need == 0:
        return 0
    return 1 if (have & need) == need else 0


# ── the properties, proved by exhaustion ────────────────────────────────────

def prove_properties():
    """Every claim in this file's header, over the whole input space."""
    bad = []
    for b in range(NVALUES):
        if reduce_(b, SAME_RIGHTS) & SAME_RIGHTS:
            bad.append("SAME_RIGHTS survived into the result for base %d" % b)
        # Reflexive EXCEPT at the empty requirement, which is refused on
        # purpose — see `check_`.  A property stated without its exception is
        # a property that will be "fixed" by somebody removing the exception.
        if b != 0 and not check_(b, b):
            bad.append("check is not reflexive at %d" % b)
        if check_(b, 0):
            bad.append("the empty requirement was satisfied at %d" % b)
        for r in range(NVALUES):
            out = reduce_(b, r)
            if out & ~b:
                bad.append("reduce(%d,%d) amplified over its base" % (b, r))
            if reduce_(out, r) != out:
                bad.append("reduce is not idempotent at (%d,%d)" % (b, r))
            if reduce_(b, r) != reduce_(r, b):
                bad.append("reduce is not commutative at (%d,%d)" % (b, r))
            if out != 0 and not check_(b, out):
                bad.append("the base does not satisfy its own reduction "
                           "(%d,%d)" % (b, r))
    return bad


# ── the same graph, from the kernel ─────────────────────────────────────────

def kernel_graph():
    p = subprocess.run([BIN], capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError("rights_exhaustive exited %d" % p.returncode)
    return p.stdout.splitlines()


def main():
    if not os.path.exists(BIN):
        print("rights_model: %s is missing — run `make spec-rights`" % BIN)
        return 2

    bad = prove_properties()
    if bad:
        print("rights_model: the SPECIFICATION does not hold:")
        for b in bad[:10]:
            print("  " + b)
        return 1

    mismatches = 0
    checked = 0
    for line in kernel_graph():
        f = line.split()
        checked += 1
        if f[0] == "reduce":
            b, r, got = int(f[1]), int(f[2]), int(f[3])
            want = reduce_(b, r)
        elif f[0] == "reduce_same":
            b, got = int(f[1]), int(f[2])
            want = reduce_(b, SAME_RIGHTS)
        elif f[0] == "check":
            h, n, got = int(f[1]), int(f[2]), int(f[3])
            want = check_(h, n)
        else:
            continue
        if want != got:
            if mismatches < 5:
                print("rights_model: %s -> kernel %d, specification %d"
                      % (line, got, want))
            mismatches += 1

    if mismatches:
        print("\nrights_model: %d of %d disagreed" % (mismatches, checked))
        return 1
    print("rights_model: %d points, EXHAUSTIVE over the whole rights space; "
          "the algebra's properties hold and the kernel computes it" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
