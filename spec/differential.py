#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
differential.py — the executable specification against the kernel that exists.

This is the check Phase 1 of the IRIS-V plan is FOR.  It generates random
sequences of capability operations, drives the same sequence through
`spec/authority_model.py` and through `build/spec_driver` (which links the
real `kcnode.c`, `kobject.c` and `cspace.c`), and compares the resulting
CSpace exactly.

A disagreement is one of three things and all three are worth having:

  * the model is wrong  — and the model is what an Isabelle abstract
    specification would be written from, so finding it now is cheaper than
    finding it in Isabelle;
  * the kernel is wrong — which is the case the 325 runtime tests cannot
    cover, because they assert what somebody already suspected;
  * the specification is AMBIGUOUS — two readings, both defensible, and the
    tree has to decide which one it means.  That is the most valuable of the
    three and the one prose can never surface.

It also runs the charter's invariants against the model's state after every
trace (`check_all`), so a trace that makes the model violate A7, A8, A9, A10
or object liveness is reported even when the two implementations agree.

    usage:  python3 spec/differential.py [traces] [--seed N] [--verbose]
"""

import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from authority_model import (AuthorityModel, check_all,   # noqa: E402
                             R_READ, R_WRITE, R_DUPLICATE, R_TRANSFER)

DRIVER = os.path.join("build", "spec_driver")
NCNODES, SPC = 4, 16
NSLOTS = NCNODES * SPC
RIGHTS_POOL = [
    R_READ, R_WRITE, R_READ | R_WRITE,
    R_READ | R_DUPLICATE, R_READ | R_WRITE | R_DUPLICATE,
    R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER,
]


def gen_trace(rng, nops):
    """
    A trace that reaches interesting states rather than a uniform one.

    Uniform random operations mostly bounce off empty slots.  This keeps a
    little state so that mints usually have something to derive FROM, which is
    what puts depth in the derivation tree — and depth is where revoke and
    reparenting are decided.
    """
    ops, occupied = [], []
    for _ in range(nops):
        r = rng.random()
        if r < 0.20 or not occupied:
            s = rng.randrange(NSLOTS)
            ops.append("seed %d %d" % (s, rng.randrange(4)))
            if s not in occupied:
                occupied.append(s)
        elif r < 0.65:
            src = rng.choice(occupied)
            dst = rng.randrange(NSLOTS)
            ops.append("mint %d %d %d %d" % (
                src, dst, rng.choice(RIGHTS_POOL), rng.choice([0, 0, 0, 7, 42])))
            if dst not in occupied:
                occupied.append(dst)
        elif r < 0.85:
            ops.append("delete %d" % rng.choice(occupied))
        else:
            ops.append("revoke %d" % rng.choice(occupied))
    return ops


def run_model(ops):
    m = AuthorityModel(NCNODES, SPC)
    for line in ops:
        f = line.split()
        if f[0] == "seed":
            m.seed(int(f[1]), ["ep", "notif", "cnode", "ut"][int(f[2]) % 4],
                   R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER)
        elif f[0] == "mint":
            m.mint(int(f[1]), int(f[2]), int(f[3]), int(f[4]))
        elif f[0] == "delete":
            m.delete(int(f[1]))
        elif f[0] == "revoke":
            m.revoke(int(f[1]))
    return m


def run_kernel(ops):
    p = subprocess.run([DRIVER], input="\n".join(ops) + "\n",
                       capture_output=True, text=True, timeout=60)
    if p.returncode != 0:
        raise RuntimeError("spec_driver exited %d: %s" % (p.returncode, p.stderr))
    return p.stdout.strip()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = "--verbose" in sys.argv
    ntraces = int(args[0]) if args else 200
    seed0 = 0
    for a in sys.argv[1:]:
        if a.startswith("--seed"):
            seed0 = int(a.split("=", 1)[1]) if "=" in a else 0

    if not os.path.exists(DRIVER):
        print("differential: %s is missing — run `make spec-driver`" % DRIVER)
        return 2

    bad = 0
    for i in range(ntraces):
        seed = seed0 + i
        rng = random.Random(seed)
        ops = gen_trace(rng, rng.randrange(8, 60))

        model = run_model(ops)
        want = model.dump().strip()
        got = run_kernel(ops)

        why = check_all(model)
        if why:
            bad += 1
            print("=== trace seed %d: the MODEL violates an invariant ===" % seed)
            print("  %s" % why)
            print("  " + "\n  ".join(ops))
            if bad > 3:
                break
            continue

        if want != got:
            bad += 1
            print("=== trace seed %d: model and kernel DISAGREE ===" % seed)
            print("--- trace")
            print("  " + "\n  ".join(ops))
            print("--- model says")
            print("  " + "\n  ".join(want.splitlines() or ["(empty)"]))
            print("--- kernel says")
            print("  " + "\n  ".join(got.splitlines() or ["(empty)"]))
            if bad > 3:
                print("... stopping after 4 disagreements")
                break
        elif verbose:
            print("seed %d: %d ops, agreed" % (seed, len(ops)))

    if bad:
        print("\ndifferential: %d of %d traces disagreed" % (bad, ntraces))
        return 1
    print("differential: %d traces, model and kernel agree, "
          "and every charter invariant held" % ntraces)
    return 0


if __name__ == "__main__":
    sys.exit(main())
