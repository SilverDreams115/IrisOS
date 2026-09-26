#!/usr/bin/env python3
"""
check_lock_order.py — enforce the lock hierarchy in the SMP roadmap (§9.1).

A lock-order inversion cannot happen on one core: with a single CPU and IRQ-off
spinlocks, taking two locks in the wrong order deadlocks nothing and no test
can see it.  It becomes a hang the day the second core starts, in whichever
path happened to be running.

So the order is checked statically, from the day it is written down rather than
from the day it can fail.  RANK below IS the roadmap's table; an edge the code
takes that goes UP the list is reported.

The analysis is an approximation and says so: it tracks locks held within one
function and follows calls three hops deep to see what a callee may take.  A
deeper chain, or a lock taken through a function pointer, slips through.  What
it catches is the mistake people actually make — taking a global while holding
a per-object lock.
"""
import re, pathlib, collections, sys

#
# The names here are the SPELLINGS the code uses, because that is all this
# script can match.  A rank for a name nothing takes is a dead row that makes
# the table look like it covers something; a lock the code takes under a name
# that is not here is IGNORED, and an inversion through it is reported as OK.
# The roadmap review found three of the first and sixteen of the second —
# `pool->lock` was ranked while `u->lock`/`ut->lock` were not, the ASID pool is
# `p->lock` and not `pool->lock`, and seven per-subsystem globals were absent
# entirely.  `scripts/check_lock_order.py --audit` now reports both directions.
#
RANK = {
    'mdb_lock': 1,
    'ep->lock': 2,
    'vs->lock': 3, 'kvspace_boot_lock': 3,
    'live_lock': 4,
    'sched_list_lock': 5,   # the scheduler's list of live threads
    # Per-OBJECT locks, one tier: every spelling the tree actually uses.
    'cn->lock': 6, 'to_cn->lock': 6, 'from_cn->lock': 6,
    'src_cn->lock': 6, 'v_cn->lock': 6,
    'n->base.lock': 6,
    't->obj_lock': 6, 'target->obj_lock': 6, 'ft->obj_lock': 6,
    'sc->lock': 6, 't->sched_ctx->lock': 6,
    'r->lock': 6, 'rp->lock': 6,
    'u->lock': 6, 'ut->lock': 6, 'dev->lock': 6,   # KUntyped, incl. device
    'p->lock': 6,                                  # KAsidPool
    'dom_lock': 7,          # the domain schedule's cursor
    'rq->lock': 8,          # leaf: nothing may be taken under it
    # A-39's CNode teardown queue.  A leaf, and BELOW rq->lock on purpose: it
    # is only ever taken with nothing held, so anything taken under it — the
    # mdb_lock the drain needs most of all — goes up the list and is reported.
    'cascade_lock': 9,
    # Per-subsystem leaves, each confined to its own file and never co-held.
    # Distinct ranks rather than one tier so nesting two of them is reported.
    'reap_queue_lock': 10,
    'tlb_lock': 11,
    'irq_lock': 12,
    'pmm_lock': 13,
    'kslab_lock': 14,
    'klog_lock': 15,
}

LOCK   = re.compile(r'(?:irq_)?spinlock_lock\(&\s*([\w\->\.\[\]]+)')
UNLOCK = re.compile(r'(?:irq_)?spinlock_unlock\(&\s*([\w\->\.\[\]]+)')
DEF    = re.compile(r'^[a-zA-Z_][\w \*]*\b([a-z_][a-z0-9_]*)\s*\(')
CALL   = re.compile(r'\b([a-z_][a-z0-9_]*)\s*\(')
SKIP   = ('return', 'if', 'for', 'while', 'else', '}')

def rank(l):
    return RANK.get(l.strip())

def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    files = sorted((root / 'kernel').rglob('*.c'))

    takes = collections.defaultdict(set)
    calls = collections.defaultdict(set)
    for p in files:
        fn = None
        for line in p.read_text().splitlines():
            d = DEF.match(line)
            if d and not line.lstrip().startswith(SKIP):
                fn = d.group(1)
            m = LOCK.search(line)
            if m and fn:
                takes[fn].add(m.group(1))
            if fn:
                calls[fn].update(CALL.findall(line))

    reach = {f: set(v) for f, v in takes.items()}
    for _ in range(3):
        for f in list(calls):
            acc = set(reach.get(f, ()))
            for c in calls[f]:
                acc |= reach.get(c, set())
            reach[f] = acc

    #
    # Drift, in both directions, BEFORE the inversion scan — because an
    # unranked lock makes that scan answer OK about a lock it never looked at,
    # which is how sixteen of them stayed invisible until the roadmap review
    # counted them.  A rank matches by SPELLING, so a new lock, or an old one
    # reached through a new variable name, has to be added here to be checked.
    #
    taken = set()
    for p in files:
        for line in p.read_text().splitlines():
            m = LOCK.search(line)
            if m: taken.add(m.group(1).strip())
    unranked = sorted(l for l in taken if rank(l) is None)
    if unranked:
        for l in unranked:
            print(f"[lockorder] {l} is taken but has no rank — it would be SKIPPED")
        print("[lockorder] RESULT: FAIL (an unranked lock is an unchecked lock)")
        print("[lockorder] Add it to RANK with the order it belongs at, and to "
              "docs/architecture/sel4-convergence-roadmap.md §9.1.")
        return 1
    dead = sorted(r for r in RANK if r not in taken)
    for r in dead:
        print(f"[lockorder] note: '{r}' is ranked but nothing takes it — "
              f"a dead row makes the table look wider than it is")

    bad = 0
    for p in files:
        rel = p.relative_to(root)
        fn = None; held = []
        for i, line in enumerate(p.read_text().splitlines(), 1):
            d = DEF.match(line)
            if d and not line.lstrip().startswith(SKIP):
                fn = d.group(1); held = []
            m = LOCK.search(line)
            if m:
                for h in held:
                    rh, rm = rank(h), rank(m.group(1))
                    if rh and rm and rm <= rh:
                        print(f"[lockorder] {rel}:{i}: holds {h} (rank {rh}), "
                              f"takes {m.group(1)} (rank {rm})")
                        bad += 1
                held.append(m.group(1))
            if held and fn:
                for c in CALL.findall(line):
                    for tl in reach.get(c, ()):
                        for h in held:
                            rh, rt = rank(h), rank(tl)
                            if rh and rt and rt < rh:
                                print(f"[lockorder] {rel}:{i}: holds {h} (rank {rh}), "
                                      f"calls {c}() which takes {tl} (rank {rt})")
                                bad += 1
            u = UNLOCK.search(line)
            if u and u.group(1) in held:
                held.remove(u.group(1))

    if bad:
        print(f"[lockorder] RESULT: FAIL ({bad} inversion(s))")
        print("[lockorder] The order is docs/architecture/sel4-convergence-roadmap.md "
              "§9.1.  Changing it means changing that table and saying why.")
        return 1
    print(f"[lockorder] RESULT: OK ({len(RANK)} ranked locks, no inversions)")
    return 0

if __name__ == '__main__':
    sys.exit(main())
