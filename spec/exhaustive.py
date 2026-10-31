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
    # From slot ONE, because slot 0 is `IRIS_CPTR_NULL`: the two `retype 0 ...`
    # symbols above can only ever answer INVALID_ARG, which is a refusal worth
    # covering and is not a construction.  Until this symbol existed no
    # notification born with WAIT could exist anywhere in this alphabet, so
    # every `npoll` in it answered NOT_FOUND or WRONG_TYPE and a model
    # sabotaged to answer 0 instead of WOULD_BLOCK passed all 810000 traces.
    "retype 1 1 2",
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
    "rotate 2 0 1",          # dest, pivot, src — two relocations
    "rotate 0 1 0",          # dest == src: the three-way SWAP branch
    "delete 0", "delete 1",
    "revoke 0", "revoke 1",
    "sdelete 1", "srevoke 0",
    #
    # D-13, against the real `kvspace.c`.  Four symbols are enough to reach
    # the case the charter names: `vframe 0` puts a frame in slot 0,
    # `mint 0 1 5 0` derives slot 1 from it, `vmap 1 0` maps THROUGH the
    # derived slot — so the space's capability is a sibling of slot 1 — and
    # `revoke 0` then takes the mapping with the authority it descended from.
    # Every sequence of four over this alphabet includes that one, and also
    # every way of getting it wrong.
    #
    "vframe 0", "vmap 0 0", "vmap 1 0", "vunmap 0 0",
    #
    # The DATA channel, against the real `knotification.c`.  Three symbols,
    # picked the same way D-13's four were — the fewest that make a named case
    # reachable at depth 4, given what the rest of the alphabet already
    # builds.
    #
    #   `ut 1`, `retype 1 1 2`, `nsig 2 0`, `npoll 2`
    #       is four: a notification carved from an Untyped, born with WAIT,
    #       signalled, and taken.  Bits a sender chose arriving at a receiver,
    #       exhaustively over every way of arranging those four and every way
    #       of getting them wrong.
    #
    #       From slot 1 and not slot 0.  The first version of this comment
    #       said `ut 0`, `retype 0 1 2` — and slot 0 is the null capability,
    #       so that retype answers INVALID_ARG and slot 2 stays empty.  The
    #       sequence was unreachable and the comment read exactly like
    #       coverage; what caught it was sabotaging the model and watching all
    #       810000 traces pass.
    #
    #   `seed 1 1`, `mint 1 2 1 0`, `nsig 2 0` / `npoll 2`
    #       is two plus one: slot 2 ends up holding a notification capability
    #       reduced to READ alone, and then BOTH ends are refused —
    #       ACCESS_DENIED to signal and ACCESS_DENIED to poll.  The rights gate
    #       is on each direction separately, which is what makes it a gate.
    #
    #       The sharper case — WRITE without WAIT, so the holder may send and
    #       may not receive — is NOT reachable at depth 4 over this alphabet,
    #       and saying so is the point.  Getting it needs a notification
    #       capability carrying WRITE and not WAIT in slot 2, and no three
    #       symbols here build one: the only mint into slot 2 from a
    #       notification source is `mint 1 2 1 0`, which reduces to READ.  An
    #       earlier version of this comment claimed `rotate 2 0 1` did it;
    #       that rotate answers NOT_FOUND.  The asymmetry is covered by
    #       `spec-differential`, where a seeded notification signals (0) and
    #       refuses to be polled (-5) on the one capability, and proved
    #       outright by `spec-prove`'s DC rows, which gate the two directions
    #       on WRITE and WAIT separately for every state.
    #
    # What is NOT reachable at depth 4, stated rather than left to look like
    # coverage: exactly-once delivery (two takes after one signal) and
    # accumulation (two signals before one take) both need five, because the
    # notification costs two operations to build with WAIT.  `SPEC_DEPTH=5`
    # reaches them; the random lane emits both as units and the four
    # sabotages in its comments are the ones that catch them.  Two patterns
    # are in the alphabet and not one so that accumulation is observable at
    # all when the depth does reach it: 0x8000000000000000 | 1 is neither.
    #
    "nsig 2 0", "nsig 2 2", "npoll 2",
]

CHUNK = 400          # traces per driver process

# D-7 shadow-comparison coverage, accumulated across driver processes.
SHADOW = {"checks": 0, "mismatch": 0}


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
    # D-7's shadow comparison reports its COVERAGE on stderr.  `shadow 0` from
    # a check that never executed is not evidence, so the count is read and
    # accumulated rather than assumed to be non-zero.
    for ln in p.stderr.splitlines():
        if ln.startswith("shadow-checks "):
            f = ln.split()
            SHADOW["checks"] += int(f[1])
            SHADOW["mismatch"] += int(f[3])
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
    if SHADOW["checks"] == 0:
        print("exhaustive: the D-7 shadow comparison never RAN — `shadow 0` "
              "in the dumps means nothing and this lane is not evidence for "
              "retiring the reference count")
        return 1
    if SHADOW["mismatch"]:
        print("exhaustive: the MDB predicate and the reference count "
              "disagreed %d time(s) in %d comparisons"
              % (SHADOW["mismatch"], SHADOW["checks"]))
        return 1

    print("exhaustive: %d traces, EVERY sequence of %d operations over the "
          "alphabet; the model and the real kcnode.c agree on every "
          "intermediate result code and on every final state, and every "
          "charter invariant held throughout; D-7's predicate agreed with the "
          "reference count on all %d finality decisions"
          % (checked, depth, SHADOW["checks"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
