#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
theory_check.py — the Isabelle text against the executable model.

`spec/isabelle/IRIS_Authority.thy` is the ABSTRACT specification and
`spec/authority_model.py` is the EXECUTABLE one, and phase 4's first link is
`abstract ⊑ executable`.  `spec/bridge.py` establishes it for the INVARIANTS:
the predicates live once in `spec/invariants.py` and both sides read them.

Nothing established it for the OPERATIONS.  The theory states `can_derive`,
`derive`, `delete`, `revoke`, `can_retype` and `retype`; the model implements
`mint`, `delete`, `revoke` and `retype`; and the two were compared by eye, by
whoever wrote the second one.  The theory says what is wrong with that in its
own words — "the SMT encoding is a SECOND formalisation, and if it disagrees
with this one it proves something about a neighbouring system" — and the same
sentence applies to the model.

So this file TRANSLITERATES the theory's definitions, mechanically, clause by
clause, and runs them against the model over random states.

It is deliberately a transliteration and not a third reading.  Every function
below is a line-for-line rendering of the `definition` of the same name, with
the Isabelle kept in the docstring so the two can be diffed by a reader who
does not know Isabelle.  Where it had to choose, it chose what the text says
and not what the model does — otherwise it would agree by construction, which
is the failure mode this whole directory exists to avoid.
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from authority_model import (AuthorityModel, R_READ, R_WRITE, R_DUPLICATE,  # noqa: E402
                             R_TRANSFER, R_ALL, T_ENDPOINT, T_NOTIFICATION,
                             T_UNTYPED, OK, ERR_INVALID_ARG, ERR_NOT_FOUND,
                             ERR_ACCESS_DENIED, ERR_ALREADY_EXISTS,
                             ERR_WRONG_TYPE, MAPCAP_BASE)

# The theory's `err` datatype, mapped onto the model's codes.  The names are
# the theory's; the numbers are the ABI's.
Ok, InvalidArg, NotFound = OK, ERR_INVALID_ARG, ERR_NOT_FOUND
AccessDenied, AlreadyExists, WrongType = (ERR_ACCESS_DENIED,
                                          ERR_ALREADY_EXISTS, ERR_WRONG_TYPE)


def occupied(C, i):
    """`occupied C i ⟷ i ∈ dom_s C ∧ s_obj (slots C i) ≠ None`"""
    return i in C["dom_s"] and C["slots"][i]["obj"] is not None


def rights_reduce(b, r):
    """`rights_reduce b r = (b AND r)`  — the theory's, intersection only."""
    return b & r


def rights_check(have, need):
    """`rights_check have need ⟷ (need ≠ 0 ∧ (have AND need) = need)`"""
    return need != 0 and (have & need) == need


def can_derive(C, src, dst, req, bg):
    """
    The theory, clause by clause:

        if src = dst then InvalidArg
        else if ¬ occupied C src then NotFound
        else if ¬ rights_check (s_rights (slots C src)) R_DUPLICATE
             then AccessDenied
        else if rights_reduce (s_rights (slots C src)) req = 0 then InvalidArg
        else if bg ≠ 0 ∧ s_badge src ≠ 0 ∧ s_badge src ≠ bg then AccessDenied
        else if bg ≠ 0 ∧ s_badge src = 0 ∧ o_type (obj of src) ∉ {Endpoint,
             Notification} then InvalidArg
        else if occupied C dst then AlreadyExists
        else Ok
    """
    if src == dst:
        return InvalidArg
    if not occupied(C, src):
        return NotFound
    s = C["slots"][src]
    if not rights_check(s["rights"], R_DUPLICATE):
        return AccessDenied
    if rights_reduce(s["rights"], req) == 0:
        return InvalidArg
    if bg != 0 and s["badge"] != 0 and s["badge"] != bg:
        return AccessDenied
    if bg != 0 and s["badge"] == 0 and \
            C["objs"][s["obj"]] not in (T_ENDPOINT, T_NOTIFICATION):
        return InvalidArg
    if occupied(C, dst):
        return AlreadyExists
    return Ok


def badge_of_derive(C, src, bg):
    """`badge_of_derive C src bg = (if bg = 0 then s_badge src else bg)`"""
    return C["slots"][src]["badge"] if bg == 0 else bg


def derive(C, src, dst, req, bg):
    """The theory's `derive`: dst becomes a CHILD of src, src's list gains it."""
    s = C["slots"]
    s[dst] = {"obj": s[src]["obj"],
              "rights": rights_reduce(s[src]["rights"], req),
              "badge": badge_of_derive(C, src, bg),
              "parent": src,
              "children": []}
    s[src] = dict(s[src], children=s[src]["children"] + [dst])
    return C


