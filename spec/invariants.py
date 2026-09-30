#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
invariants.py — the CSpace invariants, written ONCE, read two ways.

Stage V phase 4's first link, and it closes a gap the rest of `spec/` had
left open in plain sight.  Before this file there were two formalisations of
the same invariants:

    spec/authority_model.py   chk_rights_monotone, chk_badge_sealed, ...
                              VALIDATED against the real C over N traces
    spec/prove.py             inv_A7_at, inv_A8_at, inv_O2_at, ...
                              PROVED over symbolic state

and **nothing checked that they were the same invariants**.  One was proved
and the other was tested, and the chain joining a proof to a kernel had a
missing link exactly where it mattered most — the two could have drifted
without any lane noticing, and the proved one is the one nobody runs against
hardware.

So the predicates live here, written against a tiny logic backend, and both
sides import them:

  * `SYM` builds z3 expressions over arrays          -> `spec/prove.py`
  * `CON` evaluates Python bools over dicts          -> `spec/bridge.py`

`spec/bridge.py` then drives real traces through `AuthorityModel`, evaluates
THESE predicates concretely on the resulting state, and requires them to agree
with the model's own checkers.  That makes the chain:

    proved here  ==  checked here against the model  ==  agrees with the C

with the last link still being traces rather than a refinement proof, which
is phase 5 and is said so everywhere it matters.

── One deliberate asymmetry, found by writing this file ────────────────────

`chk_rights_monotone` skips an edge when the parent is an Untyped AND the
objects differ — a creation.  The predicate here applies only when the objects
are the SAME.  Those differ on exactly one shape: a parent that is NOT an
Untyped whose child names a DIFFERENT object.  That state violates O2, so it
is unreachable in any state the model can build, and the two agree wherever
the question is meaningful.  `bridge.py` checks that claim rather than
asserting it, because "unreachable" was a claim about the model and this file
exists because such claims were going unchecked.
"""

T_UNTYPED = 11                   # the wire type, as `TYPE_WIRE` records it


class State(tuple):
    """
    The five arrays, plus the domain predicate.

    `is_slot` travels WITH the state because forgetting it is what produced
    three of this project's four solver counterexamples: `-1` is the sentinel
    for "empty" and for "no parent", it is not an index, and a predicate that
    does not know where its domain ends will be handed the outside of it.
    """
    def __new__(cls, obj, rgt, par, bdg, oty, is_slot):
        st = super().__new__(cls, (obj, rgt, par, bdg, oty))
        st.is_slot = is_slot
        return st


class Defaulting(dict):
    """A concrete array: missing cells read as `default` rather than raising."""
    def __init__(self, default):
        super().__init__()
        self.default = default

    def __missing__(self, k):
        return self.default


class SYM:
    """z3 expressions."""
    def __init__(self, z3):
        self.z3 = z3

    def AND(self, *a):
        return self.z3.And(*a) if len(a) > 1 else a[0]

    def OR(self, *a):
        return self.z3.Or(*a) if len(a) > 1 else a[0]

    def IMPLIES(self, a, b):
        return self.z3.Implies(a, b)

    def NOT(self, a):
        return self.z3.Not(a)

    def EQ(self, a, b):
        return a == b

    def NE(self, a, b):
        return a != b

    def AND_BITS(self, a, b):
        return a & b


class CON:
    """Python bools over concrete values."""
    def AND(self, *a):
        return all(a)

    def OR(self, *a):
        return any(a)

    def IMPLIES(self, a, b):
        return (not a) or b

    def NOT(self, a):
        return not a

    def EQ(self, a, b):
        return a == b

    def NE(self, a, b):
        return a != b

    def AND_BITS(self, a, b):
        return a & b


def wf(L, st, i):
    """
    A parent link is `-1`, or it names a real, OCCUPIED slot.

    Both halves earned their place: an earlier version said only "occupied",
    which let a proof read `obj[-2]` — a cell no quantifier constrained — and
    produce a counterexample that was about the encoding and not about IRIS.
    """
    obj, _, par, _, _ = st
    return L.IMPLIES(L.NE(obj[i], -1),
                     L.OR(L.EQ(par[i], -1),
                          L.AND(st.is_slot(par[i]), L.NE(obj[par[i]], -1))))


def inv_A7(L, st, i):
    """
    A7 — a DERIVED capability's rights are a subset of its parent's.

    "Derived" is load-bearing: a retyped object's rights are BORN, assigned by
    type, and a notification is given WAIT by an Untyped that has none.  An
    edge to an Untyped is a creation and A7 says nothing about it.
    """
    obj, rgt, par, _, _ = st
    return L.IMPLIES(L.AND(L.NE(obj[i], -1), L.NE(par[i], -1),
                           L.EQ(obj[par[i]], obj[i])),
                     L.EQ(L.AND_BITS(rgt[i], rgt[par[i]]), rgt[i]))


def inv_A8(L, st, i):
    """A8 — a badged capability is never re-badged by a derivation."""
    obj, _, par, bdg, _ = st
    return L.IMPLIES(L.AND(L.NE(obj[i], -1), L.NE(par[i], -1),
                           L.EQ(obj[par[i]], obj[i]), L.NE(bdg[par[i]], 0)),
                     L.EQ(bdg[i], bdg[par[i]]))


def inv_O2(L, st, i):
    """
    O2 — every MDB edge is a DERIVATION (parent and child name one object) or
    a CREATION from an Untyped.  There is no third kind of edge.

    Structural, and therefore decidable.  An annotation in the theory once
    called O2 "untyped region arithmetic, not modellable"; that is what O2 is
    about and not what the definition says — no base, no size, no cursor
    appears in it or in `authority_model.py`.
    """
    obj, _, par, _, oty = st
    return L.IMPLIES(L.AND(L.NE(obj[i], -1), L.NE(par[i], -1),
                           L.NE(obj[par[i]], -1)),
                     L.OR(L.EQ(obj[i], obj[par[i]]),
                          L.EQ(oty[obj[par[i]]], T_UNTYPED)))


ALL = (("wf", wf), ("A7", inv_A7), ("A8", inv_A8), ("O2", inv_O2))
