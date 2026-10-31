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
    #
    # The thresholds ASCEND, and they did not.
    #
    # `mint` tested `r < 0.65` from the third position, so every draw between
    # 0.35 and 0.65 landed there and the next three branches — the syscall
    # publication paths, the read-only queries, and later D-13's mapping ops —
    # were unreachable.  Measured, not reasoned: counting the operations in 300
    # generated traces produced no `smint`, no `smove`, no `ident` and no
    # `same` at all.  The lane still said "200 traces, model and kernel agree",
    # which is true of a smaller alphabet than it reads as, and `ident`/`same`
    # are in NEITHER lane's alphabet so nothing exercised them.
    #
    # A generator with dead branches is the same defect as a check that cannot
    # fail: the output is indistinguishable from coverage.
    #
    # And the slots a NOTIFICATION was last put into.  The data-channel ops
    # need it: drawing their target uniformly from `occupied` found the
    # refusal branches easily and almost never polled one live notification
    # twice, which is the one property the channel exists to have — bits are
    # delivered EXACTLY once.  A deliberately non-consuming model survived 400
    # traces before this list existed.  It is a hint and not a claim: a later
    # delete or rotate can empty the slot, and the trace is still valid.
    ops, occupied, uts, notifs = [], [], [], []
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
            ty = rng.randrange(4)
            ops.append("retype %d %d %d" % (u, ty, d))
            if d not in occupied:
                occupied.append(d)
            if ty % 4 == 1 and d not in notifs:
                notifs.append(d)
        elif r < 0.35 or not occupied:
            s = rng.randrange(NSLOTS)
            ty = rng.randrange(4)
            ops.append("seed %d %d" % (s, ty))
            if s not in occupied:
                occupied.append(s)
            # A SEEDED notification carries no WAIT, so it can be signalled and
            # never polled.  Worth aiming at anyway: "signal succeeds, poll is
            # ACCESS_DENIED on the same slot" is the directional case.
            if ty % 4 == 1 and s < SPC and s not in notifs:
                notifs.append(s)
        elif r < 0.48:
            src = rng.choice(occupied)
            dst = rng.randrange(NSLOTS)
            ops.append("mint %d %d %d %d" % (
                src, dst, rng.choice(RIGHTS_POOL), rng.choice([0, 0, 0, 7, 42])))
            if dst not in occupied:
                occupied.append(dst)
        elif r < 0.56:
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
        elif r < 0.64:
            # D-13: a frame, a map through a slot, an unmap.  Mapping through
            # a DERIVED slot is the case that matters, so the source is drawn
            # from whatever is occupied rather than from slot 0.
            k = rng.random()
            if k < 0.34:
                dst = rng.randrange(SPC)
                ops.append("vframe %d" % dst)
                if dst not in occupied:
                    occupied.append(dst)
            elif k < 0.60:
                ops.append("vmap %d %d %d" % (rng.choice(occupied),
                                              rng.randrange(3),
                                              rng.randrange(2)))
            elif k < 0.80:
                ops.append("vunmap %d %d %d" % (rng.randrange(3),
                                                rng.randrange(3),
                                                rng.randrange(2)))
            else:
                # The TWO-SPACE unit: one frame mapped into both spaces, then
                # the authority revoked.  `kvspace_finalise_orphans` loops over
                # every space a frame is mapped into, and the defect that lived
                # in that loop could advance its window past a space never
                # examined — so "unmap it from every space" has to be a
                # different statement here from "unmap it", and separately
                # drawn ops almost never put one frame in both.
                src = rng.choice(occupied)
                ops.append("vmap %d %d 0" % (src, rng.randrange(3)))
                ops.append("vmap %d %d 1" % (src, rng.randrange(3)))
                if rng.random() < 0.6:
                    ops.append("revoke %d" % src)
                else:
                    ops.append("vunmap 0 %d %d" % (rng.randrange(3),
                                                   rng.randrange(2)))
        elif r < 0.69:
            # The DATA channel: signal bits through one capability, take them
            # through another.  Weighted towards an occupied slot in the root
            # CNode, because the interesting trace is the one where the slot
            # really holds a notification and the bits survive a derivation —
            # a generator drawing uniformly from 16 slots would spend its time
            # confirming NOT_FOUND.
            if notifs and rng.random() < 0.85:
                sid = rng.choice(notifs)
            elif rng.random() < 0.5:
                sid = rng.choice(occupied) % SPC
            else:
                sid = rng.randrange(SPC)
            k = rng.random()
            if k < 0.34:
                ops.append("nsig %d %d" % (sid, rng.randrange(4)))
            elif k < 0.68:
                ops.append("npoll %d" % sid)
            elif k < 0.84:
                # The ACCUMULATION case, also a unit: two different patterns
                # with no take between them.  Signalling OR-s into the word,
                # and a model that ASSIGNED instead survived 400 traces until
                # this branch existed — the two agree on every trace that
                # never signals twice in a row.  Patterns 2 and 0 are chosen
                # because 0x8000000000000000 | 1 differs from either alone.
                ops.append("nsig %d 2" % sid)
                ops.append("nsig %d 0" % sid)
                ops.append("npoll %d" % sid)
            else:
                # The exactly-once case, emitted as a UNIT so it survives the
                # draw: signal, take, and take again.  The second take is the
                # whole point — it must come back empty.
                ops.append("nsig %d %d" % (sid, rng.randrange(3)))
                ops.append("npoll %d" % sid)
                ops.append("npoll %d" % sid)
        elif r < 0.74:
            # the read-only queries: what a slot holds, and whether two
            # capabilities name one object
            if rng.random() < 0.5:
                ops.append("ident %d" % rng.randrange(SPC))
            else:
                ops.append("same %d %d" % (rng.randrange(SPC),
                                           rng.randrange(SPC)))
        elif r < 0.79:
            if rng.random() < 0.5:
                ops.append("srevoke %d" % (rng.choice(occupied) % SPC))
            else:
                ops.append("sdelete %d" % (rng.choice(occupied) % SPC))
        elif r < 0.82 and len(occupied) >= 2:
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
        elif r < 0.91:
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
        #
        # D-13's operations.  `None` means the driver took its `continue` and
        # printed NOTHING, which is a different observable from any error code
        # — so the model says so and no result is recorded, or the two sides
        # would compare a result against the next operation's.
        #
        #
        # The data channel.  Same `None` convention: the driver restricts both
        # to the root CNode and prints nothing outside it.
        #
        elif f[0] == "nsig":
            r = m.nsig(int(f[1]), int(f[2]))
            if r is not None:
                results.append(r)
        elif f[0] == "npoll":
            r = m.npoll(int(f[1]))
            if r is not None:
                results.append(r)
        elif f[0] == "vframe":
            r = m.vframe(int(f[1]))
            if r is not None:
                results.append(r)
        elif f[0] == "vmap":
            # The SPACE is optional and defaults to 0, exactly as the driver
            # reads it, so every two-argument `vmap` already recorded keeps its
            # meaning.
            r = m.vmap(int(f[1]), int(f[2]),
                       int(f[3]) if len(f) > 3 else 0)
            if r is not None:
                results.append(r)
        elif f[0] == "vunmap":
            r = m.vunmap(int(f[1]), int(f[2]),
                         int(f[3]) if len(f) > 3 else 0)
            if r is not None:
                results.append(r)
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
