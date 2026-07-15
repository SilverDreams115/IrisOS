# Resource Ownership & Accounting

Status: **IMPLEMENTED and tested end to end** (runtime T239–T250, all green in
the 246/246 suite; host units 10247/10247).  Companion to
`kernel-object-lifetime.md`, `kernel-capacity-limits.md`,
`memory-object-vmo-policy.md`, `file-backed-memory.md`, and
`service-supervision-model.md`.

This document defines how the microkernel attributes and charges resources, and
records the architecture decision that fixed a caller-charged accounting bug.

---

## The bug this closes

A loader (`svc_load`) creates each child's segment + stack VMOs.  Previously
`SYS_VMO_CREATE` bound the VMO's owner (its quota payer) to **the caller** — the
loader — and the charge released only when the VMO object was destroyed, i.e.
when the CHILD died.  A supervisor holding N children therefore accumulated
~4·N VMOs against its own `KPROCESS_VMO_QUOTA`, capping a supervisor at ~8
concurrent loaded children.  The temporary Fase 28.1 mitigation (raise the quota
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
path).  Fase 29 charges sparse pages to the **VMO's owner**, once, at page
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
| KProcess | spawner (SYS_PROCESS_CREATE) | itself (a domain) | no | destroy frees all its owned resources |
| task / TCB | process | its process | no | freed with the process |
| SchedulingContext | creator | creator process | bindable to a TCB | freed on last handle close |
| **KVMO** | caller / loader | the charge-target (self or a MANAGE'd process) | one owner, charged once | owner's object + page charge released at kvmo_destroy |
| KFrame | VMO map path | (backs a VMO page; no separate quota) | reused across mappings | freed when the VMO frees the page |
| KVSpace | process create | its process | no | freed with the process |
| KFrameMapping / PTE | map syscall | the target VSpace (kslab object) | one per (VSpace, VA) | released on unmap / VSpace teardown |
| KEndpoint | creator | creator's handle table | shared by caps | freed on last handle close |
| KNotification | creator (SYS_NOTIFY_CREATE) | the CALLER (owned_notifications) | shared by caps (e.g. the pager's ONE fault notif) | freed on last handle close; quota released at destroy |
| KReply | EP_CALL (kernel) | the replying endpoint's transaction | one-shot | consumed by SYS_REPLY |
| KCNode | creator | creator's handle table | shared by caps | freed on last handle close |
| KUntyped | boot / retype | holder | retyped into children | children track back to it |
| KIoPort / KIrqCap | cap-create (spawn authority) | holder | — | freed on close |
| IRQ route | IRQ_ROUTE_REGISTER | the routing process | one per line | cleared on teardown |
| file-backed cache VMO | pager's supervisor | its owner (the creator) | shared RO cache | pager restart / owner death releases |
| private writable page | pager fill | the cache/pool VMO's owner | per-fault, not shared | released with the pool VMO |
| shared RO page | pager fill | the cache VMO's owner | shared across targets, charged once | survives while any use exists |
| pager target grant | supervisor | the pager (per-target proc/vs caps) | no | released on pager teardown |
| file grant | VFS | VFS grant table (per session) | no | revoked / session-reset at the VFS |
| service registry entry | svcmgr | svcmgr | no | cleared on unregister / death |

---

## Quota contracts (per-process domain)

| Resource | Limit | Charge point | Release point | Exhaustion result |
|----------|-------|--------------|---------------|-------------------|
| `owned_vmos` | `KPROCESS_VMO_QUOTA` = 32 | `kvmo_bind_owner` | `kvmo_destroy` | `IRIS_ERR_NO_MEMORY`, no object, `global_failed_charges++` |
| `owned_notifications` | `KPROCESS_NOTIFICATION_QUOTA` = 16 | `knotification_bind_owner` | `knotification_destroy` | `IRIS_ERR_NO_MEMORY`, no object |
| `phys_pages_charged` | `KPROCESS_PHYS_PAGES_LIMIT` = 2048 (8 MB) | first allocation of each sparse page (charged to VMO owner) | `kvmo_destroy` (once per page) | `IRIS_ERR_NO_MEMORY`, no page installed |
| live processes | `KPROCESS_MAX_LIVE` = 64 | `kprocess_alloc` (atomic reserve) | `kprocess_destroy` | rolled back atomically, allocation fails |

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
process cap suffice).  The Fase 29 tests use it as the accounting oracle.

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
