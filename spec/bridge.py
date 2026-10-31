#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
bridge.py — the link between what is PROVED and what is TESTED.

Stage V phase 4's first real step.  Until this file existed, `spec/` had two
formalisations of the same invariants and no lane comparing them:

    spec/authority_model.py   its own checkers, driven through the real
                              kcnode.c/cspace.c over thousands of traces
    spec/prove.py             its own predicates, proved by z3 over every
                              symbolic state

Both green, and the chain between them **broken in the middle**.  The proved
predicates were never run against a state the kernel produced; the tested
checkers were never proved.  Nothing would have noticed the two drifting
apart, and the proved side is the side no hardware ever executes — which is
the worse of the two to have wrong, because it is the one that looks strongest.

`spec/invariants.py` now holds the predicates once.  `prove.py` instantiates
them symbolically and proves them.  This file instantiates the SAME
definitions concretely, on states that real traces built, and requires that
they agree with `authority_model.py`'s independently written checkers on every
one.  The chain becomes:

    invariants.py  ── proved by z3 ──────────────>  holds for every state
         │
         └──────── agrees with the model's checkers, here, over N traces
                        │
                        └──── the model agrees with the C, spec-differential

The last link is still traces.  That is phase 5 and it is said so here rather
than left for a reader to assume the chain is tighter than it is.

    usage:  python3 spec/bridge.py [ntraces]      (`make spec-bridge`)
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import invariants                                            # noqa: E402
from authority_model import (AuthorityModel, ALL_CHECKS,      # noqa: E402
                             chk_mapping_authority, MAPCAP_BASE,
                             NVSPACES,
                             chk_rights_monotone, chk_badge_sealed,
                             chk_derivation_same_object)
from differential import gen_trace, run_model, NCNODES, SPC   # noqa: E402

L = invariants.CON()

# Which model checker each shared predicate is supposed to BE.  A predicate
# with no counterpart would be a predicate nothing validates, so the mapping
# is exhaustive over `invariants.ALL` by construction below.
PAIRS = (
    ("A7", invariants.inv_A7, chk_rights_monotone),
    ("A8", invariants.inv_A8, chk_badge_sealed),
    ("O2", invariants.inv_O2, chk_derivation_same_object),
)


def to_arrays(m):
    """`AuthorityModel` -> the five arrays the shared predicates read."""
    obj = invariants.Defaulting(-1)
    rgt = invariants.Defaulting(0)
    par = invariants.Defaulting(-1)
    bdg = invariants.Defaulting(0)
    oty = invariants.Defaulting(-1)
    for sid, s in m.slots.items():
        if s.empty:
            continue
        obj[sid] = s.obj
        rgt[sid] = s.rights
        par[sid] = -1 if s.parent is None else s.parent
        bdg[sid] = s.badge
    for oid, o in m.objs.items():
        oty[oid] = AuthorityModel.TYPE_WIRE[o.type]
    return invariants.State(obj, rgt, par, bdg, oty, lambda i: i >= 0)


def holds(pred, st, slots):
    """The predicate, over every slot of this state."""
    for sid in slots:
        if not pred(L, st, sid):
            return sid
    return None


def wrong_A7(L_, st, i):
    """
    A7 with the reduction inverted — `rgt[i] & rgt[par] == rgt[par]` asks
    that the CHILD keep everything the parent had, which is the amplifying
    direction and is false of any real derivation that narrows rights.
    """
    obj, rgt, par, _, _ = st
    return L_.IMPLIES(L_.AND(L_.NE(obj[i], -1), L_.NE(par[i], -1),
                             L_.EQ(obj[par[i]], obj[i])),
                      L_.EQ(L_.AND_BITS(rgt[i], rgt[par[i]]), rgt[par[i]]))


