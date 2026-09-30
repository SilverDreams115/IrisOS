#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
shrink.py — turn a disagreement into the smallest trace that still shows it.

A random trace that disagrees is evidence; a four-line trace that disagrees is
an ARGUMENT, and the difference is most of the work of acting on one.  This is
delta debugging, kept deliberately simple: drop one operation at a time, keep
the drop if the two sides still disagree, repeat until nothing can be dropped.

    usage:  python3 spec/shrink.py <seed>
            python3 spec/shrink.py --file trace.txt
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from differential import gen_trace, run_model, run_kernel   # noqa: E402


def disagrees(ops):
    """True when the model and the kernel render different states."""
    if not ops:
        return False
    try:
        want = run_model(ops).dump().strip()
        got = run_kernel(ops).strip()
    except Exception:
        return False
    return want != got


def shrink(ops):
    """
    Drop operations while the disagreement survives.

    Two passes: first from the end (a prefix is usually enough), then any
    single line anywhere.  It is not the fastest shrinker there is; it is one
    whose output nobody has to double-check.
    """
    # shortest failing prefix
    lo, hi = 1, len(ops)
    while lo < hi:
        mid = (lo + hi) // 2
        if disagrees(ops[:mid]):
            hi = mid
        else:
            lo = mid + 1
    ops = ops[:lo] if disagrees(ops[:lo]) else ops

    # then remove anything that is not load-bearing
    changed = True
    while changed:
        changed = False
        i = 0
        while i < len(ops):
            trial = ops[:i] + ops[i + 1:]
            if disagrees(trial):
                ops = trial
                changed = True
            else:
                i += 1
    return ops


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--file":
        ops = [l.strip() for l in open(sys.argv[2]) if l.strip()]
    elif len(sys.argv) >= 2:
        rng = random.Random(int(sys.argv[1]))
        ops = gen_trace(rng, rng.randrange(8, 60))
    else:
        print(__doc__)
        return 2

    if not disagrees(ops):
        print("shrink: that trace does not disagree")
        return 1

    small = shrink(ops)
    print("=== the smallest trace that still disagrees (%d ops) ===" % len(small))
    for o in small:
        print("  " + o)
    print("--- model says")
    print("  " + "\n  ".join(run_model(small).dump().strip().splitlines() or ["(empty)"]))
    print("--- kernel says")
    print("  " + "\n  ".join(run_kernel(small).strip().splitlines() or ["(empty)"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
