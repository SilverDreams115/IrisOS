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

RANK = {
    'mdb_lock': 1,
    'ep->lock': 2,
    'vs->lock': 3,
    'live_lock': 4,
    'sched_list_lock': 5,   # the scheduler's list of live threads
    'cn->lock': 6, 'to_cn->lock': 6, 'from_cn->lock': 6, 'parent_cn->lock': 6,
    'obj->lock': 6, 'n->base.lock': 6, 'pool->lock': 6,
    't->obj_lock': 6, 'target->obj_lock': 6, 'sc->lock': 6, 'r->lock': 6,
    'dom_lock': 7,          # the domain schedule's cursor
    'rq->lock': 8,          # leaf: nothing may be taken under it
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