def the_lane_can_fail(rng):
    """
    A comparison that cannot disagree is not a comparison.

    The sabotage is done here rather than trusted: a deliberately wrong A7 is
    run over real traces and must be CAUGHT.  If it is not — because the
    traces are too shallow, or the states too empty, or the comparison was
    quietly short-circuited — every green line below means nothing, and the
    lane says so instead of passing.
    """
    for _ in range(200):
        ops = gen_trace(rng, rng.randint(4, 40))
        m, _ = run_model(ops)
        st = to_arrays(m)
        slots = sorted(m.slots)
        if holds(wrong_A7, st, slots) is not None and \
           chk_rights_monotone(m) is None:
            return True
    return False


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 400
    rng = random.Random(0xB41D9E)

    if not the_lane_can_fail(random.Random(0x5AB07)):
        print("bridge: a deliberately WRONG A7 was not caught — the "
              "comparison is not comparing, and nothing below is evidence")
        return 1

    disagreements = 0
    unreachable_seen = 0
    states = 0

    for t in range(n):
        ops = gen_trace(rng, rng.randint(4, 40))
        m, _ = run_model(ops)
        st = to_arrays(m)
        slots = sorted(m.slots)
        states += 1

        # 1. the shared predicates against the model's own checkers.
        for name, pred, checker in PAIRS:
            bad_slot = holds(pred, st, slots)
            complaint = checker(m)
            if (bad_slot is None) != (complaint is None):
                disagreements += 1
                if disagreements <= 5:
                    print("bridge: trace %d — %s: predicate says %s, "
                          "model checker says %s"
                          % (t, name,
                             "violated at slot %s" % bad_slot
                             if bad_slot is not None else "holds",
                             complaint or "holds"))
                    print("  trace: " + " ; ".join(ops))

        # 2. well-formedness, which the model has no single checker for —
        #    it is spread across the "parent that is empty" arm of A7 and the
        #    symmetry check — so it is compared against the conjunction.
        bad_slot = holds(invariants.wf, st, slots)
        if bad_slot is not None:
            disagreements += 1
            if disagreements <= 5:
                print("bridge: trace %d — wf violated at slot %d on a state "
                      "the model built" % (t, bad_slot))

        # 3. the asymmetry `invariants.py` documents, CHECKED rather than
        #    asserted: `chk_rights_monotone` skips an edge when the parent is
        #    an Untyped and the objects differ, while `inv_A7` applies only
        #    when the objects are the SAME.  They differ on exactly one shape
        #    — a non-Untyped parent whose child names another object — which
        #    violates O2 and should therefore never occur.  "Should" is what
        #    this lane exists to stop taking on trust.
        for sid, s in m.slots.items():
            if s.empty or s.parent is None:
                continue
            p = m.slots[s.parent]
            if p.empty:
                continue
            if s.obj != p.obj and m.objs[p.obj].type != "ut":
                unreachable_seen += 1

        # 4b. D-13, the two formulations against each other.
        #
        #     `inv_D13` reads the pseudo-slot's occupancy; `chk_mapping_
        #     authority` starts from what is MAPPED and searches the slot space
        #     for a capability naming it.  Same invariant, opposite directions,
        #     and the states are the ones real traces built — which now include
        #     mappings, because the generator's mapping ops are reachable.
        #
        #     PER SPACE, and this lane is what forced that.  `inv_D13` has
        #     taken a (space, object) pair since it was written, but these two
        #     adapters ignored the space and read `MAPCAP_BASE + ob` — space
        #     0's pseudo-slot — while the model and `chk_mapping_authority`
        #     were widened to two spaces.  A frame mapped only in space 1 then
        #     had `maps` true and `hascap` false, so the predicate said
        #     VIOLATED while the checker said holds: 3 disagreements over 400
        #     states, on the first gate run after the second space landed.
        #     That is exactly the drift `spec-bridge` exists to catch, found in
        #     the direction that matters — one formulation updated and the
        #     other not — and it was caught by the lane and not by reading.
        #
        CON = invariants.CON()
        def _maps(sp, ob):
            return ob in m.maps and any(k == sp for k, _ in m.maps[ob])
        def _hascap(sp, ob):
            mc = m._mapcap(ob, sp)
            return mc in m.slots and not m.slots[mc].empty
        pred_bad = None
        for fi in range(len(m.frames)):
            for sp in range(NVSPACES):
                if not invariants.inv_D13(CON, _maps, _hascap, sp, fi):
                    pred_bad = "%d in space %d" % (fi, sp)
                    break
            if pred_bad is not None:
                break
        checker_bad = chk_mapping_authority(m)
        if (pred_bad is None) != (checker_bad is None):
            disagreements += 1
            if disagreements <= 5:
                print("bridge: trace %d — D13: predicate says %s, model "
                      "checker says %s"
                      % (t,
                         "violated for frame %s" % pred_bad
                         if pred_bad is not None else "holds",
                         checker_bad or "holds"))
                print("  trace: " + " ; ".join(ops))

        # 4. and every charter check the model carries must hold, because a
        #    state that already violates one makes the comparison above
        #    meaningless rather than reassuring.
        for chk in ALL_CHECKS:
            why = chk(m)
            if why:
                print("bridge: trace %d — the MODEL violates %s: %s"
                      % (t, chk.__name__, why))
                print("  trace: " + " ; ".join(ops))
                disagreements += 1
                break

    if unreachable_seen:
        print("bridge: the shape documented as unreachable occurred %d time(s)"
              " — invariants.py's asymmetry note is WRONG" % unreachable_seen)
        disagreements += 1

    if disagreements:
        print("\nbridge: %d disagreement(s) over %d states" %
              (disagreements, states))
        return 1

    print("bridge: %d states from real traces; the PROVED predicates and the "
          "model's own checkers agree on every one, the one shape where they "
          "could differ never occurred, and a wrong A7 is still caught"
          % states)
    return 0


if __name__ == "__main__":
    sys.exit(main())
