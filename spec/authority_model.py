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
T_REPLY, T_SC = "reply", "sc"

OK = 0
ERR_INVALID_ARG = -1
ERR_NOT_FOUND = -3
ERR_ACCESS_DENIED = -5
ERR_ALREADY_EXISTS = -7
ERR_BUSY = -8
ERR_WRONG_TYPE = -9


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
        # Objects the TEST fabricated (see `_new_obj`) — the population whose
        # liveness the driver can observe, and therefore the one D-7's check
        # compares.
        self.seeded: set = set()
        self._next_oid = 1
        self.last_revoked = 0

    # ── helpers ────────────────────────────────────────────────────────────
    def _new_obj(self, type_: str, seeded: bool = False) -> int:
        """
        `seeded` marks an object the TEST fabricated rather than one a retype
        carved from an Untyped.

        The distinction exists for D-7 and nothing else: the driver can hook a
        fabricated object's destructor and count it, and cannot hook a real
        retyped object's, because that one runs the kernel's own.  So the
        liveness comparison is over the population both sides can actually
        see, and saying which population that is beats comparing two different
        ones and calling the difference a kernel bug — which is what the first
        version of this did.
        """
        oid = self._next_oid
        self._next_oid += 1
        self.objs[oid] = Obj(oid, type_)
        self.seeded.add(oid) if seeded else None
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
    # The rights a retyped capability is BORN with, per type.  They are not
    # R_ALL and they are not uniform: a notification carries WAIT and an
    # endpoint does not, because waiting is what a notification is for.
    # `syscall_untyped.c` is the source; a model that guessed "everything"
    # would agree with the kernel on shape and disagree on the one field an
    # authority model exists to describe.
    BIRTH_RIGHTS = {
        T_ENDPOINT:     R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER,
        T_NOTIFICATION: R_READ | R_WRITE | R_WAIT | R_DUPLICATE | R_TRANSFER,
        "reply":        R_READ | R_WRITE | R_TRANSFER | R_DUPLICATE,
        "sc":           R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER,
    }

    def install_untyped(self, sid: int) -> int:
        """An Untyped installed as a root — the driver's `ut`, `kcnode_mint`."""
        return self.seed(sid, T_UNTYPED,
                         R_READ | R_WRITE | R_DUPLICATE | R_TRANSFER)

    def retype(self, ut_sid: int, type_: str, dst_sid: int) -> int:
        """
        Create an object from an Untyped; the new slot is its MDB child.

        O1 and O2 of the charter in one operation: every canonical object is
        born from an Untyped somebody named, and its storage belongs to that
        Untyped — which is why the capability is installed as a CHILD of the
        Untyped's slot and not as a root.  Resetting the Untyped while the
        child lives is then refusable by looking at the tree (O6).
        """
        ut = self.slots.get(ut_sid)
        dst = self.slots.get(dst_sid)
        if ut is None or dst is None:
            return ERR_INVALID_ARG
        # CPtr 0 is `IRIS_CPTR_NULL` — the null capability names nothing, so
        # invoking it is a malformed request rather than a lookup that failed.
        # The same number means "no capability" at both ends of a retype: as
        # a source it is INVALID_ARG here, and as a destination it is
        # INVALID_ARG below, for the two different reasons.
        if ut_sid == 0:
            return ERR_INVALID_ARG
        # The Untyped is resolved FIRST — `syscall_untyped.c` validates the
        # capability it was invoked on before it looks at where the result is
        # meant to go, so a retype from a slot that no longer holds an Untyped
        # is NOT_FOUND even when the destination is also wrong.
        if ut.empty:
            return ERR_NOT_FOUND
        # TYPE before RIGHTS, which is the opposite of what this model first
        # assumed.  The tree calls it "the type-before-rights fix": a resolver
        # that has identified the capability exactly should say WRONG_TYPE
        # rather than flatten it into something vaguer, and it says so before
        # it considers whether the holder had the right.
        if self.objs[ut.obj].type != T_UNTYPED:
            return ERR_WRONG_TYPE
        # Carving from an Untyped MUTATES it — the bump cursor moves and the
        # child count rises — so it takes WRITE, not READ.
        if not (ut.rights & R_WRITE):
            return ERR_ACCESS_DENIED
        # THEN the destination.  Slot 0 is `IRIS_CPTR_NULL` when addressing
        # the root, so a capability published there could never be invoked —
        # the kernel refuses it "so every published cap is actually
        # invocable", and until the Stage V work encoded that rule it held in
        # two of the four ways a capability can be published.
        if dst_sid == 0:
            return ERR_INVALID_ARG
        if not dst.empty:
            return ERR_ALREADY_EXISTS
        oid = self._new_obj(type_)
        dst.obj = oid
        dst.rights = self.BIRTH_RIGHTS.get(type_, R_ALL)
        dst.badge = 0
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
        # An Untyped is NOT in the tracked population: the driver builds one
        # with `kuntyped_create` over real backing memory, so its destructor is
        # the kernel's own and cannot be hooked to count.  Excluded on both
        # sides, and named here so the exclusion is a decision rather than a
        # coincidence of which branch calls `obj_new`.
        oid = self._new_obj(type_, seeded=(type_ != T_UNTYPED))
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
        # Deriving a slot into ITSELF is a malformed request, not a conflict
        # with what is already there — and the kernel says so before it looks
        # at the slot at all, empty or not.
        if src_sid == dst_sid:
            return ERR_INVALID_ARG
        if src.empty:
            return ERR_NOT_FOUND
        # ── the ORDER of these checks is part of the specification ─────────
        #
        # Authority first, feasibility second.  The kernel refuses a mint
        # without DUPLICATE before it looks at whether the destination is
        # occupied, so a caller with no right to copy the capability does not
        # get to learn anything about the destination slot either.  The model
        # checked occupancy first and the two agreed on the STATE every time
        # — the disagreement is visible only in the return code, which is why
        # the differential compares those too.
        if not (src.rights & R_DUPLICATE):
            return ERR_ACCESS_DENIED

        # Rights are reduced here, and a reduction to NOTHING is refused —
        # before the badge is considered.  `mint 14 17 2 42` from a source
        # carrying READ|DUPLICATE asks for WRITE, which intersects to zero,
        # and the kernel says INVALID_ARG without ever reaching the badge
        # rule that would also have refused it.  Two refusals, and which one
        # the caller is told is part of the specification.
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

        # Feasibility LAST.  Everything above is about authority and about
        # whether the request is well formed, and the kernel settles both
        # before it looks at what is in the destination.
        if not dst.empty:
            return ERR_ALREADY_EXISTS
        dst.obj, dst.rights, dst.badge = src.obj, new_rights, new_badge
        dst.parent = src_sid
        src.children.append(dst_sid)
        return OK

    def move(self, src_sid: int, dst_sid: int) -> int:
        """
        Move the capability AND its place in the derivation tree.

        It is the same capability somewhere else, so its parent, its siblings
        and its children all travel with it: children re-point at the
        destination and the source ends fully empty.  Nothing is derived and
        no reference changes — which is exactly why a move can publish into a
        slot that a mint could not, and why the slot-0 rule has to be stated
        separately for it.  It was not, until Stage V phase 1 encoded the rule
        and asked where it applied.
        """
        src = self.slots.get(src_sid)
        dst = self.slots.get(dst_sid)
        if src is None or dst is None:
            return ERR_INVALID_ARG
        if src_sid == 0 or dst_sid == 0:
            return ERR_INVALID_ARG           # CPtr 0 names nothing, either end
        if src_sid == dst_sid:
            return ERR_INVALID_ARG
        if src.empty:
            return ERR_NOT_FOUND
        if not dst.empty:
            return ERR_ALREADY_EXISTS

        dst.obj, dst.rights, dst.badge = src.obj, src.rights, src.badge
        dst.parent = src.parent
        if src.parent is not None:
            self.slots[src.parent].children.remove(src_sid)
            self.slots[src.parent].children.append(dst_sid)
        dst.children = list(src.children)
        for c in dst.children:
            self.slots[c].parent = dst_sid
        src.obj, src.rights, src.badge, src.parent = None, 0, 0, None
        src.children = []
        return OK

    def syscall_mint(self, src_sid: int, dst_sid: int, rights: int,
                     badge: int) -> int:
        """
        `SYS_CSPACE_MINT` — the derivation as a ring-3 caller reaches it.

        The syscall adds what the primitive does not have to know: CPtr 0 is
        the null capability at both ends, so it is refused as a source and
        refused as a destination, and for the destination the reason is that
        a capability published there could never be invoked.
        """
        if src_sid == 0 or dst_sid == 0:
            return ERR_INVALID_ARG
        return self.mint(src_sid, dst_sid, rights, badge)

    # The wire values `CAP_IDENTIFY` answers: the enumerator's VALUE is the
    # ABI, which is why `kobject.h` keeps KOBJ_PROCESS and KOBJ_VMO reserved
    # after deleting the objects — renumbering would change what every other
    # capability calls itself.
    TYPE_WIRE = {T_NOTIFICATION: 2, T_ENDPOINT: 8, T_CNODE: 9,
                 T_SC: 10, T_UNTYPED: 11, T_REPLY: 12}

    def identify(self, sid: int) -> int:
        """
        `SYS_CAP_IDENTIFY` — what a slot holds, asked of the kernel.

        CPtr 0 is INVALID_ARG, not NOT_FOUND, and the distinction is the one
        that runs through this whole layer: **the same number means two
        different things depending on its position.**  Invoked, 0 is the null
        capability and naming it is malformed.  As an ARGUMENT — the slot
        index `SYS_CNODE_DELETE` takes — 0 is an ordinary slot, and deleting
        it succeeds like deleting any empty slot.
        """
        s = self.slots.get(sid)
        if s is None or sid == 0:
            return ERR_INVALID_ARG
        if s.empty:
            return ERR_NOT_FOUND
        return self.TYPE_WIRE[self.objs[s.obj].type]

    def same_object(self, a_sid: int, b_sid: int) -> int:
        """
        `SYS_CAP_SAME_OBJECT` — do two capabilities name one object?

        The question a holder cannot answer by comparing numbers, which is
        the whole reason it is a syscall: two CPtrs into different CNodes,
        with different rights and different badges, may be the same object,
        and only the kernel knows.
        """
        a = self.slots.get(a_sid)
        b = self.slots.get(b_sid)
        if a is None or b is None or a_sid == 0 or b_sid == 0:
            return ERR_INVALID_ARG      # invoked: the null capability
        if a.empty or b.empty:
            return ERR_NOT_FOUND
        return 1 if a.obj == b.obj else 0

    def syscall_revoke(self, sid: int) -> int:
        """`SYS_CSPACE_REVOKE` — the revoke as ring 3 reaches it."""
        if sid == 0:
            return ERR_INVALID_ARG      # invoked: the null capability
        r = self.revoke(sid)
        if r != OK:
            return r
        # `SYS_CSPACE_REVOKE` answers the COUNT it destroyed, not a status.
        # Zero is a perfectly good answer for a capability nobody derived, so
        # a caller that reads it as a status sees success as failure — which
        # is a mistake this project has already made once, in `objreg`.
        return self.last_revoked

    def syscall_delete(self, sid: int) -> int:
        """
        `SYS_CNODE_DELETE` on the caller's own root.

        Slot 0 IS addressable here — it is arg1, an index, not the invoked
        capability — so deleting it succeeds like deleting any empty slot.
        Nothing can ever be published there, but the slot exists.
        """
        return self.delete(sid)

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

    def rotate(self, dest_sid: int, pivot_sid: int, src_sid: int) -> int:
        """
        `kcnode_slot_rotate` — two relocations, or a swap when dest IS src.

        Nothing is derived and no reference changes: each capability keeps its
        object, its rights, its badge and its place in the derivation tree,
        and only the slot it sits in changes.  Badges TRAVEL rather than being
        arguments, because A8 says a badged capability is never re-badged and
        a badge argument here would exist to be refused.

        Modelled because nothing exercised it.  `sys_cspace_rotate` is inside
        the verification boundary — `syscall_cspace.c` is one of the files
        `spec_driver.c` links — and no trace in `spec-differential`, none of
        the 194481 in `spec-exhaustive`, and no obligation in `spec-prove`
        reached `kcnode_slot_rotate`.  A boundary with an operation nobody
        drives is a boundary that is smaller than it claims.
        """
        dest = self.slots.get(dest_sid)
        pivot = self.slots.get(pivot_sid)
        src = self.slots.get(src_sid)
        if dest is None or pivot is None or src is None:
            return ERR_INVALID_ARG
        # Distinctness is about the SLOT.  A rotate that moved a capability
        # onto itself would clear the node it had just written.
        if src_sid == pivot_sid or dest_sid == pivot_sid:
            return ERR_INVALID_ARG
        if src.empty or pivot.empty:
            return ERR_NOT_FOUND
        if dest_sid != src_sid and not dest.empty:
            return ERR_ALREADY_EXISTS

        def relocate(frm_sid: int, to_sid: int):
            """Move a capability and its place in the tree, slot to slot."""
            frm = self.slots[frm_sid]
            to = self.slots[to_sid]
            to.obj, to.rights, to.badge = frm.obj, frm.rights, frm.badge
            to.parent = frm.parent
            if frm.parent is not None:
                ch = self.slots[frm.parent].children
                ch[ch.index(frm_sid)] = to_sid
            to.children = list(frm.children)
            for c in to.children:
                self.slots[c].parent = to_sid
            frm.obj, frm.rights, frm.badge, frm.parent = None, 0, 0, None
            frm.children = []

        if dest_sid == src_sid:
            # The swap: both are occupied, so one stands aside.  The kernel
            # keeps that temporary on its stack and no reader can observe it;
            # the model gives it a slot id no trace can name.
            TMP = -1
            self.slots[TMP] = Slot()
            relocate(src_sid, TMP)
            relocate(pivot_sid, dest_sid)
            relocate(TMP, pivot_sid)
            del self.slots[TMP]
        else:
            relocate(pivot_sid, dest_sid)
            relocate(src_sid, pivot_sid)
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
            # NOT_FOUND, not OK — and the asymmetry with `delete` (which IS
            # idempotent on an empty slot) is deliberate in the kernel.  A
            # revoke drops `mdb_lock` between batches, which is what makes it
            # preemptible, so the slot it was invoked on can be deleted
            # underneath it by another processor.  `kcnode_slot_revoke_bounded`
            # answers NOT_FOUND when nothing had been revoked yet and
            # otherwise stops and reports what was: "a revoke that destroyed
            # half a subtree and then found its own root gone has still
            # destroyed half a subtree".  The model returned OK; the
            # differential disagreed on its first trace once RETURN CODES were
            # compared and not only states, which is the whole reason they are.
            return ERR_NOT_FOUND
        victims = self._descendants(sid)
        for d in victims:
            self.slots[d].children.clear()
            self._clear(d)
        s.children.clear()
        self.last_revoked = len(victims)
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
        #
        # D-7's target property, as a NUMBER both sides compute differently.
        #
        # `named` is how many distinct objects the slots name — the derivation
        # model's answer, obtained by walking the CSpace.  The driver prints
        # the same figure beside `live`, which is how many the KERNEL's
        # reference counts have not destroyed yet, obtained by counting
        # destructor calls.  The ledger's D-7 row says those two can diverge
        # and records three occasions when they did; putting both in the dump
        # makes every trace in `spec-differential` and all 194481 in
        # `spec-exhaustive` assert that they do not.
        #
        # The model prints them EQUAL by construction, because in the model an
        # object is alive exactly while a slot names it — that is the whole
        # claim.  The kernel is where the number can come out different.
        #
        named = len({self.slots[i].obj for i in self.slots
                     if not self.slots[i].empty
                     and self.slots[i].obj in self.seeded})
        out.append("live %d named %d" % (named, named))
        #
        # D-7's mechanism: how many capabilities are the LAST one to their
        # object.  seL4 answers this from the MDB neighbourhood
        # (`isFinalCapability`); the model answers it by counting, which is
        # ground truth.  The driver prints the kernel's neighbour-local
        # answer beside it, so a disagreement is the predicate being wrong —
        # and the predicate is what has to replace `active_refs` before D-7
        # can be retired at all.
        #
        from collections import Counter
        cnt = Counter(self.slots[i].obj for i in self.slots
                      if not self.slots[i].empty)
        out.append("final %d" % sum(1 for o in cnt if cnt[o] == 1))
        # D-7's shadow comparison, made at every slot delete in the kernel:
        # the MDB predicate and the reference count decided the same thing.
        # The model asserts zero disagreements, which is what retiring the
        # count requires and what the kernel has to demonstrate.
        out.append("shadow 0")
        return "\n".join(out)