def delete(C, i):
    """The theory's `delete`: REPARENTS, and a root's children become roots."""
    s = C["slots"]
    p, ch = s[i]["parent"], list(s[i]["children"])
    out = {}
    for t in C["dom_s"]:
        if t == i:
            out[t] = {"obj": None, "rights": 0, "badge": 0,
                      "parent": None, "children": []}
        elif t in ch:
            out[t] = dict(s[t], parent=p)
        elif p is not None and t == p:
            out[t] = dict(s[t],
                          children=[c for c in s[t]["children"] if c != i] + ch)
        else:
            out[t] = s[t]
    C["slots"] = out
    return C


def desc(C, i):
    """
    `desc C i = {j. (j, i) ∈ {(a, b). s_parent a = Some b}⁺}`

    The transitive closure, computed as the fixed point it is.  Isabelle can
    SAY this; a decidable fragment cannot, which is why the unbounded revoke
    obligation lives there and only an unrolling lives in `spec/prove.py`.
    """
    out, frontier = set(), {i}
    # BOUND: the slot space; each pass adds at least one slot or stops.
    while frontier:
        nxt = {j for j in C["dom_s"]
               if C["slots"][j]["parent"] in frontier and j not in out}
        out |= nxt
        frontier = nxt
    return out


def revoke(C, i):
    """The theory's `revoke`: the subtree goes, the ROOT keeps its capability."""
    D = desc(C, i)
    s = C["slots"]
    out = {}
    for t in C["dom_s"]:
        if t in D:
            out[t] = {"obj": None, "rights": 0, "badge": 0,
                      "parent": None, "children": []}
        elif t == i:
            out[t] = dict(s[t], children=[])
        else:
            out[t] = s[t]
    C["slots"] = out
    return C


def post_revoke_exact(C0, C1, i):
    """
    `revoke_removes_exactly_the_subtree`, checked against the TRUE closure.

    The lemma is one of the two the theory owes an induction for, and
    `spec/prove.py` can only unroll it to depth 4 because a transitive closure
    is outside the decidable fragment.  `desc` here IS the closure, computed as
    the fixed point it is — so this checks the lemma's full content on every
    state a trace builds: unbounded in DEPTH, finite in states.

    That is not the induction and does not replace it.  It is the difference
    between "true for every descendant within four links" and "true for every
    descendant, on these states", and the second is strictly more than the
    first was.
    """
    D = desc(C0, i)
    for j in sorted(C0["dom_s"]):
        if j in D:
            if C1["slots"][j]["obj"] is not None:
                return "descendant %d survived the revoke" % j
        elif j == i:
            if C1["slots"][j]["obj"] != C0["slots"][j]["obj"]:
                return "the revoke took its own root %d" % j
            if C1["slots"][j]["children"] != []:
                return "root %d kept children" % j
        else:
            a, b = C0["slots"][j], C1["slots"][j]
            for f in ("obj", "rights", "badge", "parent"):
                if a[f] != b[f]:
                    return ("non-descendant %d changed its %s" % (j, f))
    return None


def acyclic(C):
    """
    `inv_A9` as a DECISION, not a depth bound.

    `delete_preserves_A9` is the other owed induction, and the reason it is
    owed is recorded in the theory: a DEPTH-BOUNDED acyclicity invariant is not
    inductive under reparenting, and the solver refuted the naive statement.
    Over a concrete state the question is simply decidable — follow parents and
    see whether you return — so `delete` can be checked for the real property
    rather than for a relative one.
    """
    for start in C["dom_s"]:
        seen, cur = set(), start
        # BOUND: the slot space; a repeat is a cycle and ends the walk.
        while cur is not None:
            if cur in seen:
                return "cycle through slot %d" % start
            seen.add(cur)
            if cur not in C["dom_s"]:
                return "parent %d outside dom_s" % cur
            cur = C["slots"][cur]["parent"]
    return None


def to_theory_state(m):
    """`AuthorityModel` -> the theory's `cspace` record.

    The mapping-capability pseudo-slots are LEFT OUT: the theory models a
    CSpace and says so, and including them would be comparing the model
    against a text that never mentioned them."""
    return {
        "slots": {i: {"obj": s.obj, "rights": s.rights, "badge": s.badge,
                      "parent": s.parent, "children": list(s.children)}
                  for i, s in m.slots.items() if i < MAPCAP_BASE},
        "objs": {o: m.objs[o].type for o in m.objs},
        "dom_s": {i for i in m.slots if i < MAPCAP_BASE},
    }


