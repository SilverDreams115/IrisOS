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
    ops, occupied, uts = [], [], []
    for _ in range(nops):
        r = rng.random()
        # `ut` and `retype` only reach CNode 0 (ids 0..15): the syscall takes
        # CPtrs and this CSpace is one level deep.  Stated in spec_driver.c.
        if r < 0.10:
            s = rng.randrange(SPC)
            ops.append("ut %d" % s)
            if s not in occupied:
                occupied.append(s)
            if s not in uts:
                uts.append(s)
        elif r < 0.25 and uts:
            u = rng.choice(uts)
            d = rng.randrange(SPC)
            ops.append("retype %d %d %d" % (u, rng.randrange(4), d))
            if d not in occupied:
                occupied.append(d)
        elif r < 0.35 or not occupied:
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
        elif r < 0.55:
            # the SYSCALL publication paths, CNode 0 only
            src = rng.choice(occupied)
            dst = rng.randrange(SPC)
            if rng.random() < 0.5:
                ops.append("smint %d %d %d %d" % (
                    src % SPC, dst, rng.choice(RIGHTS_POOL),
                    rng.choice([0, 0, 7, 42])))
            else:
                ops.append("smove %d %d" % (src % SPC, dst))
            if dst not in occupied:
                occupied.append(dst)
        elif r < 0.62:
            # the read-only queries: what a slot holds, and whether two
            # capabilities name one object
            if rng.random() < 0.5:
                ops.append("ident %d" % rng.randrange(SPC))
            else:
                ops.append("same %d %d" % (rng.randrange(SPC),
                                           rng.randrange(SPC)))
        elif r < 0.70:
            if rng.random() < 0.5:
                ops.append("srevoke %d" % (rng.choice(occupied) % SPC))
            else:
                ops.append("sdelete %d" % (rng.choice(occupied) % SPC))
        elif r < 0.72 and len(occupied) >= 2:
            # rotate: dest, pivot, src.  Half the draws aim dest AT src,
            # which is the three-way SWAP branch and the only one that needs
            # a temporary — the branch a generator that never repeats an
            # argument would never reach.
            pv = rng.choice(occupied)
            sr = rng.choice([o for o in occupied if o != pv] or [pv])
            ds = sr if rng.random() < 0.5 else rng.randrange(NSLOTS)
            ops.append("rotate %d %d %d" % (ds, pv, sr))
            if ds not in occupied:
                occupied.append(ds)
        elif r < 0.85:
            ops.append("delete %d" % rng.choice(occupied))
        else:
            ops.append("revoke %d" % rng.choice(occupied))
    return ops


def run_model(ops):
    """Returns (model, [result of each operation])."""
    m = AuthorityModel(NCNODES, SPC)
    results = []
    for line in ops:
        f = line.split()
        if f[0] == "ut":
            results.append(m.install_untyped(int(f[1])))
        elif f[0] == "retype":
            results.append(m.retype(
                int(f[1]), ["ep", "notif", "reply", "sc"][int(f[2]) % 4],
                int(f[3])))
        elif f[0] == "seed":
            results.append(m.seed(
                int(f[1]), ["ep", "notif", "cnode", "ut"][int(f[2]) % 4],
                R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER))
        elif f[0] == "mint":
            results.append(m.mint(int(f[1]), int(f[2]), int(f[3]), int(f[4])))
        elif f[0] == "smint":
            results.append(m.syscall_mint(int(f[1]), int(f[2]),
                                          int(f[3]), int(f[4])))
        elif f[0] == "smove":
            results.append(m.move(int(f[1]), int(f[2])))
        elif f[0] == "srevoke":
            results.append(m.syscall_revoke(int(f[1])))
        elif f[0] == "sdelete":
            results.append(m.syscall_delete(int(f[1])))
        elif f[0] == "ident":
            results.append(m.identify(int(f[1])))
        elif f[0] == "same":
            results.append(m.same_object(int(f[1]), int(f[2])))
        elif f[0] == "rotate":
            results.append(m.rotate(int(f[1]), int(f[2]), int(f[3])))
        elif f[0] == "delete":
            results.append(m.delete(int(f[1])))
        elif f[0] == "revoke":
            results.append(m.revoke(int(f[1])))
    return m, results


def run_kernel(ops):
    """Returns (dump, [result of each operation])."""
    p = subprocess.run([DRIVER], input="\n".join(ops) + "\n",
                       capture_output=True, text=True, timeout=60)
    if p.returncode != 0:
        raise RuntimeError("spec_driver exited %d: %s" % (p.returncode, p.stderr))
    head, _, tail = p.stdout.partition("--\n")
    res = [int(l.split()[1]) for l in head.splitlines() if l.startswith("r ")]
    return tail.strip(), res


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

        model, want_res = run_model(ops)
        want = model.dump().strip()
        got, got_res = run_kernel(ops)

        why = check_all(model)
        if why:
            bad += 1
            print("=== trace seed %d: the MODEL violates an invariant ===" % seed)
            print("  %s" % why)
            print("  " + "\n  ".join(ops))
            if bad > 3:
                break
            continue

        if want_res != got_res:
            bad += 1
            first = next((k for k in range(min(len(want_res), len(got_res)))
                          if want_res[k] != got_res[k]), None)
            print("=== trace seed %d: model and kernel RETURNED differently ===" % seed)
            if first is not None:
                print("  op %d: `%s` -> model %d, kernel %d" % (
                    first, ops[first], want_res[first], got_res[first]))
            else:
                print("  different number of results: %d vs %d" % (
                    len(want_res), len(got_res)))
            print("  " + "\n  ".join(ops))
            if bad > 3:
                print("... stopping after 4 disagreements")
                break
        elif want != got:
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
