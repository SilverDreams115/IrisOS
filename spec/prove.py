#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
prove.py — machine-checked proofs of the authority model's obligations.

Stage V, the step between phase 3 and phase 4.  `spec/isabelle/` states the
obligations; this DISCHARGES the ones an SMT solver can reach, and it reaches
more than it looks: the whole rights algebra over every 32-bit value, and
one-step preservation of the invariants over SYMBOLIC state — every CSpace,
not every CSpace a random trace happened to build.

    exhaustion  (`spec/rights_model.py`)  checks 32896 concrete points
    traces      (`spec/differential.py`)  checks thousands of concrete states
    THIS                                  checks all of them at once

── What a proof here means, and what it does not ───────────────────────────

It means: for every state satisfying the invariant and the operation's
precondition, the state after the operation satisfies the invariant.  That is
the shape of a refinement proof's inductive step, and it is the thing neither
testing nor exhaustion can give.

It does NOT mean the C is correct.  It proves the MODEL, and the model's
connection to the C is `spec/differential.py` — thousands of traces where the
two agree on states and return codes.  Model proved plus model-matches-C
tested is weaker than seL4's model proved plus C-refines-model proved, and
saying which of the two IRIS has is the whole point of this file existing.

── Running it ───────────────────────────────────────────────────────────────