def state_eq(a, b):
    """Slot for slot, with the child lists compared as ORDER MATTERS."""
    if a["dom_s"] != b["dom_s"]:
        return "dom_s differs"
    for i in sorted(a["dom_s"]):
        x, y = a["slots"][i], b["slots"][i]
        for f in ("obj", "rights", "badge", "parent"):
            if x[f] != y[f]:
                return "slot %d %s: theory %r, model %r" % (i, f, x[f], y[f])
        if x["children"] != y["children"]:
            return ("slot %d children: theory %r, model %r"
                    % (i, x["children"], y["children"]))
    return None


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 400
    rng = random.Random(0x7E0C0DE)
    from differential import gen_trace, run_model

    bad = 0
    checked = {"derive": 0, "delete": 0, "revoke": 0}

    for t in range(n):
        ops = gen_trace(rng, rng.randint(3, 25))
        m, _ = run_model(ops)
        occ = [i for i in sorted(m.slots)
               if i < MAPCAP_BASE and not m.slots[i].empty]
        if not occ:
            continue

        # ── derive ───────────────────────────────────────────────────────
        src = rng.choice(occ)
        dst = rng.choice(sorted(i for i in m.slots if i < MAPCAP_BASE))
        req = rng.choice([R_READ, R_READ | R_DUPLICATE, R_ALL,
                          R_WRITE | R_TRANSFER])
        bg = rng.choice([0, 0, 7, 42])

        C = to_theory_state(m)
        r_thy = can_derive(C, src, dst, req, bg)
        import copy
        m2 = copy.deepcopy(m)
        r_mod = m2.mint(src, dst, req, bg)
        checked["derive"] += 1
        if r_thy != r_mod:
            bad += 1
            if bad <= 6:
                print("theory_check: derive(%d,%d,%d,%d) -> theory %d, "
                      "model %d" % (src, dst, req, bg, r_thy, r_mod))
                print("  trace: " + " ; ".join(ops))
        elif r_thy == Ok:
            C2 = derive(to_theory_state(m), src, dst, req, bg)
            why = state_eq(C2, to_theory_state(m2))
            if why:
                bad += 1
                if bad <= 6:
                    print("theory_check: derive STATE differs — %s" % why)
                    print("  trace: " + " ; ".join(ops))

        # ── delete ───────────────────────────────────────────────────────
        i = rng.choice(occ)
        m3 = copy.deepcopy(m)
        m3.delete(i)
        C3 = delete(to_theory_state(m), i)
        checked["delete"] += 1
        why = state_eq(C3, to_theory_state(m3))
        if why:
            bad += 1
            if bad <= 6:
                print("theory_check: delete(%d) STATE differs — %s" % (i, why))
                print("  trace: " + " ; ".join(ops))

        # ── revoke ───────────────────────────────────────────────────────
        i = rng.choice(occ)
        m4 = copy.deepcopy(m)
        m4.revoke(i)
        C0 = to_theory_state(m)
        C4 = revoke(to_theory_state(m), i)
        checked["revoke"] += 1
        # The owed lemma's own content, over the TRUE closure.
        why = post_revoke_exact(C0, C4, i)
        if why:
            bad += 1
            if bad <= 6:
                print("theory_check: revoke(%d) EXACTNESS — %s" % (i, why))
                print("  trace: " + " ; ".join(ops))
        checked["revoke_exact"] = checked.get("revoke_exact", 0) + 1
        # and A9 decided rather than bounded, after the reparenting delete
        why = acyclic(C3)
        if why:
            bad += 1
            if bad <= 6:
                print("theory_check: delete(%d) broke A9 — %s" % (i, why))
                print("  trace: " + " ; ".join(ops))
        checked["a9"] = checked.get("a9", 0) + 1
        why = state_eq(C4, to_theory_state(m4))
        if why:
            bad += 1
            if bad <= 6:
                print("theory_check: revoke(%d) STATE differs — %s" % (i, why))
                print("  trace: " + " ; ".join(ops))

        if bad > 6:
            print("... stopping after %d disagreements" % bad)
            break

    tot = sum(checked.values())
    if bad:
        print("\ntheory_check: %d disagreement(s) over %d applications"
              % (bad, tot))
        return 1
    print("theory_check: %d applications over %d states — the Isabelle text "
          "and the executable model agree on every result code and every "
          "resulting state (derive %d, delete %d, revoke %d); and the two OWED "
          "lemmas hold over the TRUE transitive closure on every one: revoke "
          "removed exactly the subtree %d times and a reparenting delete left "
          "the forest acyclic %d times"
          % (tot, n, checked["derive"], checked["delete"], checked["revoke"],
             checked.get("revoke_exact", 0), checked.get("a9", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
