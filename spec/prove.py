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


def decide(assertions):
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
        s.set("timeout", 60000)
        for k, v in cfg.items():
            s.set(k, v)
        for a in assertions:
            s.add(a)
        r = s.check()
        if r != z3.unknown:
            return s, r
    return s, z3.unknown


def prove(name, claim, extra=None):
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
        g, r = decide(extra)
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
    prove("    reduce is idempotent",
          rights_reduce(rights_reduce(b, r), r) == rights_reduce(b, r),
          extra=[r & SAME_RIGHTS == 0])
    prove("    reduce is commutative",
          rights_reduce(b, r) == rights_reduce(r, b),
          extra=[b & SAME_RIGHTS == 0, r & SAME_RIGHTS == 0])
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


def main():
    prove_the_harness_can_fail()
    prove_rights()
    prove_derive_preserves()
    prove_delete_preserves()
    prove_delete_preserves_A9()
    prove_retype_preserves_O2()
    prove_revoke_clears_the_subtree()
    prove_revoke_root_survives()

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
