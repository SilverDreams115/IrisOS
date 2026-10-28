#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
noninterference.py — what one subject can learn about another's actions.

Phase 5 asks for integrity, information flow and binary verification.
`spec/prove.py` carries integrity (nothing is granted) and authority
confinement (a revoke inside one subject leaves a disjoint subject's slots
alone).  Information flow was recorded as absent, with a reason: it needs a
notion of OBSERVATION — what a subject can SEE, not what it can reach — and
nothing in `spec/` modelled reading a message.

That reason was half right and it stopped too early.  A capability system
gives every subject one observation channel whether or not IPC is modelled:
**the result codes of its own operations.**  If what B does can change what A
is told, then A learns about B — and it learns it through the kernel, which is
exactly the channel a confinement theorem does not cover, because A's slots
can be untouched while A's ANSWERS differ.

So the property here is the standard one over observations:

    A's sequence of result codes is the same whether or not B ran at all.

Checked by running A's operations alone, then interleaving B's into them, and
requiring A's observations to be identical.  A subject is a SUBTREE of the
derivation forest — the same notion `prove_authority_confinement` uses, and
the only one this model can carry honestly.

What this still is NOT: a statement about data.  Nothing here models the
CONTENTS of a message, so a flow through what is sent rather than through what
is answered is outside it.  That bound is stated rather than left for a reader
to discover, because "non-interference, proved" is the kind of phrase that
stops people looking.
"""
import copy
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from authority_model import (AuthorityModel, R_READ, R_DUPLICATE, R_ALL,  # noqa: E402
                             MAPCAP_BASE)
from differential import gen_trace, run_model, NCNODES, SPC               # noqa: E402


def subtree(m, root):
    """Every slot at or below `root`, by the real parent relation."""
    out, frontier = {root}, {root}
    # BOUND: the slot space; each pass adds at least one slot or stops.
    while frontier:
        nxt = {i for i in m.slots
               if i < MAPCAP_BASE and m.slots[i].parent in frontier
               and i not in out}
        out |= nxt
        frontier = nxt
    return out


def two_subjects(m, rng):
    """Two DISJOINT subtrees, each with something in it, or None."""
    roots = [i for i in sorted(m.slots)
             if i < MAPCAP_BASE and not m.slots[i].empty
             and m.slots[i].parent is None]
    rng.shuffle(roots)
    for a in roots:
        sa = subtree(m, a)
        for b in roots:
            if b == a:
                continue
            sb = subtree(m, b)
            if sa & sb:
                continue
            return a, sa, b, sb
    return None


def ops_within(rng, slots, n):
    """Operations whose every slot argument lies inside one subject."""
    xs = sorted(slots)
    out = []
    for _ in range(n):
        k = rng.random()
        if k < 0.45 and len(xs) >= 2:
            s, d = rng.sample(xs, 2)
            out.append(("mint", s, d, rng.choice([R_READ, R_READ | R_DUPLICATE,
                                                  R_ALL]), 0))
        elif k < 0.75:
            out.append(("delete", rng.choice(xs)))
        else:
            out.append(("revoke", rng.choice(xs)))
    return out


def apply_op(m, op):
    if op[0] == "mint":
        return m.mint(op[1], op[2], op[3], op[4])
    if op[0] == "delete":
        return m.delete(op[1])
    return m.revoke(op[1])


def observations(m, a_ops, b_ops=()):
    """
    A's result codes, with B's operations interleaved.

    B's answers are DISCARDED: they are B's observations, not A's, and the
    claim is about what A is told.
    """
    seen = []
    bi = list(b_ops)
    for op in a_ops:
        # B acts between A's operations — the shape that matters, because a
        # subject that can only act before A starts cannot signal at all.
        if bi and random.Random(len(seen)).random() < 0.5:
            apply_op(m, bi.pop(0))
        seen.append(apply_op(m, op))
    for op in bi:
        apply_op(m, op)
    return seen


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rng = random.Random(0xF10FACE)
    bad = 0
    pairs = 0

    for t in range(n):
        base, _ = run_model(gen_trace(rng, rng.randint(6, 30)))
        got = two_subjects(base, rng)
        if got is None:
            continue
        a, sa, b, sb = got
        a_ops = ops_within(rng, sa, rng.randint(2, 6))
        b_ops = ops_within(rng, sb, rng.randint(1, 5))
        pairs += 1

        alone = observations(copy.deepcopy(base), a_ops)
        together = observations(copy.deepcopy(base), a_ops, b_ops)

        if alone != together:
            bad += 1
            if bad <= 5:
                print("noninterference: subject %d's answers changed when "
                      "subject %d acted" % (a, b))
                print("  alone      %s" % alone)
                print("  interleaved %s" % together)
                print("  A ops: %s" % (a_ops,))
                print("  B ops: %s" % (b_ops,))
        if bad > 5:
            print("... stopping after %d" % bad)
            break

    if bad:
        print("\nnoninterference: %d of %d subject pairs INTERFERED" % (bad, pairs))
        return 1
    print("noninterference: %d disjoint subject pairs over %d states — a "
          "subject's own result codes are identical whether or not the other "
          "subject ran, so nothing it is TOLD depends on what the other did. "
          "Flows through message CONTENTS are outside this: nothing here "
          "models what is sent." % (pairs, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
