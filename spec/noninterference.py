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

And since the notification bits were modelled, it is also a statement about
DATA.  `nsig`/`npoll` move sixty-four bits a sender chose, `npoll` returns
them AS its result code, and so the machinery that compares A's answers
compares the bits A receives without needing anything added to it.  That turns
the property into the one worth having:

    A's answers AND the bits A polls are the same whether or not B ran.

That upgrade brought a PRECONDITION with it that result codes alone did not
need.  Two subjects are disjoint SUBTREES, and two disjoint subtrees can still
name the same object — a notification reached through two unrelated roots is
shared, and then B signalling genuinely changes what A polls.  That is the
channel working, not a leak, so the subjects here must be disjoint in their
OBJECTS and not only in their slots.  With the objects allowed to overlap this
lane fails, which is checked below: `--shared` drops the requirement and the
interference appears, because otherwise the precondition would be an
assumption nobody had tested.

What this still is NOT: a statement about SYNCHRONOUS IPC.  A notification
needs no scheduler, which is why its contents can be modelled here at all; an
endpoint message is copied between two threads that must be running, and
nothing in `spec/` has threads.  So a flow through what an endpoint SENDS is
still outside this.  That bound is stated rather than left for a reader to
discover, because "non-interference, proved" is the kind of phrase that stops
people looking.
"""
import copy
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from authority_model import (AuthorityModel, R_READ, R_DUPLICATE, R_ALL,  # noqa: E402
                             R_WRITE, R_WAIT, T_NOTIFICATION, MAPCAP_BASE)
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


def objects_of(m, slots):
    """The OBJECTS a set of slots names.  Pseudo-slots excluded: they are the
    mapping records D-13 added, not capabilities a subject invokes."""
    return {m.slots[i].obj for i in slots
            if i < MAPCAP_BASE and not m.slots[i].empty}


def receiver_slots(m, slots):
    """Slots in `slots` that can both send and take bits: a notification in the
    root CNode carrying WRITE and WAIT.  `sys_notify_signal` needs the first and
    `sys_notify_poll` the second, so only a capability with both makes a
    subject a full participant in the channel."""
    out = []
    for i in sorted(slots):
        if not (0 < i < SPC) or i >= MAPCAP_BASE:
            continue
        sl = m.slots[i]
        if sl.empty or m.objs[sl.obj].type != T_NOTIFICATION:
            continue
        if sl.rights & R_WRITE and sl.rights & R_WAIT:
            out.append(i)
    return out


def two_subjects(m, rng):
    """Two DISJOINT subtrees, each with something in it, or None.

    Disjoint in SLOTS and in OBJECTS both — see the module docstring: a shared
    notification is a channel the capabilities grant, and a theorem that
    ignored it would be reporting a leak where there is a feature.
    """
    roots = [i for i in sorted(m.slots)
             if i < MAPCAP_BASE and not m.slots[i].empty
             and m.slots[i].parent is None]
    rng.shuffle(roots)
    best = None
    for a in roots:
        sa = subtree(m, a)
        for b in roots:
            if b == a:
                continue
            sb = subtree(m, b)
            if sa & sb:
                continue
            if objects_of(m, sa) & objects_of(m, sb):
                continue
            # Among valid pairs, prefer one where A can actually RECEIVE: a
            # notification it may both signal and poll.  Without this the
            # claim "and the bits it polls" was nearly empty — 1 of 378 polls
            # inside a subject returned any bits, the rest being NOT_FOUND,
            # WRONG_TYPE or ACCESS_DENIED.  Measured, then fixed.
            if receiver_slots(m, sa):
                return a, sa, b, sb
            if best is None:
                best = (a, sa, b, sb)
    return best


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
        elif k < 0.62:
            out.append(("delete", rng.choice(xs)))
        elif k < 0.78:
            out.append(("revoke", rng.choice(xs)))
        else:
            # The DATA operations.  Restricted to the root CNode because that
            # is where a CPtr resolves, and drawn from this subject's own slots
            # like everything else here — B signalling through a capability it
            # holds is precisely the action that must not reach A.
            inroot = [x for x in xs if 0 < x < SPC]
            if not inroot:
                out.append(("delete", rng.choice(xs)))
            else:
                # Emitted as a UNIT on ONE slot: signal, then take.  Separately
                # drawn ops almost never pair up on the same capability, and a
                # take with nothing pending observes WOULD_BLOCK rather than
                # any data — which is how the first version of this came to
                # make a claim about bits that only 1 operation in 378 tested.
                sid = rng.choice(inroot)
                out.append(("nsig", sid, rng.randrange(4)))
                out.append(("npoll", sid))
    return out


def apply_op(m, op):
    if op[0] == "mint":
        return m.mint(op[1], op[2], op[3], op[4])
    if op[0] == "delete":
        return m.delete(op[1])
    if op[0] == "nsig":
        return m.nsig(op[1], op[2])
    if op[0] == "npoll":
        # The BITS come back as the result, so they land in A's observation
        # sequence with no special case: the comparison is already over data.
        return m.npoll(op[1])
    return m.revoke(op[1])


def observations(m, a_ops, b_ops=(), rng=None):
    """
    A's result codes, with B's operations interleaved.

    B's answers are DISCARDED: they are B's observations, not A's, and the
    claim is about what A is told.

    `rng` picks the interleaving; `None` means B acts as EARLY as possible,
    before every one of A's operations.  That case exists because it is the
    one most likely to show a flow, and because the first version of this
    function chose the interleaving with `random.Random(len(seen))` — a FRESH
    generator seeded with the position, which is not a random choice at all.
    The sequence it produces is fixed (0.844, 0.134, 0.956, 0.238, ...), so B
    acted before A's second and fourth operations and never before its first,
    identically for all 300 pairs.  A subject with one observation could
    therefore never be reached, which is exactly the shape the data-channel
    control needs.
    """
    seen = []
    bi = list(b_ops)
    for op in a_ops:
        # B acts between A's operations — the shape that matters, because a
        # subject that can only act before A starts cannot signal at all.
        if bi and (rng is None or rng.random() < 0.5):
            apply_op(m, bi.pop(0))
        seen.append(apply_op(m, op))
    for op in bi:
        apply_op(m, op)
    return seen


def data_channel_control():
    """
    The precondition, VIOLATED on purpose, as a built-in control.

    Two tries at this were worthless and both printed like evidence.  Allowing
    the subjects to share an object changed nothing, because the search returns
    the first disjoint pair it finds and that is the same pair either way — it
    reported the identical 278 pairs and no interference.  Hunting for a
    sharing pair found 4 states in 300 and still none interfered, because what
    has to be shared is not any object: it is a NOTIFICATION, with WAIT on one
    side and WRITE on the other.  Random traces do not build that.

    So it is built here, and the kernel was asked the same question directly
    (`spec_driver`: ut 1 ; retype 1 1 2 ; mint 2 3 6 0 ; mint 2 4 16 0 ;
    delete 2 ; delete 1 ; same 3 4 -> 1 ; nsig 3 1 -> 0 ; npoll 4 -> 3).

    Slot 3 and slot 4 name ONE notification and are both MDB roots, which is
    what deleting their common ancestors leaves behind — a delete reparents
    children to the deleted slot's parent, so deleting the notification's own
    slot and then the Untyped's promotes both derived capabilities to roots.
    Slot 3 holds WRITE and may only send; slot 4 holds WAIT and may only take.
    They are two disjoint subtrees that share an object, and the bits cross.

    This lane FAILS if they do not, because then the object-disjointness
    requirement above is excluding nothing and the main result is weaker than
    its own wording.
    """
    m = AuthorityModel(NCNODES, SPC)
    if (m.install_untyped(1) or m.retype(1, T_NOTIFICATION, 2)
            or m.mint(2, 3, R_WRITE | R_DUPLICATE, 0)
            or m.mint(2, 4, R_WAIT, 0)
            or m.delete(2) or m.delete(1)):
        return "the control state could not be built"

    a, b = 4, 3                       # A takes, B sends
    if m.slots[a].parent is not None or m.slots[b].parent is not None:
        return "slots %d and %d are not both MDB roots" % (a, b)
    if subtree(m, a) & subtree(m, b):
        return "the two subtrees are not slot-disjoint"
    if not objects_of(m, {a}) & objects_of(m, {b}):
        return "the two subjects do not share an object after all"

    alone = observations(copy.deepcopy(m), [("npoll", a)])
    together = observations(copy.deepcopy(m), [("npoll", a)],
                            [("nsig", b, 1)])
    if alone == together:
        return ("B signalled a notification A polls and A observed %s either "
                "way — the data channel does not cross subjects, so the "
                "precondition excludes nothing" % (alone,))
    return None


def data_flow_phase(n, rng):
    """
    The data-flow claim, on states that can actually SHOW a flow.

    Measured before this existed: of 285 object-disjoint pairs drawn from
    random traces, exactly ONE had A able to receive and B able to send; in 236
    neither could do either.  So "and the bits it polls" rested on a single
    configuration, and a model sabotaged to let `npoll` take the bits of EVERY
    notification — a textbook cross-subject leak — passed all 285 pairs.

    Random traces do not build two subjects that both hold notifications, so
    this builds them: two Untypeds, a notification carved from each, and the
    Untypeds deleted so each notification's capability is left an MDB root.
    Both carry full birth rights, so each subject can both send and take, and
    the two notifications are DISTINCT objects in disjoint subtrees — the
    configuration where a flow is possible and must not happen.

    The kernel was asked the same thing directly (`spec_driver`: ut 1 ;
    retype 1 1 2 ; delete 1 ; ut 3 ; retype 3 1 4 ; delete 3 ; same 2 4 -> 0 ;
    nsig 2 1 -> 0 ; npoll 4 -> -13 ; npoll 2 -> 3): signalling A's
    notification does not appear in B's poll, and A's own bits are still
    there afterwards.
    """
    pairs = bad = 0
    for _ in range(n):
        m, _r = run_model(gen_trace(rng, rng.randint(6, 24)))
        # Built over the noise rather than instead of it: `install_untyped`
        # replaces an occupied slot, so the construction lands either way.
        if (m.install_untyped(1) or m.retype(1, T_NOTIFICATION, 2) or m.delete(1)
                or m.install_untyped(3) or m.retype(3, T_NOTIFICATION, 4)
                or m.delete(3)):
            continue
        sa, sb = subtree(m, 2), subtree(m, 4)
        if sa & sb or objects_of(m, sa) & objects_of(m, sb):
            continue
        if not receiver_slots(m, sa) or not receiver_slots(m, sb):
            continue
        pairs += 1

        a_ops = [("nsig", 2, rng.randrange(4)), ("npoll", 2)]
        b_ops = [("nsig", 4, rng.randrange(4)), ("npoll", 4)]
        # A's own authority operations go in too, so the flow is checked
        # against a subject that is doing other things at the same time.
        a_ops += ops_within(rng, sa, rng.randint(0, 2))

        alone = observations(copy.deepcopy(m), a_ops)
        early = observations(copy.deepcopy(m), a_ops, b_ops)
        mixed = observations(copy.deepcopy(m), a_ops, b_ops, rng)
        if alone != early or alone != mixed:
            bad += 1
            if bad <= 3:
                print("noninterference: DATA flowed from subject 4 to "
                      "subject 2")
                print("  A alone      %s" % alone)
                print("  B early      %s" % early)
                print("  B interleaved %s" % mixed)
    return pairs, bad


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    n = int(args[0]) if args else 300
    rng = random.Random(0xF10FACE)

    # The control runs FIRST and unconditionally: if violating the precondition
    # does not produce interference, nothing below is worth reading.
    why = data_channel_control()
    if why is not None:
        print("noninterference: CONTROL FAILED — %s" % why)
        return 1
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
        # Two interleavings, because the claim is about every one of them and a
        # single sample is the weakest possible evidence for that: B as early
        # as it can act, and B placed by the lane's own generator.
        early = observations(copy.deepcopy(base), a_ops, b_ops)
        mixed = observations(copy.deepcopy(base), a_ops, b_ops, rng)
        together = early if early != alone else mixed

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

    dpairs, dbad = data_flow_phase(max(60, n // 4), rng)
    if dbad:
        print("\nnoninterference: %d of %d constructed data-flow pairs LEAKED"
              % (dbad, dpairs))
        return 1
    if dpairs == 0:
        print("\nnoninterference: the data-flow phase built NO usable pair, so "
              "it checked nothing")
        return 1
    print("noninterference: %d object-disjoint subject pairs over %d states — a "
          "subject's own result codes AND the notification bits it polls are "
          "identical whether or not the other subject ran, so neither what it "
          "is TOLD nor what it RECEIVES depends on what the other did. Flows "
          "through synchronous IPC are outside this: an endpoint message needs "
          "two running threads and nothing here has threads. The control "
          "passed: two subjects sharing one notification DO interfere, so the "
          "object-disjointness precondition is excluding something real. And "
          "%d CONSTRUCTED pairs, each subject holding a notification it can "
          "both signal and poll, showed no flow between the two."
          % (pairs, n, dpairs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