Needs `z3`.  `make spec-prove` finds it if it is importable; otherwise it
says so and returns 0, because a machine without a prover should not fail a
build — it should say that this evidence is missing on it.
"""

import os
import sys

try:
    import z3
except ImportError:
    print("spec-prove: z3 is not importable on this machine, so none of the "
          "obligations below were checked here.\n"
          "            They are STATED in spec/isabelle/IRIS_Authority.thy "
          "and discharged wherever z3 exists.")
    sys.exit(0)

W = 32                       # iris_rights_t is 32 bits
SAME_RIGHTS = 1 << 31

R_READ, R_WRITE, R_DUPLICATE, R_TRANSFER = 1, 2, 4, 8
R_WAIT, R_ROUTE, R_MANAGE = 16, 32, 64
MASK = (R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER |
        R_WAIT | R_ROUTE | R_MANAGE)

results = []


_PREMISE_CACHE = {}


def premises_satisfiable(extra, witness=None):
    """
    Are these premises satisfiable, and is the answer worth caching?

    It is: `move` states five theorems over ONE premise set, so without a
    cache the same query runs five times — and each run is a chance for the
    solver to time out and report the premises undecided, which fails the
    lane.  That is exactly what it did, intermittently, on the commit that
    added those five.  A gate that flakes is a gate people rerun until it is
    green, which is worse than not having it.

    Keyed on the assertions' printed form, which is exact for the way this
    file builds them: the same premise list produces the same key.
    """
    key = "\u0000".join(a.sexpr() for a in extra)
    if key in _PREMISE_CACHE:
        return _PREMISE_CACHE[key]
    #
    # A WITNESS turns the question around.
    #
    # "Are these premises satisfiable" is a search, and for `rotate` — two
    # quantifiers over arrays plus three distinct slots — the solver does not
    # finish it, so all five of its theorems came back PREMISE? and failed the
    # lane.  Exhibiting a state that satisfies them is a GROUND check instead,
    # and it is also better evidence: a model the reader can look at beats
    # "the solver said yes".
    #
    if witness is not None:
        _, r = decide(list(extra) + list(witness), timeout=180000)
        if r == z3.sat:
            _PREMISE_CACHE[key] = z3.sat
            return z3.sat
        # A witness that does not satisfy the premises is a bug in the
        # WITNESS, and saying "unknown" would hide it behind the hard query.
        if r == z3.unsat:
            print("  (witness rejected — it does not satisfy the premises)")
    _, r = decide(extra, timeout=180000)
    _PREMISE_CACHE[key] = r
    return r


def decide(assertions, timeout=60000):
    """
    Ask z3, and if the default engine cannot decide, ask it differently.

    Quantifiers over arrays put these queries on the edge of what is
    decidable, and the default model-based instantiation gives up on some of
    them where a config without MBQI answers immediately.  Both are asked
    before anything is reported as undecided.

    Only `unsat` is ever relied on for a PROOF, and `unsat` is sound under
    either configuration: it means a contradiction was derived, not that a
    search finished.  A `sat` is used only to show premises are not
    contradictory, and it comes with the model that shows it.
    """
    for cfg in ({}, {"smt.mbqi": False}):
        s = z3.Solver()
        s.set("timeout", timeout)
        for k, v in cfg.items():
            s.set(k, v)
        for a in assertions:
            s.add(a)
        r = s.check()
        if r != z3.unknown:
            return s, r
    return s, z3.unknown


def prove(name, claim, extra=None, witness=None):
    """
    A claim holds when its NEGATION is unsatisfiable.

    And before believing that: the PREMISES are checked satisfiable.  A
    theorem over contradictory premises is `unsat` for the wrong reason and
    prints exactly like a real proof — it is the single way a file like this
    can claim thirteen results and mean none of them.  A premise set the
    solver finds unsatisfiable is reported VACUOUS and counted as a failure,
    because a precondition no state can meet is a bug in the specification
    whichever way it was reached.

    Reported as PROVED / REFUTED / VACUOUS / UNKNOWN, and `unknown` is printed
    as what it is — the solver gave up — rather than folded into either.
    """
    if extra:
        r = premises_satisfiable(extra, witness)
        if r == z3.unsat:
            results.append((name, "VACUOUS", None))
            return
        if r != z3.sat:
            # NOT the same as vacuous, and the first version of this file
            # conflated them: it reported any non-`sat` premise check as
            # VACUOUS, so A9 — whose premises are perfectly satisfiable and
            # merely hard for the default engine — was printed as a
            # specification bug.  A solver that cannot decide is not a
            # specification that cannot hold.
            results.append((name, "PREMISE?", None))
            return

    s, r = decide((list(extra) if extra else []) + [z3.Not(claim)])
    if r == z3.unsat:
        results.append((name, "PROVED", None))
    elif r == z3.sat:
        results.append((name, "REFUTED", s.model()))
    else:
        results.append((name, "UNKNOWN", None))


def prove_the_harness_can_fail():
    """
    A prover that cannot refute is a prover that proves everything.

    Two claims that are FALSE about IRIS's algebra, stated here on purpose:
    if either comes back PROVED, every other line in this file is worthless
    and the run must fail.  This is the check that `make spec-prove` is
    testing the kernel's model and not the solver's willingness to agree.
    """
    b = z3.BitVec("nb", W)
    r = z3.BitVec("nr", W)
    h = z3.BitVec("nh", W)

    s = z3.Solver()
    s.add(z3.Not(z3.Implies(b != 0, rights_reduce(b, r) != 0)))
    amp = s.check()               # a reduction CAN empty a non-empty base

    s2 = z3.Solver()
    s2.add(z3.Not(rights_check(h, 0)))
    emp = s2.check()              # check(h, 0) is FALSE, so `Not` is SAT

    ok = (amp == z3.sat and emp == z3.sat)
    results.append(("--  the harness can still refute a false claim",
                    "PROVED" if ok else "REFUTED", None))


# ── the rights algebra, over every 32-bit value ─────────────────────────────
#
# `spec/rights_model.py` enumerates the 7-bit space.  This proves the same
# properties for the whole word, which is what `iris_rights_t` actually is —
# so a right added in bit 8 tomorrow does not quietly leave the checked
# region.

def rights_reduce(base, req):
    return z3.If(req & SAME_RIGHTS != 0, base & ~z3.BitVecVal(SAME_RIGHTS, W),
                 base & req)


def rights_check(have, need):
    return z3.And(need != 0, (have & need) == need)


def prove_rights():
    b = z3.BitVec("base", W)
    r = z3.BitVec("req", W)
    h = z3.BitVec("have", W)
    n = z3.BitVec("need", W)

    # A7 itself: a derivation never amplifies.
    prove("A7  reduce never amplifies",
          rights_reduce(b, r) & b == rights_reduce(b, r))
    # Unguarded, and it was guarded with `r AND SAME_RIGHTS = 0` for no reason:
    # if `r` carries the sentinel then reduce gives `b AND NOT SAME`, and
    # reducing THAT by `r` again clears a bit already clear.  z3 decides the
    # general claim, so the guard only narrowed what was being proved.
    prove("    reduce is idempotent, sentinel or not",
          rights_reduce(rights_reduce(b, r), r) == rights_reduce(b, r))
    # This one's guard is load-bearing and stays: see the obligation below.
    prove("    reduce is commutative where neither side is the sentinel",
          rights_reduce(b, r) == rights_reduce(r, b),
          extra=[b & SAME_RIGHTS == 0, r & SAME_RIGHTS == 0])
    #
    # And commutativity WITHOUT that guard must stay refutable.
    #
    # `spec/isabelle/IRIS_Authority.thy` defines `rights_reduce` as plain
    # intersection — the sentinel is outside its 7-bit `rights` type — and
    # states `rights_reduce_commutative` unguarded.  That lemma is provable in
    # that theory and FALSE of the kernel, so proving it there would produce a
    # certificate for a property IRIS does not have.  The theory now says so at
    # the definition; this is the row that NOTICES if the guard is ever dropped
    # here, or if `rights_reduce` is ever "simplified" into intersection.
    _c = z3.Solver()
    _c.add(z3.Not(rights_reduce(b, r) == rights_reduce(r, b)))
    results.append(("--  unguarded commutativity is still refutable "
                    "(the Isabelle lemma's scope)",
                    "PROVED" if _c.check() == z3.sat else "REFUTED", None))
    prove("    SAME_RIGHTS never survives into a stored value",
          rights_reduce(b, z3.BitVecVal(SAME_RIGHTS, W)) & SAME_RIGHTS == 0)
    prove("    a copy keeps exactly what the source had",
          rights_reduce(b, z3.BitVecVal(SAME_RIGHTS, W)) ==
          b & ~z3.BitVecVal(SAME_RIGHTS, W))
    prove("    check is reflexive except at the empty requirement",
          z3.Implies(h != 0, rights_check(h, h)))
    prove("    the empty requirement is never satisfied",
          z3.Not(rights_check(h, 0)))
    prove("    check is monotone in what the holder has",
          z3.Implies(z3.And(rights_check(h, n), True),
                     rights_check(h | b, n)))
    prove("    a reduction satisfies no right its base lacked",
          z3.Implies(z3.And(r & SAME_RIGHTS == 0, n != 0,
                            rights_check(rights_reduce(b, r), n)),
                     rights_check(b, n)))


# ── the CSpace invariants, over symbolic state ──────────────────────────────
#
# Slots are arrays: `obj` (-1 = empty), `rights`, `parent` (-1 = root),
# `badge`.  That is the state `spec/authority_model.py` keeps and
# `spec/isabelle/IRIS_Authority.thy` declares, in the form a solver can
# reason about.

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import invariants                                            # noqa: E402

L = invariants.SYM(z3)
I = z3.IntSort()
BV = z3.BitVecSort(W)


T_UNTYPED = 11                   # the wire type, as `TYPE_WIRE` records it


def new_state(tag):
    """
    Slots as arrays, plus `oty` — the type of each OBJECT, indexed by object
    id and not by slot, because that is where a type lives: two slots naming
    one object name one type.
    """
    return invariants.State(z3.Array("obj_" + tag, I, I),
                            z3.Array("rgt_" + tag, I, BV),
                            z3.Array("par_" + tag, I, I),
                            z3.Array("bdg_" + tag, I, BV),
                            z3.Array("oty_" + tag, I, I),
                            slot)


def slot(i):
    """
    `-1` is the sentinel for "empty" in `obj` and "no parent" in `par`.  It is
    not a slot index, and the first version of this file let the quantifiers
    range over it — which produced two counterexamples to the delete theorem
    where the victim WAS the sentinel, so "reparent every child of the victim"
    reparented every ROOT.  Artifacts of the encoding, not of the kernel, and
    indistinguishable from a real refutation until read.

    It cost a second refutation before the lesson took.  `wf` said a parent
    link points at an OCCUPIED slot and never said it points at a SLOT, so
    the solver gave a capability the parent -2, read `obj[-2]` — a cell no
    quantifier constrained — and broke O2 with it.  Both times the gap was
    the domain, not the kernel, and both times the counterexample was
    indistinguishable from a real one until it was read.  That is the
    argument for printing them in full rather than printing a count.

    Stating the domain is what makes the difference legible: with this, a
    REFUTED row is about IRIS.
    """
    return i >= 0


def wf(st, i):
    """A parent link points at an occupied slot. Stated per-slot for the
    quantifier below."""
    return invariants.wf(L, st, i)

def inv_A7_at(st, i):
    """Derivation edges only — an edge to an Untyped is a CREATION, and the
    charter says 'mint never amplifies', so it is outside this."""
    return invariants.inv_A7(L, st, i)

def inv_A8_at(st, i):
    return invariants.inv_A8(L, st, i)

def inv_O2_at(st, i):
    """
    O2, as the theory states it and as the model implements it: every MDB edge
    is either a DERIVATION (parent and child name the same object) or a
    CREATION from an Untyped.  There is no third kind of edge.

    The first annotation this file carried said O2 was "untyped region
    arithmetic, not modelled here".  That is what O2 is ABOUT and not what the
    specification says: `spec/authority_model.py` carries no base, no size and
    no cursor, and `inv_O2` in the theory is a statement about the shape of
    the tree.  Structural, therefore decidable, therefore no excuse.
    """
    return invariants.inv_O2(L, st, i)

def prove_derive_preserves():
    """
    One-step preservation, the inductive step of a refinement proof.

    For EVERY state satisfying the invariants and every legal derive, the
    state afterwards satisfies them.  Not "every state a trace reached" —
    every state, including ones no generator would build.

    ── Why the badge is symbolic and not `bdg[src]` ────────────────────────

    The first version of this stored `bdg[src]` into the destination, which
    is `mdb_badge_derive`'s FIRST case — a zero request inherits — and called
    the result "every derivation".  It is one of three.  The model
    (`spec/authority_model.py`, and the kernel it agrees with) also permits a
    non-zero request equal to the source's badge, and permits stamping a
    FRESH badge when the source is unbadged and the object is an identity
    type.  A theorem proved over one case and labelled with all three is an
    overclaim, and the fact that the other two also preserve A8 does not make
    the label true before it is checked.

    So the new badge is a variable constrained by the whole rule, and
    `is_identity` is left UNINTERPRETED: the proof holds for any set of
    identity types, which is stronger than proving it for the two IRIS has
    and cannot go stale when a third is added.
    """
    obj, rgt, par, bdg, oty = new_state("0")
    src, dst = z3.Int("src"), z3.Int("dst")
    req = z3.BitVec("req", W)
    req_badge = z3.BitVec("req_badge", W)
    nb = z3.BitVec("new_badge", W)
    is_identity = z3.Function("is_identity", I, z3.BoolSort())
    i = z3.Int("i")

    st = invariants.State(obj, rgt, par, bdg, oty, slot)
    pre = [
        z3.ForAll([i], z3.Implies(slot(i),
                                  z3.And(wf(st, i), inv_A7_at(st, i),
                                         inv_A8_at(st, i), inv_O2_at(st, i)))),
        slot(src), slot(dst), src != dst,
        obj[src] != -1,                                   # source occupied
        obj[dst] == -1,                                   # destination empty
        rgt[src] & R_DUPLICATE != 0,                      # may be copied
        rgt[src] & req != 0,                              # does not collapse
        req & SAME_RIGHTS == 0,
        # mdb_badge_derive, in full: inherit / keep / stamp.  Anything else
        # is a REFUSAL and so never reaches a post-state at all.
        z3.If(req_badge == 0,
              nb == bdg[src],
              z3.If(bdg[src] != 0,
                    z3.And(req_badge == bdg[src], nb == bdg[src]),
                    z3.And(is_identity(obj[src]), nb == req_badge))),
    ]

    # the post-state: the destination gains the source's object, the reduced
    # rights, the derived badge and the source as its MDB parent.  Nothing
    # else moves.
    post = invariants.State(z3.Store(obj, dst, obj[src]),
                            z3.Store(rgt, dst, rgt[src] & req),
                            z3.Store(par, dst, src),
                            z3.Store(bdg, dst, nb),
                            oty, slot)

    j = z3.Int("j")
    prove("A7  derive preserves it, for every state and every derivation",
          z3.ForAll([j], z3.Implies(slot(j), inv_A7_at(post, j))), extra=pre)
    prove("A8  derive preserves it, for every state and every derivation",
          z3.ForAll([j], z3.Implies(slot(j), inv_A8_at(post, j))), extra=pre)
    prove("    derive preserves well-formedness",
          z3.ForAll([j], z3.Implies(slot(j), wf(post, j))), extra=pre)
    prove("O2  derive preserves it — a copy is a derivation edge, never a new kind",
          z3.ForAll([j], z3.Implies(slot(j), inv_O2_at(post, j))), extra=pre)
    prove("    a derived capability never outranks its source",
          rgt[src] & req & rgt[src] == rgt[src] & req, extra=pre)


def prove_delete_preserves():
    """
    Delete, which is where an MDB gets subtle: emptying a slot that still has
    CHILDREN would leave them naming a hole, and `wf` is exactly the property
    that forbids it.

    IRIS reparents — a deleted node's children are adopted by its parent —
    so the theorem is that adoption preserves A7 and A8, and THAT is not
    obvious: it makes a new edge between two capabilities that were never
    directly related, and A7 has to survive it.  It does, because rights
    shrink along every edge and shrinking is transitive; the solver is what
    turns "because transitive" into a result.
    """
    obj, rgt, par, bdg, oty = new_state("d")
    victim = z3.Int("victim")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i),
                                  z3.And(wf(st, i), inv_A7_at(st, i),
                                         inv_A8_at(st, i), inv_O2_at(st, i)))),
        slot(victim),
        obj[victim] != -1,
        par[victim] != -1,                  # has a parent to adopt its children
        obj[par[victim]] == obj[victim],    # a derivation edge, so A7/A8 apply
        par[par[victim]] != victim,         # the tree is a tree
    ]

    # every child of the victim is adopted by the victim's parent, and the
    # victim's slot is emptied.
    k = z3.Int("k")
    par2 = z3.Lambda([k], z3.If(par[k] == victim, par[victim], par[k]))
    obj2 = z3.Store(obj, victim, -1)
    post = invariants.State(obj2, rgt, par2, bdg, oty, slot)

    j = z3.Int("j")
    prove("A7  delete-with-reparent preserves it",
          z3.ForAll([j], z3.Implies(z3.And(slot(j), j != victim),
                                    inv_A7_at(post, j))),
          extra=pre)
    prove("A8  delete-with-reparent preserves it",
          z3.ForAll([j], z3.Implies(z3.And(slot(j), j != victim),
                                    inv_A8_at(post, j))),
          extra=pre)
    prove("O2  delete-with-reparent preserves it",
          z3.ForAll([j], z3.Implies(z3.And(slot(j), j != victim),
                                    inv_O2_at(post, j))), extra=pre)
    prove("    delete-with-reparent leaves no child naming an empty slot",
          z3.ForAll([j], z3.Implies(z3.And(slot(j), j != victim),
                                    wf(post, j))), extra=pre)


ACYCLIC_DEPTH = 3


def acyclic_within(par, i, d=None):
    """
    No slot is its own ancestor within `d` steps — and the walk STOPS at the
    no-parent sentinel.

    The version without the `live` guard walked through `-1` into `par[-1]`,
    a cell nothing constrains, and reported the cycle `0 -> -1 -> 3 -> 2 -> 4
    -> 0`: five links, one of which is the end of the chain.  Third
    counterexample in this file caused by the encoding's domain rather than
    by IRIS, and the third is what makes it a pattern worth naming — a
    sentinel is not a value, and every walk over a partial map has to say
    where it ends or the solver will happily continue past it.
    """
    d = ACYCLIC_DEPTH if d is None else d
    conj = []
    live = z3.BoolVal(True)
    cur = par[i]
    for _ in range(d):
        conj.append(z3.Implies(live, cur != i))
        live = z3.And(live, cur != -1)
        cur = par[cur]
    return z3.And(*conj)


def prove_delete_preserves_A9():
    """
    A9 — the derivation tree is acyclic — under the operation most likely to
    break it, and stated in the only form that is both true and provable here.

    ── The obvious statement is FALSE, and finding out why is the result ────

    "acyclic to depth 5 before implies acyclic to depth 5 after" is refutable,
    and the solver refuted it: the pre-state holds the 6-cycle
    0 -> 4 -> 5 -> 1 -> 2 -> 3 -> 0, which a depth-5 premise happily admits,
    and deleting slot 1 reparents 5 onto 2 and pulls that cycle down to length
    5, where the check now sees it.  No cycle was created.  One was shortened
    into view.

    That is not a kernel defect and it is not an encoding artifact either —
    it is the fact that **a bounded acyclicity invariant is not inductive
    under an operation that shortens paths**, which is precisely what
    reparenting is.  Bounding the depth of a safety property is usually
    harmless; bounding it here changes what the property means.

    ── So the theorem is the relative one ──────────────────────────────────

    Every reparented edge is either one original edge or exactly two of them
    (`par2[x] = par[par[x]]` when x was a child of the victim), so a cycle of
    length L after lifts to a cycle of length at most 2L before.  The theorem
    proved is therefore:

        no cycle of length <= %d before  =>  no cycle of length <= %d after

    which says **delete-with-reparent introduces no cycle that was not
    already there**.  That is the inductive content of A9; the step from it to
    unbounded acyclicity is an induction over the tree, and it stays owed to
    Isabelle where it was always owed.
    """ % (2 * ACYCLIC_DEPTH, ACYCLIC_DEPTH)
    obj, rgt, par, bdg, oty = new_state("a")
    victim = z3.Int("victim_a")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i), wf(st, i))),
        # the premise is the DOUBLED bound, because a reparented edge is worth
        # two original ones — this is what makes the implication sound rather
        # than a bound chosen to make the lane green.
        z3.ForAll([i], z3.Implies(slot(i),
                                  acyclic_within(par, i, 2 * ACYCLIC_DEPTH))),
        slot(victim),
        obj[victim] != -1,
        par[victim] != -1,
    ]
    k = z3.Int("k")
    par2 = z3.Lambda([k], z3.If(par[k] == victim, par[victim], par[k]))

    j = z3.Int("j")
    prove("A9  delete-with-reparent introduces no cycle (%d before -> %d after)"
          % (2 * ACYCLIC_DEPTH, ACYCLIC_DEPTH),
          z3.ForAll([j], z3.Implies(z3.And(slot(j), j != victim),
                                    acyclic_within(par2, j, ACYCLIC_DEPTH))),
          extra=pre)


def prove_retype_preserves_O2():
    """
    `retype_preserves_O2`, the obligation this file first declined to model.

    Retype carves a NEW object from an Untyped and installs the capability as
    that Untyped's MDB child — which is precisely how O2 stays true without
    the kernel tracking storage: the edge is a creation edge, and a creation
    edge's parent is an Untyped by construction.  O6 (refusing a reset while
    children live) then reads off the same tree.

    The new object's id is symbolic and constrained to be FRESH, because
    reusing a live id is the one way retype could break the invariant and the
    proof should depend on the freshness rather than assume it away.
    """
    obj, rgt, par, bdg, oty = new_state("t")
    ut, dst = z3.Int("ut"), z3.Int("dst_t")
    noid = z3.Int("new_oid")
    birth = z3.BitVec("birth", W)
    nty = z3.Int("new_type")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i),
                                  z3.And(wf(st, i), inv_A7_at(st, i),
                                         inv_A8_at(st, i), inv_O2_at(st, i)))),
        slot(ut), slot(dst), ut != dst,
        ut != 0, dst != 0,                       # neither end may be CPtr 0
        obj[ut] != -1,                           # the untyped slot is occupied
        oty[obj[ut]] == T_UNTYPED,               # and holds an Untyped
        rgt[ut] & R_WRITE != 0,                  # carving MUTATES it
        obj[dst] == -1,                          # destination empty
        noid >= 0,
        # FRESH: no live slot already names this object.
        z3.ForAll([i], z3.Implies(z3.And(slot(i), obj[i] != -1),
                                  obj[i] != noid)),
    ]

    post = invariants.State(z3.Store(obj, dst, noid),
                            z3.Store(rgt, dst, birth),
                            z3.Store(par, dst, ut),
                            z3.Store(bdg, dst, z3.BitVecVal(0, W)),  # unbadged
                            z3.Store(oty, noid, nty), slot)

    j = z3.Int("j")
    prove("O2  retype preserves it — every creation edge names an Untyped",
          z3.ForAll([j], z3.Implies(slot(j), inv_O2_at(post, j))), extra=pre)
    prove("A7  retype preserves it — birth rights are not a derivation",
          z3.ForAll([j], z3.Implies(slot(j), inv_A7_at(post, j))), extra=pre)
    prove("A8  retype preserves it — an object is born unbadged",
          z3.ForAll([j], z3.Implies(slot(j), inv_A8_at(post, j))), extra=pre)
    prove("    retype preserves well-formedness",
          z3.ForAll([j], z3.Implies(slot(j), wf(post, j))), extra=pre)


REVOKE_DEPTH = 4


def prove_move_preserves():
    """
    MOVE, which is the operation these theorems had quietly skipped.

    derive, delete-with-reparent, retype and revoke were proved; `move` was
    not, and it is the one that relocates a capability WITH its place in the
    tree — the destination inherits the source's parent, every child
    re-points at the destination, and the source empties.  Three edges rewired
    at once is exactly the shape where A7 or O2 would break, and "we proved
    the other four" is not an argument about the fifth.

    `rotate` is NOT here and is not claimed: `sys_cspace_rotate` exists in the
    kernel but no operation in `spec/authority_model.py` drives it, so there
    is nothing for a theorem to be about yet.  Saying that beats a lemma over
    a definition nobody wrote.
    """
    obj, rgt, par, bdg, oty = new_state("m")
    src, dst = z3.Int("src_m"), z3.Int("dst_m")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i),
                                  z3.And(wf(st, i), inv_A7_at(st, i),
                                         inv_A8_at(st, i), inv_O2_at(st, i)))),
        # A9 as a HYPOTHESIS, and it has to be one.
        #
        # Without it the well-formedness half is REFUTED, by a source that is
        # its own parent: `wf` is satisfied by that state (the parent is a
        # slot and it is occupied), the move gives the destination `par[src]`
        # as its parent, and the source is then emptied — so the destination
        # names an empty slot.  Acyclicity forbids the state, `wf` alone does
        # not, and move is where the difference shows.
        #
        # The invariants are not independent, and this is the one place so far
        # that says so out loud: four operations were proved from `wf` plus
        # A7/A8/O2, and the fifth needs A9 as well.
        z3.ForAll([i], z3.Implies(slot(i), acyclic_within(par, i))),
        slot(src), slot(dst), src != dst,
        src != 0, dst != 0,               # CPtr 0 names nothing, either end
        obj[src] != -1,                   # source occupied
        obj[dst] == -1,                   # destination empty
    ]

    # dst takes src's identity and its place; src's children re-point at dst;
    # src empties.  Nothing else in the tree moves.
    k = z3.Int("k")
    obj2 = z3.Lambda([k], z3.If(k == dst, obj[src],
                                z3.If(k == src, -1, obj[k])))
    rgt2 = z3.Lambda([k], z3.If(k == dst, rgt[src], rgt[k]))
    bdg2 = z3.Lambda([k], z3.If(k == dst, bdg[src], bdg[k]))
    par2 = z3.Lambda([k], z3.If(k == dst, par[src],
                                z3.If(k == src, -1,
                                      z3.If(par[k] == src, dst, par[k]))))
    post = invariants.State(obj2, rgt2, par2, bdg2, oty, slot)

    j = z3.Int("j")
    prove("A7  move preserves it — the capability travels with its ancestry",
          z3.ForAll([j], z3.Implies(slot(j), inv_A7_at(post, j))), extra=pre)
    prove("A8  move preserves it",
          z3.ForAll([j], z3.Implies(slot(j), inv_A8_at(post, j))), extra=pre)
    prove("O2  move preserves it",
          z3.ForAll([j], z3.Implies(slot(j), inv_O2_at(post, j))), extra=pre)
    prove("    move preserves well-formedness",
          z3.ForAll([j], z3.Implies(slot(j), wf(post, j))), extra=pre)
    prove("    move changes no capability's rights",
          z3.And(rgt2[dst] == rgt[src], bdg2[dst] == bdg[src]), extra=pre)


def prove_revoke_clears_the_subtree():
    """
    A10's real content: revoke removes the DESCENDANTS and spares the root.

    "Every descendant" is a transitive closure, and the decidable fragment
    this file lives in cannot express one — so the closure is UNROLLED to
    depth %d and the theorem is proved for every chain that short.  That is
    weaker than the induction Isabelle owes and it is much stronger than the
    single step this file proved before: a bug that clears the root, or that
    stops one level early, or that spares a grandchild, is caught here.

    The unrolled depth is stated in the output for the same reason the
    trace count is: a bounded result that prints like an unbounded one is the
    failure mode this whole file is arranged against.
    """ % REVOKE_DEPTH
    obj, rgt, par, bdg, oty = new_state("v")
    root = z3.Int("root")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    def anc_within(k, x):
        """`x` has `root` among its first k ancestors."""
        acc = z3.BoolVal(False)
        cur = x
        for _ in range(k):
            acc = z3.Or(acc, cur == root)
            cur = par[cur]
        return z3.Or(acc, cur == root)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i), wf(st, i))),
        slot(root), obj[root] != -1,
    ]

    k = z3.Int("k")
    # revoke clears every slot that is a descendant of root within the bound,
    # and touches nothing else — root included.
    obj2 = z3.Lambda([k], z3.If(z3.And(k != root, anc_within(REVOKE_DEPTH, k)),
                                -1, obj[k]))

    d = z3.Int("d")
    prove("A10 revoke spares its own root",
          obj2[root] != -1, extra=pre)
    prove("A10 revoke clears every descendant within depth %d" % REVOKE_DEPTH,
          z3.ForAll([d], z3.Implies(z3.And(slot(d), d != root,
                                           anc_within(REVOKE_DEPTH, d)),
                                    obj2[d] == -1)), extra=pre)
    prove("    revoke clears nothing that is not a descendant",
          z3.ForAll([d], z3.Implies(z3.And(slot(d), obj[d] != -1,
                                           z3.Not(anc_within(REVOKE_DEPTH, d))),
                                    obj2[d] != -1)), extra=pre)


def prove_rotate_preserves():
    """
    ROTATE, which the previous commit said was not modelled and so not
    claimed.  It is modelled now, so it is claimed.

    Two relocations — pivot to dest, src to pivot — each carrying the
    capability's object, rights, badge and place in the tree.  Nothing is
    derived and no rights change, which is what makes it provable at all:
    A7 and A8 are about DERIVATION edges, and rotate creates none.

    A9 is a hypothesis for the same reason `move` needs it: a self-parented
    slot satisfies `wf` and breaks it the moment its capability moves and its
    old slot empties.  Two operations now need A9 where three do not, which
    is the shape of that dependency rather than a one-off.

    The dest == src branch — the three-way SWAP through a temporary — is NOT
    proved here.  It is a different post-state and stating it as this one
    would be the overclaim this file keeps catching; `spec-exhaustive` drives
    it 279841 traces' worth, which is evidence and not a proof.
    """
    obj, rgt, par, bdg, oty = new_state("rot")
    dest, pivot, src = z3.Int("dest_r"), z3.Int("pivot_r"), z3.Int("src_r")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i),
                                  z3.And(wf(st, i), inv_A7_at(st, i),
                                         inv_A8_at(st, i), inv_O2_at(st, i)))),
        z3.ForAll([i], z3.Implies(slot(i), acyclic_within(par, i))),
        slot(dest), slot(pivot), slot(src),
        src != pivot, dest != pivot, dest != src,   # the non-swap branch
        obj[src] != -1, obj[pivot] != -1,           # both occupied
        obj[dest] == -1,                            # destination empty
    ]

    #
    # pivot -> dest, then src -> pivot, written as ONE substitution instead of
    # nested special cases.
    #
    # The first version took `par[pivot]` raw for the destination's new
    # parent, which is wrong exactly when pivot's parent IS src: src's
    # capability has moved to the pivot slot, so the child must name `pivot`,
    # not the slot src just vacated.  The nested form applied the remap in its
    # fallthrough branch and not to the two capabilities that moved — a bug in
    # the encoding, and one that reads as correct until the case is named.
    #
    # `origin(k)` is the slot the capability now at k came from, and `remap`
    # is where a parent link has to point now.  Every field uses both, so
    # there is no branch left for one of them to be missing from.
    #
    def origin(x):
        return z3.If(x == dest, pivot, z3.If(x == pivot, src, x))

    def remap(x):
        return z3.If(x == pivot, dest, z3.If(x == src, pivot, x))

    k = z3.Int("k")
    obj2 = z3.Lambda([k], z3.If(k == src, -1, obj[origin(k)]))
    rgt2 = z3.Lambda([k], rgt[origin(k)])
    bdg2 = z3.Lambda([k], bdg[origin(k)])
    par2 = z3.Lambda([k], z3.If(k == src, -1, remap(par[origin(k)])))
    post = invariants.State(obj2, rgt2, par2, bdg2, oty, slot)

    #
    # A concrete state satisfying every premise above: three roots, two of
    # them occupied, nothing derived from anything.  Every invariant holds
    # vacuously (no parent links) and the three slots are distinct, which is
    # all the premises ask for.  Exhibited rather than searched for, because
    # the search does not terminate here.
    #
    wit = [
        obj == z3.Lambda([k], z3.If(k == 1, 10, z3.If(k == 2, 11, -1))),
        rgt == z3.Lambda([k], z3.BitVecVal(7, W)),
        bdg == z3.Lambda([k], z3.BitVecVal(0, W)),
        par == z3.Lambda([k], z3.IntVal(-1)),
        oty == z3.Lambda([k], z3.IntVal(8)),
        src == 1, pivot == 2, dest == 3,
    ]

    j = z3.Int("j")
    prove("A7  rotate preserves it — two relocations, no derivation",
          z3.ForAll([j], z3.Implies(slot(j), inv_A7_at(post, j))),
          extra=pre, witness=wit)
    prove("A8  rotate preserves it — badges TRAVEL, they are not arguments",
          z3.ForAll([j], z3.Implies(slot(j), inv_A8_at(post, j))),
          extra=pre, witness=wit)
    prove("O2  rotate preserves it",
          z3.ForAll([j], z3.Implies(slot(j), inv_O2_at(post, j))),
          extra=pre, witness=wit)
    prove("    rotate preserves well-formedness",
          z3.ForAll([j], z3.Implies(slot(j), wf(post, j))),
          extra=pre, witness=wit)
    prove("    rotate changes no capability's rights or badge",
          z3.And(rgt2[dest] == rgt[pivot], bdg2[dest] == bdg[pivot],
                 rgt2[pivot] == rgt[src], bdg2[pivot] == bdg[src]),
          extra=pre, witness=wit)


def prove_rotate_swap_preserves():
    """
    ROTATE's other branch: dest == src, which is a SWAP of src and pivot.

    Named as unproved one commit ago — "a different post-state, and stating it
    as the other one would be the overclaim this file keeps catching".  It is a
    different post-state and it is not a harder one: the swap is an INVOLUTION,
    and the same uniform substitution that fixed the non-swap branch states it
    in four lines.  Declining to prove something because the first encoding of
    its neighbour was wrong is not caution, it is leaving the easy half of a
    row red.

    The kernel routes this through a temporary slot on its stack
    (`kcnode_slot_rotate`'s `tmp`), which is an implementation detail of doing
    a swap without a third name — no reader can observe it, `mdb_lock` is held
    across all three relocations, so the specification is the swap itself.
    """
    obj, rgt, par, bdg, oty = new_state("swp")
    pivot, src = z3.Int("pivot_w"), z3.Int("src_w")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i),
                                  z3.And(wf(st, i), inv_A7_at(st, i),
                                         inv_A8_at(st, i), inv_O2_at(st, i)))),
        z3.ForAll([i], z3.Implies(slot(i), acyclic_within(par, i))),
        slot(pivot), slot(src), src != pivot,
        obj[src] != -1, obj[pivot] != -1,     # both occupied: it is a swap
    ]

    # An involution: each of the two slots takes the other's capability, and
    # every parent link naming either one follows it.
    def swap(x):
        return z3.If(x == src, pivot, z3.If(x == pivot, src, x))

    k = z3.Int("k")
    obj2 = z3.Lambda([k], obj[swap(k)])
    rgt2 = z3.Lambda([k], rgt[swap(k)])
    bdg2 = z3.Lambda([k], bdg[swap(k)])
    par2 = z3.Lambda([k], swap(par[swap(k)]))
    post = invariants.State(obj2, rgt2, par2, bdg2, oty, slot)

    wit = [
        obj == z3.Lambda([k], z3.If(k == 1, 10, z3.If(k == 2, 11, -1))),
        rgt == z3.Lambda([k], z3.BitVecVal(7, W)),
        bdg == z3.Lambda([k], z3.BitVecVal(0, W)),
        par == z3.Lambda([k], z3.IntVal(-1)),
        oty == z3.Lambda([k], z3.IntVal(8)),
        src == 1, pivot == 2,
    ]

    j = z3.Int("j")
    prove("A7  rotate's SWAP branch preserves it",
          z3.ForAll([j], z3.Implies(slot(j), inv_A7_at(post, j))),
          extra=pre, witness=wit)
    prove("A8  rotate's SWAP branch preserves it",
          z3.ForAll([j], z3.Implies(slot(j), inv_A8_at(post, j))),
          extra=pre, witness=wit)
    prove("O2  rotate's SWAP branch preserves it",
          z3.ForAll([j], z3.Implies(slot(j), inv_O2_at(post, j))),
          extra=pre, witness=wit)
    prove("    rotate's SWAP branch preserves well-formedness",
          z3.ForAll([j], z3.Implies(slot(j), wf(post, j))),
          extra=pre, witness=wit)
    prove("    the swap is an involution — doing it twice is the identity",
          z3.ForAll([j], z3.And(obj2[swap(j)] == obj[j],
                                par2[swap(j)] == swap(par[j]))),
          extra=pre, witness=wit)


def prove_slices_make_progress():
    """
    The two SLICED operations terminate, which is the property their designs
    rest on and the one neither had.

    D-8 bounded `SYS_CSPACE_REVOKE` and D-12 bounded CNode teardown, and both
    work the same way: do a slice, then ask to be re-executed while work
    remains.  That is safe only if a slice makes PROGRESS — otherwise the
    re-execution is a livelock with a preemption point in it, which is worse
    than the unbounded loop it replaced because it looks bounded.

    Both arguments were written in comments on the commits that introduced
    them, mine included.  A comment is where this project keeps its reasoning
    and not where it keeps its evidence.

    "The subtree strictly shrinks" needs a measure, and cardinality is not in
    the decidable fragment.  It is stated instead as the two halves that mean
    the same thing and are expressible: something that WAS an occupied
    descendant is now empty, and nothing that was empty became one.  A slice
    that satisfied only the first could be undone by the second.
    """
    obj, rgt, par, bdg, oty = new_state("s")
    root, victim = z3.Int("root_s"), z3.Int("victim_s")
    i = z3.Int("i")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    pre = [
        z3.ForAll([i], z3.Implies(slot(i), wf(st, i))),
        slot(root), slot(victim), root != victim,
        obj[root] != -1,
        obj[victim] != -1,
        par[victim] == root,          # the slice revoked a child of the root
    ]
    k = z3.Int("k")
    obj2 = z3.Lambda([k], z3.If(k == victim, -1, obj[k]))

    j = z3.Int("j")
    prove("    a revoke slice empties a slot that was occupied",
          z3.And(obj[victim] != -1, obj2[victim] == -1), extra=pre)
    prove("    a revoke slice fills nothing that was empty",
          z3.ForAll([j], z3.Implies(z3.And(slot(j), obj[j] == -1),
                                    obj2[j] == -1)), extra=pre)
    prove("    and it spares the root, so the NEXT slice has a root to run on",
          obj2[root] != -1, extra=pre)

    #
    # D-12's cascade, which is the same shape over a list instead of a tree.
    # A pass takes the head, empties that CNode and releases it; the property
    # that makes re-entry terminate is that the head is GONE from the list and
    # nothing was added by the taking.
    #
    head, nxt = z3.Int("head"), z3.Int("nxt")
    qn = z3.Array("queued", I, I)       # intrusive next; -1 ends the list
    qn2 = z3.Store(qn, head, -1)        # the head comes off
    prove("    a cascade pass removes the head from the queue",
          z3.And(qn2[head] == -1, qn2[nxt] == qn[nxt]),
          extra=[slot(head), slot(nxt), head != nxt, qn[head] == nxt])


def prove_revoke_root_survives():
    """
    A10's half that is a one-step property: revoke removes the SUBTREE and
    not the root.  The recursive half — that it reaches every descendant —
    is an induction over the tree and is left to Isabelle; what is provable
    here is that the invoked slot is untouched.
    """
    obj, rgt, par, bdg, oty = new_state("r")
    root = z3.Int("root")
    victim = z3.Int("victim")
    pre = [slot(root), slot(victim),
           obj[root] != -1, par[victim] == root, victim != root]
    obj2 = z3.Store(obj, victim, -1)
    prove("A10 revoke clears a child and never its own root",
          z3.And(obj2[victim] == -1, obj2[root] != -1), extra=pre)


def prove_d13_mapping_authority():
    """
    D-13 — the mechanism that makes a revoke reach a MAPPING, over every state.

    The invariant is `invariants.inv_D13`: a space that maps an object holds a
    capability naming it.  What is proved here is that the three operations
    compose to keep it — and, because this is the part that cost a boot, that
    the sweep does not take a mapping the space is still entitled to.

    `maps` and `hascap` are arrays of arrays indexed by (space, object).
    `hascap` is what `kvspace_map_cap_has` computes; `maps` is the frame's
    mapping list as seen from one space.
    """
    SP = z3.ArraySort(I, z3.BoolSort())
    maps   = z3.Array("maps", I, SP)
    hascap = z3.Array("hascap", I, SP)
    s_, o_ = z3.Int("space"), z3.Int("obj_d13")
    sp, ob = z3.Int("sp"), z3.Int("ob")

    def M(a, b):   return maps[a][b]
    def H(a, b):   return hascap[a][b]
    inv = lambda a, b: invariants.inv_D13(L, M, H, a, b)

    held = z3.ForAll([sp, ob], inv(sp, ob))

    # ── 1. a map sets both, so it cannot be what breaks the invariant ──────
    maps_m = z3.Store(maps, s_, z3.Store(maps[s_], o_, True))
    hcap_m = z3.Store(hascap, s_, z3.Store(hascap[s_], o_, True))
    prove("D13 a map installs the capability with the mapping",
          z3.ForAll([sp, ob],
                    z3.Implies(maps_m[sp][ob], hcap_m[sp][ob])),
          extra=[held])

    # ── 2. revoke may clear authority; finalise is what restores it ────────
    #
    # `hascap2` is ANY weakening of `hascap` on the revoked object: revoke only
    # ever removes authority, and which spaces lose it depends on the
    # derivation tree, which this theorem deliberately does not model.  Proving
    # it for every weakening is stronger than proving it for one.
    hascap2 = z3.Array("hascap2", I, SP)
    weakened = z3.ForAll([sp, ob],
                         z3.Implies(hascap2[sp][ob], hascap[sp][ob]))
    only_o = z3.ForAll([sp, ob],
                       z3.Implies(ob != o_, hascap2[sp][ob] == hascap[sp][ob]))

    # finalise(o): a mapping of `o` survives exactly where the authority does.
    maps_f = z3.Lambda([sp],
                z3.Lambda([ob], z3.If(ob == o_,
                                      z3.And(maps[sp][ob], hascap2[sp][ob]),
                                      maps[sp][ob])))

    prove("D13 revoke then finalise preserves it, for EVERY weakening",
          z3.ForAll([sp, ob], z3.Implies(maps_f[sp][ob], hascap2[sp][ob])),
          extra=[held, weakened, only_o])

    # ── 3. the sweep must not take a mapping that is still authorised ──────
    #
    # This is the half a boot refuted when the space's capability was a CHILD
    # of the slot mapped through: `Revoke(S)` destroyed the MAPPER's own
    # authority and the sweep then correctly unmapped the mapper's own page.
    # The bug was in `hascap`, not here — and this says what "not here" means.
    prove("D13 the sweep keeps every mapping the space still has authority for",
          z3.ForAll([sp, ob],
                    z3.Implies(z3.And(maps[sp][ob], hascap2[sp][ob]),
                               maps_f[sp][ob])),
          extra=[held, weakened, only_o])

    # ── 4. and it touches nothing else ─────────────────────────────────────
    prove("D13 the sweep changes no object but the one it was given",
          z3.ForAll([sp, ob],
                    z3.Implies(ob != o_, maps_f[sp][ob] == maps[sp][ob])),
          extra=[held, weakened, only_o])

    # ── 5. the claim that must stay REFUTABLE ──────────────────────────────
    #
    # Without the sweep, a revoke alone leaves a mapping with no authority.
    # That is the whole of D-13, so if the solver ever "proves" it this file is
    # agreeing with itself rather than checking anything.
    neg = z3.Solver()
    neg.add(held, weakened, only_o)
    neg.add(z3.Not(z3.ForAll([sp, ob],
                             z3.Implies(maps[sp][ob], hascap2[sp][ob]))))
    r = neg.check()
    results.append(("--  D13 revoke WITHOUT the sweep is still refutable",
                    "PROVED" if r == z3.sat else "REFUTED", None))


def prove_integrity_no_fabrication():
    """
    Phase 5's first property: INTEGRITY — no operation fabricates authority.

    Functional correctness is "the operation does what the specification
    says".  Integrity is weaker and different: whatever the operation does, it
    does not GRANT anything.  seL4 proves it as a separate theorem for the same
    reason — a kernel can satisfy its functional specification at every step
    and still, somewhere in that specification, hand out an authority nobody
    conferred.

    Three parts, and the third is the one that makes the other two mean
    anything:

      1. a capability that appears names an object that was ALREADY named;
      2. its rights are a subset of the rights it came from;
      3. and NOTHING ELSE CHANGED.

    Part 3 is the frame condition.  Without it, 1 and 2 are satisfied by an
    operation that installs correctly at the destination and quietly grants
    WRITE to an unrelated slot on the way past — which is exactly the shape of
    an integrity violation that a functional specification stated per-slot
    would not catch.  It is stated here as a universally quantified claim over
    every slot that is not the destination.
    """
    obj, rgt, par, bdg, oty = new_state("ig")
    src, dst = z3.Int("src_ig"), z3.Int("dst_ig")
    i, j = z3.Int("i_ig"), z3.Int("j_ig")
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    req = z3.BitVec("req_ig", W)
    pre = [
        z3.ForAll([i], z3.Implies(slot(i), wf(st, i))),
        slot(src), slot(dst), src != dst,
        obj[src] != -1,
        req & SAME_RIGHTS == 0,
    ]
    # The derive, as `kcnode_slot_derive` performs it.
    new_r = rights_reduce(rgt[src], req)
    obj2 = z3.Store(obj, dst, obj[src])
    rgt2 = z3.Store(rgt, dst, new_r)

    prove("IG  a derive names no object that was not already named",
          obj2[dst] == obj[src], extra=pre)
    prove("    and confers no right the source did not hold",
          new_r & rgt[src] == new_r, extra=pre)
    prove("    and changes NOTHING else — the frame condition",
          z3.ForAll([j], z3.Implies(z3.And(slot(j), j != dst),
                                    z3.And(obj2[j] == obj[j],
                                           rgt2[j] == rgt[j]))),
          extra=pre)

    #
    # The DESTRUCTIVE direction: authority only ever shrinks.
    #
    # A delete empties one slot and reparents its children; a revoke empties a
    # subtree.  Neither may leave any slot holding MORE than it held, and that
    # is a different claim from "the right slots were emptied": it is what
    # rules out a teardown that repaired a tree by widening somebody's rights.
    #
    victim = z3.Int("victim_ig")
    dpre = [z3.ForAll([i], z3.Implies(slot(i), wf(st, i))),
            slot(victim), obj[victim] != -1]
    objd = z3.Store(obj, victim, -1)
    rgtd = z3.Store(rgt, victim, z3.BitVecVal(0, W))
    prove("    a delete grants nothing: every slot holds what it held or less",
          z3.ForAll([j], z3.Implies(slot(j),
                                    z3.And(rgtd[j] & rgt[j] == rgtd[j],
                                           z3.Or(objd[j] == obj[j],
                                                 objd[j] == -1)))),
          extra=dpre)

    #
    # And the claim that must stay REFUTABLE: an operation that adds a right
    # the source did not hold is NOT integrity-preserving.  If this came back
    # PROVED, the three rows above would be agreeing with the encoding rather
    # than constraining the kernel.
    #
    extra_r = z3.BitVec("extra_ig", W)
    bad = z3.Solver()
    bad.add(*pre)
    bad.add(extra_r != 0, extra_r & rgt[src] == 0)
    bad.add(z3.Not((new_r | extra_r) & rgt[src] == (new_r | extra_r)))
    r = bad.check()
    results.append(("--  IG amplifying by one bit is still refutable",
                    "PROVED" if r == z3.sat else "REFUTED", None))


def prove_authority_confinement():
    """
    Phase 5's information-flow property, in the form this model can carry:
    AUTHORITY CONFINEMENT.

    seL4 proves two things beyond functional correctness.  Integrity — nothing
    is granted — is `prove_integrity_no_fabrication` above.  The other is about
    SUBJECTS: what one subject does cannot reach another's authority.  That
    needs a notion of subject, and this model has one that is not invented:
    a subject is a SUBTREE of the derivation forest, which is what "the
    authority reachable from a capability" means in a capability system.

    The claim: a revoke invoked inside one subtree changes no slot in a
    DISJOINT subtree.  Stated at a bounded depth, like the other revoke rows,
    because the unbounded descendant relation is a transitive closure the
    decidable fragment cannot express — `desc` in the Isabelle text can, and
    that asymmetry is recorded there.

    What this is NOT: non-interference in the confidentiality sense.  That
    needs a notion of OBSERVATION — what a subject can see, not what it can
    reach — and nothing in `spec/` models reading a message or timing an
    operation.  Saying so is the point: a confinement theorem presented as
    non-interference would be the strongest-sounding wrong claim in this file.
    """
    obj, rgt, par, bdg, oty = new_state("cf")
    a, b, victim, j = (z3.Int("subj_a"), z3.Int("subj_b"),
                       z3.Int("vic_cf"), z3.Int("j_cf"))
    st = invariants.State(obj, rgt, par, bdg, oty, slot)

    def anc(x, y, k):
        """`y` is within `k` parent links of `x` — the bounded ancestor walk,
        guarded by liveness so it never strolls through the -1 sentinel."""
        out, cur = [], x
        for _ in range(k):
            out.append(z3.And(slot(cur), obj[cur] != -1, par[cur] == y))
            cur = z3.If(z3.And(slot(cur), obj[cur] != -1), par[cur], -1)
        return z3.Or(*out)

    pre = [
        z3.ForAll([z3.Int("i_cf")],
                  z3.Implies(slot(z3.Int("i_cf")), wf(st, z3.Int("i_cf")))),
        slot(a), slot(b), slot(victim), slot(j),
        obj[a] != -1, obj[b] != -1,
        a != b,
        # Two SUBJECTS: neither subtree contains the other's root.
        z3.Not(anc(a, b, 4)), z3.Not(anc(b, a, 4)),
        # The revoke happens inside A: the victim descends from `a`.
        anc(victim, a, 4),
        # `j` is in B's subtree.
        z3.Or(j == b, anc(j, b, 4)),
    ]
    # The revoke clears the victim and nothing else — the one-step shape the
    # other revoke rows use.
    obj2 = z3.Store(obj, victim, -1)
    rgt2 = z3.Store(rgt, victim, z3.BitVecVal(0, W))

    prove("CF  a revoke inside one subject leaves a disjoint subject's slot alone",
          z3.And(obj2[j] == obj[j], rgt2[j] == rgt[j]),
          extra=pre + [j != victim])

    #
    # And the premise that makes it a confinement claim rather than a tautology
    # must be NECESSARY: drop "the subtrees are disjoint" and the conclusion
    # must stop following, or the theorem is about any two slots and says
    # nothing about subjects.
    #
    loose = z3.Solver()
    loose.add(slot(a), slot(b), slot(victim), slot(j),
              obj[a] != -1, obj[b] != -1, a != b,
              anc(victim, a, 4),
              z3.Or(j == b, anc(j, b, 4)))
    loose.add(j == victim, obj[victim] != -1)
    r = loose.check()
    results.append(("--  CF without disjointness the victim CAN be the "
                    "other subject's slot",
                    "PROVED" if r == z3.sat else "REFUTED", None))


def main():
    prove_the_harness_can_fail()
    prove_rights()
    prove_derive_preserves()
    prove_delete_preserves()
    prove_delete_preserves_A9()
    prove_retype_preserves_O2()
    prove_move_preserves()
    prove_revoke_clears_the_subtree()
    prove_rotate_preserves()
    prove_rotate_swap_preserves()
    prove_slices_make_progress()
    prove_revoke_root_survives()
    prove_d13_mapping_authority()
    prove_integrity_no_fabrication()
    prove_authority_confinement()

    bad = 0
    for name, verdict, model in results:
        print("  %-8s %s" % (verdict, name))
        if verdict != "PROVED":
            bad += 1
            if model is not None:
                print("           counterexample: %s" % model)

    print("\nspec-prove: %d of %d obligations discharged by z3 %s"
          % (len(results) - bad, len(results), z3.get_version_string()))
    if bad:
        print("spec-prove: %d NOT proved — see above" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
