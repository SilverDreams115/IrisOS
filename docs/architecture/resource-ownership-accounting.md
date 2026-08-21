# Resource Ownership & Accounting

> **Superseded (Stage 7).**  The model this document describes — a per-process
> resource domain that objects are charged against — **no longer exists**.
> `struct KProcess` is deleted, so there is nothing to be a domain; the VMO
> owner relation and `KPROCESS_VMO_QUOTA` went with it in Stage 7-mem, the page
> ceiling and the live-process ceiling in Stage 7 Steps 2-3, and
> `SYS_RESOURCE_INFO` — the syscall that reported all of it — answers
> `NOT_SUPPORTED`.
>
> **What replaced it**: the `Untyped` an allocation names *is* the accounting.
> A VMO's pages, its metadata and its header are carved from the budget passed
> to `SYS_VMO_CREATE`, as are page tables, PML4s, `KVSpace` headers, root
> CNodes, TCBs, mapping records and device capabilities; each carve is a child
> of that Untyped, `SYS_UNTYPED_RESET` refuses while a child lives, and
> resetting reclaims the whole region.  `SYS_UNTYPED_QUERY` reports it — kind
> `ONE` per budget, kind `GLOBAL` for the kslab and failed-charge gauges that
> were never per-process anyway.  See the
> [roadmap's Stage 7](sel4-convergence-roadmap.md) and the README's *Resource
> accounting* section.
>
> Kept because the **decision** it records is still the reason the current model
> looks the way it does: charge the resource to whoever owns it, never to
> whoever ran the syscall.  Stage 7's answer to "who owns it" is "the budget it
> came from", which is the same principle with the invented domain removed.

Status: **HISTORICAL — Phase 29 record.**  Implemented and tested end to end at
the time (runtime T239–T250, all green in the 246/246 suite; host units
10247/10247).  Companion to `kernel-object-lifetime.md`,
`kernel-capacity-limits.md`, `memory-object-vmo-policy.md`,
`file-backed-memory.md`, and `service-supervision-model.md`.

This document defines how the microkernel attributed and charged resources in
Phase 29, and records the architecture decision that fixed a caller-charged
accounting bug.

---

## The bug this closes

A loader (`svc_load`) creates each child's segment + stack VMOs.  Previously
`SYS_VMO_CREATE` bound the VMO's owner (its quota payer) to **the caller** — the
loader — and the charge released only when the VMO object was destroyed, i.e.
when the CHILD died.  A supervisor holding N children therefore accumulated
~4·N VMOs against its own `KPROCESS_VMO_QUOTA`, capping a supervisor at ~8
concurrent loaded children.  The temporary Phase 28.1 mitigation (raise the quota
32→128, grow the kslab 4→16 MB) bought headroom but did **not** fix the
attribution: the child's memory was still charged to whoever loaded it.

The fix does not raise a constant.  It charges each resource to the domain that
**owns** it.

---

## Ownership vocabulary

Six roles, kept deliberately distinct:

| Role | Meaning |
|------|---------|
| **creator** | the actor that ran the creating syscall |
| **owner / payer / resource domain** | the actor the resource logically belongs to and whose budget it is charged against |
| **holder** | a process that holds a capability to the resource |
| **target** | the process / VSpace where the resource is used |
| **supervisor** | an actor that delegates budget and authority |

The core defect was conflating **creator == owner == payer** when it is not
true.  A loader is the creator and holder of a child's image VMO, but the child
is its owner and payer.

---

## Architecture decision — process as resource domain

Three models were considered:

| Model | Advantages | Problems | Decision |
|-------|-----------|----------|----------|
| **A — corrected process-scoped charging** | Every kernel object already has a single natural owning KProcess; budget-delegation authority already exists as *process* authority (a process cap with RIGHT_MANAGE); no new object, no new death/exhaustion/revocation surface. | Needs an explicit payer-selection path so a loader can charge a child. | **CHOSEN.** |
| **B — explicit KResourceDomain object** | Independent budgets for a shared subtree; a supervisor could cap total consumption of a group. | Large new kernel object + capability + rights + delegation + revocation, all of which must be correct under death and exhaustion — high risk to the baseline; **no current consumer** needs a domain distinct from "the owning process" (pager cache → pager; child image → child; shared RO page → the VMO's owner). | Rejected — deferred to when containers / sandboxes / SMP need subtree budgets. |
| **C — untyped as the only memory authority** | Purest capability story. | Hides other genuinely-bounded capacities (notifications, tasks, mappings) behind one number; a large rework of the current model. | Rejected. |

**Decision: Model A.**  *A KProcess **is** a resource domain.*  Every object is
charged to the KProcess that logically owns it (its payer), and the payer is
selected by **explicit capability authority** at creation: default self,
overridable to a process the caller holds RIGHT_MANAGE on.  This reuses process
authority as budget authority instead of inventing a parallel authority system,
and adds no new object type — consistent with IRIS's principle of not adding
mechanism without a consumer (the same principle that built the user pager with
zero new syscalls).  Budget delegation = holding a process cap with RIGHT_MANAGE.

---

## The payer-selection mechanism

`SYS_VMO_CREATE_FOR(size, charge_target)` (syscall 109) — the additive core of
the fix:

- `charge_target` is a CPtr/handle to a KProcess the caller holds RIGHT_MANAGE
  on (the same authority `SYS_VMO_MAP_INTO` requires to map into that process);
- the VMO OBJECT quota **and** its sparse physical pages are charged to
  `charge_target`, not to the caller;
- the handle is installed in the CALLER's table — the loader is the *holder*,
  the child is the *owner/payer*.

`SYS_VMO_CREATE(size)` keeps its 1-arg ABI (charges self) unchanged, so every
existing caller is unaffected.

`svc_load` creates the child process **first**, then creates the child's segment
and stack VMOs with `SYS_VMO_CREATE_FOR(size, child_proc)`.  The loader's own
`owned_vmos` and `phys_pages_charged` stay flat regardless of how many children
it launches.

### Physical pages follow the VMO owner

Sparse-VMO physical pages were previously charged to whichever process mapped
the VMO **first** (in the loader flow, the loader — which mapped the segment
into its own window to fill it, then never released the charge on the happy
path).  Phase 29 charges sparse pages to the **VMO's owner**, once, at page
allocation (`sys_vmo_map` / `sys_vmo_map_into` / `sys_vmo_map_page` all charge
`kvmo_owner(v)`), and releases them at `kvmo_destroy` (one release per allocated
page).  So:

- a loader filling a child's segment VMO in its own window charges the CHILD;
- a shared VMO's pages are paid **once** by its owner — extra targets that map
  it do not re-charge (the second mapper finds the pages already allocated);
- unmapping / closing never strands the charge on the mapper.

The lightweight per-mapping objects (KFrameMapping nodes + PTEs) are kslab
objects bounded by global kslab capacity, not per-process quota — the *memory*
is charged to the owner once; the *mapping* is a cheap kslab object.

---

## Kernel object ownership table

| Object | Creator | Owner / payer | Shared behavior | Death behavior |
|--------|---------|---------------|-----------------|----------------|
| ~~KProcess~~ | — | — | — | **the object is deleted (Stage 7-proc); a process is threads sharing a CSpace and a VSpace** |
| task / TCB | retyped from an Untyped by its spawner | that Untyped | no | the block returns to the budget when the last capability goes |
| SchedulingContext | retyped from an Untyped | that Untyped | bindable to a TCB | freed on last capability |
| **KVMO** | caller / loader | the charge-target (self or a MANAGE'd process) | one owner, charged once | owner's object + page charge released at kvmo_destroy |
| KFrame | VMO map path | (backs a VMO page; no separate quota) | reused across mappings | freed when the VMO frees the page |
| KVSpace | retyped by its holder (`IRIS_KOBJ_VSPACE`) | the Untyped it came from | named by `SYS_TCB_CONFIGURE`, and several threads may share it | comes down when its LAST CAPABILITY goes, not when a thread dies (Stage 7-proc) |
| KFrameMapping / PTE | map syscall | the target VSpace (kslab object) | one per (VSpace, VA) | released on unmap / VSpace teardown |
| KEndpoint | retyped from an Untyped | that Untyped | shared by caps | freed on last capability |
| KNotification | creator (retype from Untyped) | no numeric owner charge since Phase S1 — the Untyped it was retyped from is the budget | shared by caps (e.g. the pager's ONE fault notif) | freed on last reference |
| KReply | EP_CALL (kernel) | the replying endpoint's transaction | one-shot | consumed by SYS_REPLY |
| KCNode | retyped from an Untyped | that Untyped | shared by caps; a slot naming its OWN CNode takes no active reference | freed on last EXTERNAL capability |
| KUntyped | boot / retype | holder | retyped into children | children track back to it |
| KIoPort / KIrqCap | cap-create (spawn authority) | holder | — | freed on close |
| IRQ route | IRQ_ROUTE_REGISTER | the NOTIFICATION the route is bound to (Stage 7-mem) | one per line | cleared when that notification goes |
| file-backed cache VMO | pager's supervisor | its owner (the creator) | shared RO cache | pager restart / owner death releases |
| private writable page | pager fill | the cache/pool VMO's owner | per-fault, not shared | released with the pool VMO |
| shared RO page | pager fill | the cache VMO's owner | shared across targets, charged once | survives while any use exists |
| pager target grant | supervisor | the pager (per-target THREAD + VSpace caps; no process capability, Stage 7) | no | released on pager teardown |
| file grant | VFS | VFS grant table (per session) | no | revoked / session-reset at the VFS |
| service registry entry | svcmgr | svcmgr | no | cleared on unregister / death |

---

## Quota contracts (per-process domain) — all retired

Every row below is now history; the column that matters is the last one, which
already said "the Untyped runs out" for two of the three before the third
followed.

| Resource | Limit | Charge point | Release point | Exhaustion result |
|----------|-------|--------------|---------------|-------------------|
| `owned_vmos` | **deleted (Stage 7-mem)** with the owner relation — was `KPROCESS_VMO_QUOTA` = 32 | `kvmo_bind_owner` | `kvmo_destroy` | the Untyped the VMO was carved from runs out |
| `phys_pages_charged` | none since **Stage 7 Step 2** (`KPROCESS_PHYS_PAGES_LIMIT` reports 0) | first allocation of each sparse page (charged to VMO owner) | `kvmo_destroy` (once per page) | the Untyped the VMO names runs out; the counter is instrumentation |
| live processes | none since **Stage 7 Step 3** | `kprocess_alloc_from` (a child block of the creator's Untyped) | `kprocess_destroy` | the creator's budget runs out, or the PCID pool (1–4094) does; the live count is instrumentation |

The notification quota (`KPROCESS_NOTIFICATION_QUOTA` = 16, charged at
`knotification_bind_owner`) was **retired in Phase S1** together with the
`owned_notifications` counter: the capacity to create a notification is holding
Untyped memory plus a CSpace slot, not a numeric kernel budget.

The page quota and the live-process ceiling retired the same way in Stage 7,
for the same reason and in the same shape: the counters stay as reportable
gauges, the refusals are gone, and what answers "out of memory" is the region
somebody delegated.  `KPROCESS_VMO_QUOTA` is the last numeric ceiling here, and
it retires with the `KVMO` object itself (memory server) — a VMO is still the
one principal object with no Untyped ancestor.
`SYS_RESOURCE_INFO` keeps the `notifs_*` fields additive and frozen at 0 (see
the ledger's TRANSITIONAL_DIAGNOSTICS row).

Every acquire updates a monotonic **high-water mark** (`*_hwm`, never decreases)
and, on rejection, `global_failed_charges`.  A provisional charge rolled back on
a later failure in the same operation increments `global_rollbacks`.

`KPROCESS_VMO_QUOTA` restored 128 → **32**: now that children pay their own
VMOs, 32 is a genuine per-domain ceiling on how many VMOs one process owns, not
a proxy for how many children a supervisor can launch.

---

## Failure atomicity

Every reservation follows: validate authority → validate payer → reserve quota →
allocate object → initialize → publish capability → commit.  On any failure the
allocation and the provisional charge are rolled back, no capability is
published, and no counter drifts.  Proven by T246 (VMO quota), the host
fault-injection tests (`kslab_fail_after` across KEndpoint/KReply/KFrame — object
creation is atomic when kslab returns NULL), and T250 under seeded load.

---

## Observability

`SYS_RESOURCE_INFO(proc_handle, out)` (syscall 110) writes a versioned,
size-validated `struct iris_resource_info`: per-domain usage / limit /
high-water for VMOs, notifications and pages, plus system-wide
`global_failed_charges`, `global_rollbacks`, and kslab `used` / `total` /
high-water / `alloc_failures`.  Additive and read-only (any rights on a non-self
process cap suffice).  The Phase 29 tests use it as the accounting oracle.

---

## Invariants (proved by T239–T250)

```
Q1  Creator, owner, payer and holder have separate contracts.
Q2  No object is accidentally charged to the caller.
Q3  Payer is selected by explicit authority (RIGHT_MANAGE).
Q4  A child pays its own resources.
Q5  A supervisor does not accumulate charge for independent children.
Q6  A shared object is charged exactly once.
Q7  Each mapping charges pages to the owner; mapping nodes are per-VSpace.
Q8  Deriving/minting caps does not re-charge the object.
Q9  Transferring a cap does not transfer the debt.
Q10 Retain/release do not double-count accounting.
Q11 Destroy releases exactly one charge.
Q12 Process death releases all its own resources.
Q13 Independent children are independent domains.
Q14 Target death does not charge cleanup to the pager.
Q15 Pager death releases its private pages per contract.
Q16 Shared RO pages survive while any use exists.
Q17 A private writable page belongs to a target/owner.
Q18 VMO-backed mappings are not charged to the loader.
Q19 The pager's shared notification has a stable payer.
Q20 Quota exhaustion returns a clean error.
Q21 Quota exhaustion leaves no partial object.
Q22 Quota counters never underflow/overflow.
Q23 Usage returns to baseline after cleanup.
Q24 High-water never decreases.
Q25 Resource-accounting rights are monotonic.
Q26 A child cannot raise its own limit.
Q27 Revocation does not free still-active resources without contract.
Q28 Kslab exhaustion produces an explicit diagnostic.
Q29 No wedge on exhausted capacity.
Q30 Full file-backed regression keeps authority and content.
Q31 No CSpace/VSpace ghost.
Q32 No endpoint/notification/KReply drift.
Q33 No VMO/frame/mapping drift.
Q34 No process/task drift.
Q35 Deterministic stress keeps accounting exact.
```

## Tests

| Test | Scenario | Accounting boundary |
|------|----------|---------------------|
| T239 | resource ownership manifest | payer/charge/release per object; CREATE self vs CREATE_FOR child |
| T240 | loader creates 1/8/16/32 children + push to limit | supervisor VMO usage flat; child image charged to child; process-limit is the real ceiling |
| T241 | VMO payer & child ownership | MANAGE authority, wrong-type, dead target |
| T242 | shared VMO single-charge | charged once; dup does not re-charge |
| T243 | mapping target-charge | page charged once to owner; mapping released on unmap |
| T244 | pager cache/private accounting | no supervisor page/VMO leak after pager death |
| T245 | independent children | killing one frees only its charges |
| T246 | quota exhaustion atomicity | NO_MEMORY, no object, fail counter, recovery, hwm pinned |
| T247 | delegation rights monotonicity | no-MANAGE denied; MANAGE unrecoverable |
| T248 | kslab capacity | used ≤ total, no failures, bump-monotone |
| T249 | file-backed regression | baseline exact under multi-target file-backed load |
| T250 | deterministic resource stress | usage baseline, counters coherent, hwm monotone per round |
