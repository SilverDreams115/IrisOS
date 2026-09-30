#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
authority_model.py — the EXECUTABLE SPECIFICATION of IRIS's authority model.

This is Phase 1 of the IRIS-V verification plan: the middle layer of the three
the plan calls for (abstract spec -> executable spec -> C), and the one that is
written first because it is the only one that can be CHECKED against something
today.

    What it is        a state machine over CNodes, slots, objects and the
                      capability derivation tree, written independently of the
                      kernel and deliberately in another language, so that
                      agreeing with the C is evidence rather than tautology.

    What it is for    `spec/differential.py` drives the SAME operation
                      sequences through this model and through the real
                      `kcnode.c` / `cspace.c` / `kobject.c` / `kuntyped.c`
                      (linked into `build/spec_driver`) and compares the
                      resulting state, slot by slot and edge by edge.

    Why this order    seL4 built its functional model before the C existed,
                      because you cannot verify a kernel that was not written
                      against a specification.  IRIS is in the opposite and
                      better position: the implementation already works and
                      325 runtime tests describe what it should do, so the
                      model can be validated against a running oracle from the
                      first day.  That advantage is the whole reason this file
                      comes before any Isabelle.

    What it is NOT    a proof, and not Isabelle.  It is the artefact that makes
                      an Isabelle refinement tractable later, and that finds
                      disagreements NOW.

