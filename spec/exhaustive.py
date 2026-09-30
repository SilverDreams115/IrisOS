#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
exhaustive.py — the model against the C over EVERY trace, not 200 of them.

Stage V phase 4's second link, and it attacks the softest claim in the whole
chain.  After `spec-prove` and `spec-bridge`, the edifice reads:

    invariants.py    PROVED by z3 for every symbolic state
    spec-bridge      those same predicates == the model's own checkers
    spec-differential  the model == the real kcnode.c ............ over 200
                                                                   RANDOM
                                                                   traces

Everything above the last line is a proof or a total check.  The last line is
sampling, and it is the line that actually touches the kernel — so it is both
the weakest link and the only one that says anything about the C.

This lane makes it total over a bounded domain: **every trace of length <= L
drawn from a fixed alphabet**, run through the model and the real C, compared
on every intermediate return code and on the final state.  Not "no
disagreement was found"; "no disagreement exists, below this bound".

That is bounded model checking, and it is the standard thing that sits between
testing and proof.  It does not prove refinement — a disagreement needing five
operations, or a slot index outside the three used here, is outside what it
covers, and `spec-differential`'s random traces keep running for exactly that
reason.  Small scope is where the bugs are, and "where the bugs are" is not
"all of them".

── Why it is fast enough to be exhaustive ──────────────────────────────────

`spec_driver.c` grew a `reset` operation: it prints the finished trace's dump
and empties the CSpace in the SAME process, by deleting every slot rather than
reallocating, so nothing leaks across traces.  One process then runs thousands
of traces instead of one, which is the difference between this lane taking
minutes and taking a day.

The reset is not only an optimisation.  Emptying a populated CSpace completely
is something the random lane almost never does, and this one does it after
every single trace.

── What sabotage showed about this lane ────────────────────────────────────

A lane that cannot fail proves nothing, so the model was deliberately broken
— occupancy checked before authority, which is the exact ordering phase 1
found the kernel disagreeing about — and the lane was watched.  At depth 3 it
catches it; at depth 2 it does not.  That is the bound doing what a bound
does, and it is the reason the depth is printed on every run rather than
mentioned once in a comment.

It also showed a gap: every mint in the alphabet had DUPLICATE on its source,
so the rule was only ever caught INDIRECTLY, through the badge ordering.
`mint 2 0 5 0` was added to reach it head-on.

    usage:  python3 spec/exhaustive.py [depth]      (`make spec-exhaustive`)
"""

import itertools
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from authority_model import check_all                          # noqa: E402
from differential import DRIVER, NCNODES, SPC, run_model       # noqa: E402

# ── the alphabet ────────────────────────────────────────────────────────────
#
# Three slots, because the shapes that matter — a derivation chain, a revoke
# with a surviving sibling, a reparent — all fit in three and the trace count
# is exponential in the alphabet.  Every operation class the driver
# understands is represented, including the syscall publication paths, since
# the slot-0 defect this project found lived in exactly those and in no other.
ALPHABET = [
    "ut 0", "ut 1",
    "seed 0 0", "seed 1 1", "seed 2 2",
    "retype 0 0 1", "retype 0 1 2", "retype 1 2 0",
    "mint 0 1 5 0",          # READ|DUPLICATE, inherit badge
    "mint 0 1 5 7",          # a fresh badge on an identity type
    "mint 1 2 1 0",          # a reduction to READ alone, leaving slot 2
                             # WITHOUT duplicate...
    "mint 2 0 5 0",          # ...so that this one is a mint from a source
                             # that may NOT be copied.  Every other mint here
                             # has DUPLICATE on its source, so without this
                             # op the rule "authority is checked before
                             # occupancy" is never exercised head-on.
                             #
                             # Measured, not assumed: a model sabotaged to
                             # check occupancy first is caught at depth 3
                             # either way — but via the BADGE rule, not the
                             # duplicate one — and is missed entirely at
                             # depth 2.  This op makes the rule phase 1
                             # actually found a disagreement in reachable
                             # directly, at the depth where it lives.
    "mint 0 2 15 42",        # everything, and a badge
    "smint 0 1 5 0",         # the syscall path, which refuses differently
    "smove 0 2",
    "delete 0", "delete 1",
    "revoke 0", "revoke 1",
    "sdelete 1", "srevoke 0",
]

CHUNK = 400          # traces per driver process


def run_kernel_batch(traces):
    """Every trace in one process, separated by `reset`."""
    lines = []
    for t in traces:
        lines.extend(t)
        lines.append("reset")
    lines.pop()                      # the last trace ends at EOF, not a reset
    p = subprocess.run([DRIVER], input="\n".join(lines) + "\n",
                       capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        raise RuntimeError("spec_driver exited %d: %s"
                           % (p.returncode, p.stderr[:400]))
    out = []
    for seg in p.stdout.split("==\n"):
        if not seg.strip():
            continue
        res, _, dump = seg.partition("--\n")
        out.append(([l[2:] for l in res.strip().splitlines() if l[:2] == "r "],
                    dump.strip().splitlines()))
    return out


def main():
    depth = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    if not os.path.exists(DRIVER):
        print("exhaustive: %s is missing — run `make spec-driver`" % DRIVER)
        return 2

    traces = [list(t) for t in itertools.product(ALPHABET, repeat=depth)]
    total = len(traces)
    print("exhaustive: %d traces — every sequence of %d operations over an "
          "alphabet of %d" % (total, depth, len(ALPHABET)))

    bad = 0
    checked = 0
    for base in range(0, total, CHUNK):
        chunk = traces[base:base + CHUNK]
        kres = run_kernel_batch(chunk)
        if len(kres) != len(chunk):
            print("exhaustive: driver returned %d segments for %d traces"
                  % (len(kres), len(chunk)))
            return 1
        for t, (kcodes, kdump) in zip(chunk, kres):
            m, mcodes = run_model(t)
            checked += 1
            mcodes = [str(c) for c in mcodes]
            if mcodes != kcodes:
                bad += 1
                if bad <= 5:
                    print("exhaustive: RESULT CODES differ\n  trace: %s\n"
                          "  model:  %s\n  kernel: %s"
                          % (" ; ".join(t), mcodes, kcodes))
                continue
            mdump = m.dump().strip().splitlines()
            if mdump != kdump:
                bad += 1
                if bad <= 5:
                    print("exhaustive: STATE differs\n  trace: %s\n"
                          "  model:\n    %s\n  kernel:\n    %s"
                          % (" ; ".join(t), "\n    ".join(mdump),
                             "\n    ".join(kdump)))
                continue
            why = check_all(m)
            if why:
                bad += 1
                if bad <= 5:
                    print("exhaustive: the model violates an invariant: %s\n"
                          "  trace: %s" % (why, " ; ".join(t)))

    if bad:
        print("\nexhaustive: %d disagreement(s) over %d traces" % (bad, checked))
        return 1
    print("exhaustive: %d traces, EVERY sequence of %d operations over the "
          "alphabet; the model and the real kcnode.c agree on every "
          "intermediate result code and on every final state, and every "
          "charter invariant held throughout" % (checked, depth))
    return 0


if __name__ == "__main__":
    sys.exit(main())