# ── the charter's invariants, as checkable properties ───────────────────────
#
# Each returns None when it holds, or a string naming the violation.  These
# are what an Isabelle abstract specification would state as theorems; here
# they are executable, so a random trace can look for a counterexample.

def chk_rights_monotone(m: AuthorityModel):
    """
    A7 — a DERIVED capability's rights are a subset of its parent's.

    "Derived" is the load-bearing word, and the charter has it: the row says
    *"rights are only kept or reduced; MINT never amplifies"*.  A retyped
    object's rights are BORN — `syscall_untyped.c` assigns them by type, and a
    notification is given WAIT by an Untyped that has none.  So an edge whose
    parent is an Untyped is a creation, not a derivation, and this invariant
    has nothing to say about it.

    The first version of this checker did not make the distinction and
    reported the model violating A7 the moment `retype` entered the trace
    language — a checker too strong is as wrong as one too weak, and it is
    wrong in the direction that wastes the most time.
    """
    for sid, s in m.slots.items():
        if s.empty or s.parent is None:
            continue
        p = m.slots[s.parent]
        if p.empty:
            return "slot %d has a parent %d that is empty" % (sid, s.parent)
        if m.objs[p.obj].type == T_UNTYPED and s.obj != p.obj:
            continue                       # a creation, not a derivation
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
        if m.objs[p.obj].type == T_UNTYPED and s.obj != p.obj:
            continue                       # a creation carries no badge down
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
