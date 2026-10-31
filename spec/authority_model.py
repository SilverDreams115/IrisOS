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
T_FRAME = "frame"

#
# ── The VSpace side, so D-13 is CHECKED and not only proved ────────────────
#
# `inv_D13` — a space that maps an object holds a capability naming it — is
# proved over symbolic state in `spec/prove.py`.  Until these ops existed
# nothing tied it to `kvspace.c`, and the proved side is the one no hardware
# executes: the worse of the two to have wrong, because it looks strongest.
#
# The space's capability does NOT live in the CSpace slots a trace addresses —
# the kernel keeps it in the space's own `map_caps` chunks.  It is modelled as
# a pseudo-slot above every real slot id so that it participates in the MDB
# exactly as it does in the kernel: a revoke reaches it, a delete reparents
# around it, and it keeps the frame alive while it names it.  It is excluded
# from the dump's slot lines and from `final`, because `kcnode_count_final_caps`
# walks the four CNodes and never sees it.
#
MAPCAP_BASE = 10000
NVSPACES = 2                     # as `spec_driver.c` declares
NVFRAMES = 3

OK = 0
ERR_INVALID_ARG = -1
ERR_NOT_FOUND = -3
ERR_ACCESS_DENIED = -5
ERR_ALREADY_EXISTS = -7
ERR_BUSY = -8
ERR_WRONG_TYPE = -9
ERR_WOULD_BLOCK = -13