The invariants below are the charter's, restated as properties of a state
rather than as prose.  Where the charter says "rights are only kept or
reduced", this file says `chk_rights_monotone`, and a counterexample is a
sequence of operations rather than an argument.
"""

from dataclasses import dataclass, field
from typing import Optional

# ── rights, matching kernel/new_core/include/iris/nc/rights.h ───────────────
R_READ, R_WRITE, R_DUPLICATE, R_TRANSFER = 1, 2, 4, 8
R_WAIT, R_ROUTE, R_MANAGE = 16, 32, 64
R_ALL = R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER | R_WAIT | R_ROUTE | R_MANAGE

# ── object types, matching kobject_type_t ───────────────────────────────────
T_ENDPOINT, T_NOTIFICATION, T_CNODE, T_UNTYPED = "ep", "notif", "cnode", "ut"

OK = 0
ERR_INVALID_ARG = -1
ERR_NOT_FOUND = -3
ERR_ACCESS_DENIED = -5
ERR_ALREADY_EXISTS = -7
ERR_BUSY = -8


@dataclass
class Obj:
    """A kernel object. `oid` stands in for the pointer the C uses."""
    oid: int
    type: str
    # The DERIVATION TREE decides when this dies, not a reference count.
    # That is the D-7 question stated as a model: an object exists exactly
    # while some slot names it, and "some slot names it" is a fact about the
    # tree, so the model never counts.
    alive: bool = True


@dataclass
class Slot:
    """One capability. Empty when `obj` is None."""
    obj: Optional[int] = None          # oid
    rights: int = 0
    badge: int = 0
    parent: Optional[int] = None       # slot id
    children: list = field(default_factory=list)   # slot ids, in order

    @property
    def empty(self) -> bool:
        return self.obj is None


class AuthorityModel:
    """
    The whole authority model, as a state machine.

    Slots are addressed by a flat integer so that a trace is a list of small
    integers and nothing in the trace format depends on how the C lays CNodes
    out.  `slots_per_cnode` fixes the mapping: slot id = cnode * N + index.
    """

    def __init__(self, ncnodes: int = 4, slots_per_cnode: int = 16):
        self.ncnodes = ncnodes
        self.spc = slots_per_cnode
        self.slots = {i: Slot() for i in range(ncnodes * slots_per_cnode)}
        self.objs: dict = {}
        self._next_oid = 1

    # ── helpers ────────────────────────────────────────────────────────────
    def _new_obj(self, type_: str) -> int:
        oid = self._next_oid
        self._next_oid += 1
        self.objs[oid] = Obj(oid, type_)
        return oid

    def _descendants(self, sid: int):
        """Every slot below `sid`, depth first. NOT including `sid`."""
        out = []
        stack = list(self.slots[sid].children)
        while stack:
            c = stack.pop()
            out.append(c)
            stack.extend(self.slots[c].children)
        return out

    def _unlink(self, sid: int):
        p = self.slots[sid].parent
        if p is not None and sid in self.slots[p].children:
            self.slots[p].children.remove(sid)
        self.slots[sid].parent = None

    def _clear(self, sid: int):
        """
        Empty one slot and collect the object if nothing names it any more.

        The collection test is a SEARCH of the slot space, not a counter.
        That is the point of the model: `an object exists exactly while a
        capability names it` is checked against the tree, so a counter that
        drifted from the tree — which the ledger records as the cause of three
        defects (D-7) — cannot be reproduced here by construction.
        """
        s = self.slots[sid]
        oid = s.obj
        self._unlink(sid)
        s.obj, s.rights, s.badge = None, 0, 0
        if oid is not None and not any(
                t.obj == oid for t in self.slots.values()):
            self.objs[oid].alive = False

    # ── the operations ─────────────────────────────────────────────────────
    def retype(self, ut_sid: int, type_: str, dst_sid: int) -> int:
        """Create an object from an Untyped; the new slot is its MDB child."""
        ut = self.slots.get(ut_sid)
        if ut is None or ut.empty:
            return ERR_NOT_FOUND
        if self.objs[ut.obj].type != T_UNTYPED:
            return ERR_INVALID_ARG
        dst = self.slots.get(dst_sid)
        if dst is None:
            return ERR_INVALID_ARG
        if not dst.empty:
            return ERR_ALREADY_EXISTS
        oid = self._new_obj(type_)
        dst.obj, dst.rights, dst.badge = oid, R_ALL, 0
        dst.parent = ut_sid
        ut.children.append(dst_sid)
        return OK

    def seed(self, sid: int, type_: str, rights: int = R_ALL) -> int:
        """
        A fresh object installed as an MDB ROOT — `kcnode_mint`.

        It REPLACES an occupied slot rather than refusing it, and that is the
        documented split in the kernel: `kcnode_mint` installs, and
        `kcnode_mint_excl` is the one that answers ALREADY_EXISTS, "so a
        spawner cannot clobber a child's slots".  The model refused, the
        differential disagreed on its 10th trace, and the kernel was right.

        What makes the replace SAFE is that the old capability is DELETED
        first, with the reparenting a delete does: a slot overwritten while it
        had children leaves those children attached to its parent, not
        dangling.  That is checked — a trace that overwrites a slot in the
        middle of a derivation chain is exactly what the random generator
        produces, and the two sides agree on where the grandchildren land.
        """
        s = self.slots.get(sid)
        if s is None:
            return ERR_INVALID_ARG
        if not s.empty:
            self.delete(sid)
        oid = self._new_obj(type_)
        s.obj, s.rights, s.badge = oid, rights, 0
        s.parent = None
        return OK

    def mint(self, src_sid: int, dst_sid: int, rights: int,
             badge: int = 0) -> int:
        """
        Derive a copy. Rights may only be REDUCED (A7), the source must carry
        DUPLICATE, and a badged capability is never re-badged (A8).
        """
        src = self.slots.get(src_sid)
        dst = self.slots.get(dst_sid)
        if src is None or dst is None:
            return ERR_INVALID_ARG
        if src.empty:
            return ERR_NOT_FOUND
        if not dst.empty:
            return ERR_ALREADY_EXISTS
        if not (src.rights & R_DUPLICATE):
            return ERR_ACCESS_DENIED
        new_rights = rights & src.rights          # reduction, never amplification
        if new_rights == 0:
            return ERR_INVALID_ARG                # collapse to NONE is refused
        # ── A8, in full, and it has three parts, not one ──────────────────
        #
        # The charter's A8 row says "badges are kernel-sealed identity; a
        # badged cap is never re-badged", and the FIRST version of this model
        # implemented exactly that sentence.  The differential disagreed with
        # the kernel on its 4th trace, and the kernel was right: the sentence
        # is half the rule.  `mdb_badge_derive` in kcnode.c is the only badge
        # logic in the system and it decides three things:
        #
        #   1. asking for 0 INHERITS — a zero request is not "no badge", it
        #      is "whatever the source has";
        #   2. a badged source asked for a DIFFERENT badge is ACCESS_DENIED —
        #      not silently inherited, which is what this model used to do;
        #   3. a fresh badge may only be stamped on an IDENTITY TYPE, an
        #      endpoint or a notification.  A badge is sender identity, and a
        #      CNode has no sender, so badging one is meaningless and the
        #      kernel refuses it rather than storing a number nothing reads.
        #
        # Part 3 is nowhere in the charter.  That is the finding, and it is
        # the kind this phase exists to produce: not a bug in either side, but
        # a specification that was ambiguous and is now decided.
        if badge == 0:
            new_badge = src.badge                       # (1) inherit
        elif src.badge != 0:
            if src.badge != badge:
                return ERR_ACCESS_DENIED                # (2) never re-badged
            new_badge = src.badge
        else:
            if self.objs[src.obj].type not in (T_ENDPOINT, T_NOTIFICATION):
                return ERR_INVALID_ARG                  # (3) identity types only
            new_badge = badge
        dst.obj, dst.rights, dst.badge = src.obj, new_rights, new_badge
        dst.parent = src_sid
        src.children.append(dst_sid)
        return OK

    def delete(self, sid: int) -> int:
        """
        Empty one slot. Its children are REPARENTED to its parent, so no
        capability loses its ancestry because an intermediate slot went
        (A9: every derived capability is traceable to its ancestor).
        """
        s = self.slots.get(sid)
        if s is None:
            return ERR_INVALID_ARG
        if s.empty:
            return OK                              # deleting nothing succeeds
        p = s.parent
        for c in list(s.children):
            self.slots[c].parent = p
            if p is not None:
                self.slots[p].children.append(c)
        s.children.clear()
        self._clear(sid)
        return OK

    def revoke(self, sid: int) -> int:
        """
        Destroy the SUBTREE and not its root (A10). Returns OK; the count is
        not part of the state, so the model does not model it.
        """
        s = self.slots.get(sid)
        if s is None:
            return ERR_INVALID_ARG
        if s.empty:
            return OK
        for d in self._descendants(sid):
            self.slots[d].children.clear()
            self._clear(d)
        s.children.clear()
        return OK

    def ut_reset(self, ut_sid: int) -> int:
        """An Untyped rewinds only when nothing was carved from it (O6)."""
        ut = self.slots.get(ut_sid)
        if ut is None or ut.empty:
            return ERR_NOT_FOUND
        if self.objs[ut.obj].type != T_UNTYPED:
            return ERR_INVALID_ARG
        if ut.children:
            return ERR_BUSY
        return OK

    # ── the state, canonically ─────────────────────────────────────────────
    def dump(self) -> str:
        """
        One line per occupied slot, sorted, in the format the C driver prints.
        Objects are renumbered in slot order so the two sides can agree
        without sharing pointers.
        """
        canon, nxt = {}, 1
        for sid in sorted(self.slots):
            o = self.slots[sid].obj
            if o is not None and o not in canon:
                canon[o] = nxt
                nxt += 1
        out = []
        for sid in sorted(self.slots):
            s = self.slots[sid]
            if s.empty:
                continue
            out.append("slot %d obj=%d type=%s rights=%d badge=%d parent=%s" % (
                sid, canon[s.obj], self.objs[s.obj].type, s.rights, s.badge,
                str(s.parent) if s.parent is not None else "-"))
        return "\n".join(out)


# ── the charter's invariants, as checkable properties ───────────────────────
#
# Each returns None when it holds, or a string naming the violation.  These
# are what an Isabelle abstract specification would state as theorems; here
# they are executable, so a random trace can look for a counterexample.

def chk_rights_monotone(m: AuthorityModel):
    """A7 — a child's rights are a subset of its parent's."""
    for sid, s in m.slots.items():
        if s.empty or s.parent is None:
            continue
        p = m.slots[s.parent]
        if p.empty:
            return "slot %d has a parent %d that is empty" % (sid, s.parent)
        if s.rights & ~p.rights:
            return "slot %d amplified rights over its parent %d" % (sid, s.parent)
    return None


def chk_badge_sealed(m: AuthorityModel):
    """A8 — a badged capability is never re-badged by a derivation."""
    for sid, s in m.slots.items():
        if s.empty or s.parent is None:
            continue
        p = m.slots[s.parent]
        if p.empty:
            continue
        if p.badge != 0 and s.badge != p.badge:
            return "slot %d re-badged a badged parent %d" % (sid, s.parent)
    return None


def chk_tree_acyclic(m: AuthorityModel):
    """A9 — the derivation graph is a forest: every slot reaches a root."""
    for sid, s in m.slots.items():
        if s.empty:
            continue
        seen, cur, n = set(), sid, 0
        while cur is not None:
            if cur in seen:
                return "slot %d is in a derivation cycle" % sid
            seen.add(cur)
            cur = m.slots[cur].parent
            n += 1
            if n > len(m.slots) + 1:
                return "slot %d has an unbounded ancestor chain" % sid
    return None


def chk_links_symmetric(m: AuthorityModel):
    """The tree is doubly linked and the two directions agree."""
    for sid, s in m.slots.items():
        for c in s.children:
            if m.slots[c].parent != sid:
                return "slot %d lists child %d which does not point back" % (sid, c)
        if s.parent is not None and sid not in m.slots[s.parent].children:
            return "slot %d names parent %d which does not list it" % (sid, s.parent)
    return None


def chk_derivation_same_object(m: AuthorityModel):
    """A derivation names the same object as its parent — except a retype,
    whose parent is the Untyped the storage came from."""
    for sid, s in m.slots.items():
        if s.empty or s.parent is None:
            continue
        p = m.slots[s.parent]
        if p.empty:
            continue
        if s.obj != p.obj and m.objs[p.obj].type != T_UNTYPED:
            return "slot %d derives from %d but names another object" % (sid, s.parent)
    return None


def chk_object_liveness(m: AuthorityModel):
    """
    D-7's target, stated: an object is alive exactly while a slot names it.

    This is the property refcounting only approximates.  Here it is decided by
    searching the slot space, so the model cannot drift the way two counters
    can — and a C implementation that disagrees with this check is showing the
    drift the ledger says caused three defects.
    """
    named = {s.obj for s in m.slots.values() if not s.empty}
    for oid, o in m.objs.items():
        if o.alive and oid not in named:
            return "object %d is alive and no slot names it" % oid
        if not o.alive and oid in named:
            return "object %d is dead and slot(s) still name it" % oid
    return None


ALL_CHECKS = (
    chk_rights_monotone,
    chk_badge_sealed,
    chk_tree_acyclic,
    chk_links_symmetric,
    chk_derivation_same_object,
    chk_object_liveness,
)


def check_all(m: AuthorityModel):
    for c in ALL_CHECKS:
        why = c(m)
        if why:
            return "%s: %s" % (c.__name__, why)
    return None