@dataclass
class Obj:
    """A kernel object. `oid` stands in for the pointer the C uses."""
    oid: int
    type: str
    # The PENDING SIGNAL BITS, for a notification; unused by every other type.
    #
    # The only mutable DATA in this model.  Everything else here is authority
    # — who may do what — and authority is what the slots say.  These sixty-
    # four bits are the one thing a subject can put into the kernel and
    # another subject can take out, which is why they are modelled at all:
    # without them "no information flows" is a claim about return codes only.
    bits: int = 0
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
        # D-13: the one address space, its frames and what it maps.
        self.frames: list = []            # frame index -> oid
        # frame index -> set of (space, VA index) pairs.
        #
        # The space is in the KEY because `kvspace_finalise_orphans` loops over
        # every space a frame is mapped into, and with one space that loop is
        # never entered.  `inv_D13` was written over (space, object) pairs from
        # the start; it was the model and the driver that collapsed to one, so
        # this widens them to the predicate rather than the other way round.
        self.maps: dict = {}
        # The space's capability is PER SPACE, which is what
        # `kvspace_map_cap_has(vs, obj)` asks.  Space 0 keeps the pseudo-slot
        # ids it had (`MAPCAP_BASE + fi`) so nothing already recorded shifts.
        self.nvspaces = NVSPACES

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
        # D-13's sweep, run from the SAME choke point the kernel runs it from:
        # `kcnode_slot_drop_old` is every path that removes a capability, and
        # putting it anywhere else is how the kernel's own revoke was missed.
        self._finalise_orphans()

    def _finalise_orphans(self):
        """
        Unmap wherever the space no longer holds a capability for what it
        mapped — `kvspace_finalise_orphans`, which is the mechanism D-13 asked
        for.

        "Wherever" is one question per (frame, SPACE), and that is the whole
        reason the second space exists here.  With one space the sweep's loop
        over spaces was never entered, and the count-cursor defect that lived
        in exactly that loop — a window that could advance past a space never
        examined — was found by re-reading the C, because no fixture mapped one
        frame into more than one space.  A sweep that clears one space and
        leaves the other now shows up as a different dump.
        """
        for fi in list(self.maps):
            for k in range(NVSPACES):
                mc = self._mapcap(fi, k)
                held = mc in self.slots and not self.slots[mc].empty
                if not held:
                    self.maps[fi] = {(sp, va) for sp, va in self.maps[fi]
                                     if sp != k}

    def _mapcap(self, fi: int, space: int) -> int:
        """The pseudo-slot standing for space `space`'s capability on frame
        `fi`.  Space 0 is `MAPCAP_BASE + fi`, unchanged."""
        return MAPCAP_BASE + space * NVFRAMES + fi

    def _frame_index(self, oid: int):
        for i, o in enumerate(self.frames):
            if o == oid:
                return i
        return None

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
        # A FRAME joins the Untyped in the untracked population for the same
        # reason: the driver builds one with `kframe_alloc` over real backing,
        # so its destructor is the kernel's own and cannot be hooked to count.
        oid = self._new_obj(type_, seeded=(type_ not in (T_UNTYPED, T_FRAME)))
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
        #
        # The PRIMITIVE's order: the self case first, then emptiness.
        #
        # `kcnode_slot_derive` is reached directly and rejects `src == dst`
        # before it reads the slot, so `mint 0 0 7 7` on an empty slot is
        # INVALID_ARG.  The SYSCALL is the other way round — it resolves the
        # source, so the same arguments through `SYS_CSPACE_MINT` answer
        # NOT_FOUND — and `syscall_mint` states that rather than sharing this
        # order.  Making both agree with one order is what the lane refuted
        # twice: once in each direction.
        #
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
        #
        # Emptiness FIRST, then the self case.  The model asked about
        # `src == dst` before resolving the source and answered INVALID_ARG
        # where the kernel answers NOT_FOUND; removing the self case
        # altogether was then wrong in the other direction, because an
        # OCCUPIED self-move is INVALID_ARG and not ALREADY_EXISTS.  Two
        # guesses, both refuted by the lane in consecutive runs.
        #
        # Found only when the generator's dead branches were fixed — `smove`
        # was unreachable in the random lane, so `smove 3 3` had never been
        # generated in any run.  A disagreement that waits for a generator bug
        # to be fixed is a disagreement nobody was looking for.
        #
        if src.empty:
            return ERR_NOT_FOUND
        if src_sid == dst_sid:
            return ERR_INVALID_ARG
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
        #
        # The syscall RESOLVES the source before anything else, so an empty
        # source is NOT_FOUND even when the arguments would also have been
        # refused for naming one slot twice.  The primitive answers the other
        # way; see `mint`.
        #
        src = self.slots.get(src_sid)
        if src is None:
            return ERR_INVALID_ARG
        if src.empty:
            return ERR_NOT_FOUND
        return self.mint(src_sid, dst_sid, rights, badge)

    # The wire values `CAP_IDENTIFY` answers: the enumerator's VALUE is the
    # ABI, which is why `kobject.h` keeps KOBJ_PROCESS and KOBJ_VMO reserved
    # after deleting the objects — renumbering would change what every other
    # capability calls itself.
    # Read off `kobject_type_t`'s enumerators, not guessed: the enum reserves
    # retired values (KOBJ_PROCESS, KOBJ_VMO, KOBJ_INITRD_ENTRY) precisely
    # because the VALUE is the wire type, so the positions are not contiguous
    # and a frame is 15.
    TYPE_WIRE = {T_NOTIFICATION: 2, T_ENDPOINT: 8, T_CNODE: 9,
                 T_SC: 10, T_UNTYPED: 11, T_REPLY: 12, T_FRAME: 15}

    # ── the data channel ───────────────────────────────────────────────────
    #
    # `SYS_NOTIFY_SIGNAL` and `SYS_NOTIFY_POLL`, which are the only pair in
    # this model that moves BITS instead of authority.  They are here so that
    # an information-flow statement has something to be about: a subject
    # confined to its own subtree still shares a notification with whoever
    # derived the capability to it, and what crosses is a word the sender
    # chose.
    #
    # The two rights are NOT the same one.  Signalling needs WRITE, polling
    # needs WAIT (`sys_notify_signal`/`sys_notify_poll`), so a derived
    # capability can carry the power to send and not the power to receive, or
    # the reverse — which is the asymmetry that makes the channel directional
    # and is the reason seL4 splits them.  A model that required one right for
    # both would describe a channel nobody can build.
    #
    # The refusal ORDER is the resolver's and was measured, not assumed:
    # CPtr 0 is malformed (INVALID_ARG) before anything is looked up, an empty
    # slot is NOT_FOUND, then TYPE before RIGHTS — `DUAL_RESOLVE_IPC` checks
    # the tag and only then the mask, so a WRITE-less endpoint capability
    # answers WRONG_TYPE and not ACCESS_DENIED.

    def _resolve_notification(self, sid: int, required: int):
        """The resolver, as a refusal code or a slot.  `None` is "not printed"."""
        if sid >= self.spc:
            return None, None          # the driver's `continue`: root CNode only
        s = self.slots.get(sid)
        if s is None or sid == 0:
            return ERR_INVALID_ARG, None
        if s.empty:
            return ERR_NOT_FOUND, None
        if self.objs[s.obj].type != T_NOTIFICATION:
            return ERR_WRONG_TYPE, None
        if not s.rights & required:
            return ERR_ACCESS_DENIED, None
        return None, s

    # The patterns the driver can send, by index.  A zero pattern is in the
    # table on purpose: signalling nothing SUCCEEDS and leaves the word alone,
    # so a following poll still answers WOULD_BLOCK — the one case where the
    # send is permitted and no information moves at all.
    SIG_PATTERNS = (1, 3, 0x8000000000000000, 0)

    def nsig(self, sid: int, pat_idx: int):
        err, s = self._resolve_notification(sid, R_WRITE)
        if s is None:
            return err
        self.objs[s.obj].bits |= self.SIG_PATTERNS[pat_idx % 4]
        return OK

    def npoll(self, sid: int):
        err, s = self._resolve_notification(sid, R_WAIT)
        if s is None:
            return err
        o = self.objs[s.obj]
        got, o.bits = o.bits, 0        # an exchange, not a read: polling CONSUMES
        if got == 0:
            return ERR_WOULD_BLOCK
        return got

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

    def vframe(self, sid: int):
        """
        A frame with real backing, named by a capability in the root CNode —
        the driver's `vframe`.  `None` means the driver printed nothing
        (`continue`), which is a distinct observable from any error code.
        """
        if sid >= self.spc or len(self.frames) >= NVFRAMES:
            return None
        r = self.seed(sid, T_FRAME, R_READ | R_WRITE | R_DUPLICATE)
        if r == OK:
            self.frames.append(self.slots[sid].obj)
            self.maps[len(self.frames) - 1] = set()
        return r

    def vmap(self, sid: int, va: int, space: int = 0):
        """
        Map through the SLOT, which is what makes the space's capability a
        SIBLING of it (`kcnode_slot_install_sibling`).  A child is what
        `Revoke(S)` would destroy, and then a revoke of the slot a holder
        mapped through would unmap the holder's own page — which is what it
        did, for thirteen pages of a boot.
        """
        if sid not in self.slots or sid >= self.ncnodes * self.spc:
            return None
        s = self.slots[sid]
        if s.empty or self.objs[s.obj].type != T_FRAME:
            return ERR_WRONG_TYPE
        fi = self._frame_index(s.obj)
        if fi is None:
            return ERR_WRONG_TYPE
        # The address, not the frame: a VA already carrying a PTE is BUSY
        # whichever frame put it there, because the kernel checks the walk.
        # Per SPACE, because a walk is a walk of one space's tables — the same
        # VA in two spaces is two PTEs and neither is busy on the other's
        # account.
        space %= NVSPACES
        if any((space, va) in m for m in self.maps.values()):
            return ERR_BUSY
        mc = self._mapcap(fi, space)
        if mc not in self.slots:
            self.slots[mc] = Slot()
        if self.slots[mc].empty:
            # ONE capability per SPACE covers however many times THAT space
            # maps the frame, so a second map into the same space installs
            # nothing new — and a map into the OTHER space installs its own.
            self.slots[mc].obj = s.obj
            # The SOURCE's rights and badge, unchanged — what
            # `kcnode_slot_install_sibling` copies.  They were hardcoded to
            # READ|WRITE|DUPLICATE here, and `spec-exhaustive` caught it as an
            # A7 violation in the model: a map through a slot reduced to READ
            # produced a capability with WRITE, amplifying over the parent it
            # was installed beside.  531441 traces, found at depth 4.
            self.slots[mc].rights = s.rights
            self.slots[mc].badge = s.badge
            # A source that is a ROOT has no parent to share, so the
            # capability becomes its CHILD rather than a second root: two
            # unrelated roots naming one frame is what made
            # `kcnode_slot_is_final_cap` wrong, and this lane is what caught
            # it.  For a root, revoke-unmaps is also the only answer that
            # leaves no mapping outliving every capability.
            par = s.parent if s.parent is not None else sid
            self.slots[mc].parent = par
            self.slots[par].children.append(mc)
        self.maps[fi].add((space, va))
        return OK

    def vunmap(self, fi: int, va: int, space: int = 0):
        """The driver's `vunmap`: by frame index, since it holds the pointer."""
        if fi >= NVFRAMES:
            return None
        space %= NVSPACES
        if fi >= len(self.frames) or fi not in self.maps:
            return ERR_NOT_FOUND
        if (space, va) not in self.maps[fi]:
            #
            # Two different answers, and the kernel distinguishes them:
            # `kframe_unmap_page` searches the space's list by VA, and a VA
            # that matches while naming a DIFFERENT frame is INVALID_ARG —
            # "the VA is occupied by somebody else" — while a VA with no PTE
            # at all is NOT_FOUND.  The model answered NOT_FOUND for both and
            # `vunmap 1 1` disagreed.
            #
            if any((space, va) in self.maps[o] for o in self.maps if o != fi):
                return ERR_INVALID_ARG
            return ERR_NOT_FOUND
        self.maps[fi].discard((space, va))
        # The space's capability goes with THAT space's last mapping of this
        # frame.  Summed over both spaces it would survive its own space's
        # last unmap, which is the authority-without-mapping direction.
        if not any(sp == space for sp, _ in self.maps[fi]):
            mc = self._mapcap(fi, space)
            if mc in self.slots and not self.slots[mc].empty:
                self._clear(mc)
        return OK

    # ── the state, canonically ─────────────────────────────────────────────
    def dump(self) -> str:
        """
        One line per occupied slot, sorted, in the format the C driver prints.
        Objects are renumbered in slot order so the two sides can agree
        without sharing pointers.
        """
        real = [i for i in sorted(self.slots) if i < MAPCAP_BASE]
        canon, nxt = {}, 1
        for sid in real:
            o = self.slots[sid].obj
            if o is not None and o not in canon:
                canon[o] = nxt
                nxt += 1
        out = []
        for sid in real:
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
        named = len({self.slots[i].obj for i in real
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
        # Counted over the REAL slots, but "is this the last one?" is asked of
        # EVERY capability including the space's.
        #
        # Both halves are the kernel's: `kcnode_count_final_caps` walks the
        # four CNodes, so a mapping capability is never itself counted; the
        # predicate it calls is `kcnode_slot_is_final_cap`, which reads the MDB
        # neighbourhood and therefore DOES see one.  Asking only over the real
        # slots made the model call a slot final while the space still held a
        # capability to its object — `spec-differential` disagreed at 10 vs 9.
        total = Counter(self.slots[i].obj for i in self.slots
                        if not self.slots[i].empty)
        out.append("final %d" % sum(1 for i in real
                                    if not self.slots[i].empty
                                    and total[self.slots[i].obj] == 1))
        #
        # D-13, both sides of it, per frame — the lines the driver prints from
        # the real `mapped_count` and the real `kvspace_map_cap_has`.  The
        # invariant is `mapped > 0 -> cap`, and neither side asserts it here:
        # both numbers are computed independently and compared, so a
        # disagreement says which side is wrong instead of only that one is.
        #
        for fi in range(len(self.frames)):
            held = []
            for k in range(NVSPACES):
                mc = self._mapcap(fi, k)
                held.append(1 if (mc in self.slots
                                  and not self.slots[mc].empty) else 0)
            out.append("frame %d mapped %d cap %s"
                       % (fi, len(self.maps.get(fi, ())),
                          " ".join(str(h) for h in held)))
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



def chk_mapping_authority(m):
    """
    D-13, checked from the MAPPING side — independently of `inv_D13`.

    The shared predicate asks, per (space, object), whether `hascap` holds
    wherever `maps` does, reading the pseudo-slot's occupancy.  This walks the
    other way: it starts from what the space has MAPPED and searches the slot
    space for a capability that names that object and lives above
    `MAPCAP_BASE` — which is what "the space holds a capability for it" means
    structurally — and it also requires that capability to be IN the derivation
    tree, because a capability no revoke can reach is not authority a revoke
    can withdraw.

    Two formulations of one invariant is the entire point of `spec/bridge.py`:
    the proved side is the one no hardware executes, so letting it drift is the
    worse of the two failures.
    """
    for fi, vas in m.maps.items():
        if not vas:
            continue
        if fi >= len(m.frames):
            return "frame %d has mappings but no object" % fi
        oid = m.frames[fi]
        #
        # PER SPACE, and that is not a refinement of the wording — it is what
        # makes the check able to fail.  Searching every pseudo-slot for one
        # naming the object passes whenever ANY space still holds a capability,
        # so a sweep that cleared space 0's capability and left its mapping
        # standing would be excused by space 1's.  That is precisely the defect
        # shape the second space was added to reach.
        #
        for space in sorted({sp for sp, _ in vas}):
            n_here = sum(1 for sp, _ in vas if sp == space)
            held = [sid for sid, sl in m.slots.items()
                    if sid >= MAPCAP_BASE and not sl.empty and sl.obj == oid
                    and (sid - MAPCAP_BASE) // NVFRAMES == space]
            if not held:
                return ("frame %d is mapped at %d address(es) in space %d and "
                        "that space holds no capability naming it"
                        % (fi, n_here, space))
            for sid in held:
                sl = m.slots[sid]
                if sl.parent is not None and sid not in m.slots[sl.parent].children:
                    return ("space %d's capability for frame %d is detached "
                            "from the derivation tree" % (space, fi))
    return None

ALL_CHECKS = (
    chk_rights_monotone,
    chk_badge_sealed,
    chk_tree_acyclic,
    chk_links_symmetric,
    chk_derivation_same_object,
    chk_object_liveness,
    # D-13, enforced on every state every lane builds — including all 531441
    # of `spec-exhaustive`'s.  A mapping that outlives the space's capability
    # is access without authority (charter A10), so a trace that produces one
    # is a failure and not a curiosity.
    chk_mapping_authority,
)


def check_all(m: AuthorityModel):
    for c in ALL_CHECKS:
        why = c(m)
        if why:
            return "%s: %s" % (c.__name__, why)
    return None
