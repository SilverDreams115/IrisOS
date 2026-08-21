# CPtr-first services (Phase 8)

> **Historical (Phase 8).**  This document describes the transition, when CPtrs
> and handle IDs shared one argument namespace.  **That transition is over**:
> Stage 4 DELETED the handle table, so a syscall argument is a CPtr or it is
> `INVALID_ARG` — there is no second namespace, no dual resolver, and nothing
> that is "still handle-only".  A CPtr is walked radix-by-radix through the
> CNode tree and addresses exactly one capability, and receive slots are full
> CPtrs too.  Read the README's *CPtr-first addressing* section for what is
> true today; read this for how the slot layout and the per-service bootstrap
> flow came to be.

Phase 8 moves the service ecosystem from "bootstrap bag of KChannel-delivered
handles" to **well-known CSpace slots minted by the spawner before the child
runs**. This document is the operational guide: slot layout, bootstrap flow
per service, namespace rules, and the remaining handle boundary.

## The namespace rule (kernel-enforced)

`handle_id`s are `slot | generation << 10` with generation ≥ 1, so every
live handle is ≥ 1024. Since Phase 8 the dual resolvers
(`cspace_or_handle_resolve_*`, kernel/new_core/src/cspace.c) **enforce** the
split:

| Argument value | Namespace | Behavior |
|---|---|---|
| 0 (`CPTR_NULL`) | — | always invalid |
| 1..1023 | CSpace | resolved against the root CNode ONLY; missing slot → clean error; `ACCESS_DENIED` → hard stop; **no handle-table fallback** |
| ≥ 1024 | handle table | handle table ONLY; **never walks the CSpace** |

History: before the split, the dual resolvers fed handle values into the
radix walker, which masks the index (`cptr & (slot_count-1)`); once Phase 8
populated the low slots, handles like 1027 silently aliased root slot 3
(wrong-object IPC, `WRONG_TYPE` hard stops, broken endpoint close
semantics). Found by smoke T020/T036+ and fixed in this phase; guarded by a
host regression test in `tests/kernel/test_ipc_cspace.c`.

## Well-known slot layout (root CNode, 256 slots)

| Slot | Name | Carries | Rights | Minted by |
|---|---|---|---|---|
| 0 | `CPTR_NULL` | — | — | — |
| 1 | `IRIS_CPTR_SVCMGR_EP` | svcmgr discovery EP (call side) | WRITE | svcmgr → catalog children; init → iris_test |
| 2 | `IRIS_CPTR_VFS_EP` | `"vfs.ep"` (call side) | WRITE | svcmgr; init → iris_test (via lookup + DUPLICATE) |
| 3 | `IRIS_CPTR_CONSOLE_EP` | `"console.ep"` (call side) | WRITE (svcmgr gets +DUPLICATE+TRANSFER to republish) | init → svcmgr/iris_test; svcmgr → catalog children |
| 4 | `IRIS_CPTR_KBD_EP` | `"kbd.ep"` (call side) | WRITE | svcmgr; init → iris_test |
| 5 | `IRIS_CPTR_OWN_EP` | the service's OWN endpoint (recv side) | READ | svcmgr (own_service_ep); init → console |
| 6 | reserved | future: initrd/bootstrap cap (blocked: KBootstrapCap not in the dual resolver) | — | — |
| 7 | `IRIS_CPTR_IRQ_NOTIFY` | IRQ KNotification (WAIT side) | WAIT | svcmgr (irq_notify: kbd) |
| 8–15 | reserved | future core services | — | — |
| 16–29 | reserved | unassigned | — | — |
| 30 | `IRIS_CPTR_TEST_FIX_A` | wrong-type cap (KChannel) | WRITE | init → iris_test only |
| 31 | `IRIS_CPTR_TEST_FIX_B` | endpoint with TRANSFER only | TRANSFER | init → iris_test only |

## Pre-start minting (no barriers, no races)

`svc_load_minted()` (services/common/svc_loader.{h,c}) accepts a
`struct svc_mint` table and performs every `SYS_PROC_CSPACE_MINT` **between
process creation and the moment the child's first thread is resumed**
(`SYS_THREAD_START` until Stage 7 retired it; `SYS_TCB_RESUME` now) — the child
observes its slots populated from its first instruction. This is what allows sh to run with an
EMPTY bootstrap bag and zero `SYS_CHAN_*` call sites: there is no message to
wait for, hence no ordering race and no retry loop.

`SYS_PROC_CSPACE_MINT` (104) is exclusive: an occupied destination slot
fails with `ALREADY_EXISTS` (`kcnode_mint_excl`) — a spawner cannot clobber
a child's slots. Mint failures are non-fatal by design; every consumer
verifies its slot with a PING and prints a smoke-gated marker.

## Bootstrap flow per service (after Phase 8)

| Service | Spawner | CSpace slots received | Bootstrap channel still carries | SYS_CHAN sites |
|---|---|---|---|---:|
| console | init | 5 (own EP recv) | KIoPort + service KChannel (svcmgr's legacy writer) | 4 |
| svcmgr | init | 3 (console.ep, +DUP+XFER) | legacy console channel, SPAWN_CAP | 15 |
| kbd | svcmgr | 1,2,3,4 + 5 (own) + 7 (IRQ notify) | service/reply KChannels (probes), KIoPort, KIrqCap | 5 |
| vfs | svcmgr | 1,2,3,4 + 5 (own) | INITRD_CAP only | 1 |
| sh | svcmgr | 1,2,3,4 | **nothing — empty bag, channel closed unread** | **0** |
| iris_test | init | 1,2,3,4 + 30,31 (fixtures) | SPAWN_CAP only | 19 (test-only, kept) |
| fb | init | — (one-shot painter) | SPAWN_CAP | 1 |
| userboot | kernel | — | (sends init's bootstrap) | 1 |
| init | userboot | — (boot CSpace grants: untyped/vspace/bootcap) | everything (orchestrator) | 58 |

Retired bootstrap kinds (reserved, do not reuse): `0x20` SVCMGR_EP, `0x21`
SERVICE_EP, `0x22` CONSOLE_EP, `0x23` IRQ_NOTIFY.

## The remaining handle boundary

Cap kinds that could not live in CSpace slots **at Phase 8**, because the dual
resolver then covered only IPC objects (endpoint/reply/notification), CNode,
Untyped and Frame:

- **KChannel** (legacy IPC, svcmgr legacy loop and console legacy writer);
- **KIoPort / KIrqCap** (device authority: console, kbd);
- **KBootstrapCap** (initrd/spawn authority: vfs, init, iris_test);
- **KProcess** (spawner-side authority).

All four entries are closed.  KChannel was removed in Phase 13; device and
bootstrap capabilities resolve through CSpace and are published there as MDB
children of the slot that authorised them; and `KProcess` does not exist —
a spawner holds its child's TCB, CNode and VSpace, which it retyped itself.

## Runtime verification

Smoke-gated markers: `[SH] svcmgr/vfs/console/kbd cptr OK`,
`[VFS] console cptr OK`, `[IRIS][TEST] console cptr write OK`.
Runtime tests: T039 (call via slot), T040 (null/wrong-type/denied — no
fallback), T041 (slots resolve with right type; empty slot fails),
T042–T044 (vfs/console/kbd served via slots), T045 (client slots are
WRITE-only: recv denied), T046 (legacy lookup still yields real handles
≥ 1024 that work and close).

## Phase 13 prerequisite — device caps resolve through CSpace

`cspace_or_handle_resolve_obj()` (generic, lifecycle-only ref contract) extends
the dual-resolution model to **device/authority caps** — `KIoPort`, `KIrqCap`,
`KBootstrapCap`. The device-access syscalls (`SYS_IOPORT_IN/OUT`,
`SYS_IRQ_ROUTE_REGISTER`, `SYS_IRQ_ACK`, `SYS_INITRD_VMO/COUNT`,
`SYS_PROCESS_CREATE`, `SYS_CAP_CREATE_IRQCAP/IOPORT`,
`SYS_FRAMEBUFFER_VMO`) now accept a CPtr slot (`<1024`, CSpace-only) or a handle
(`>=1024`, handle-table-only); the namespace split and `ACCESS_DENIED`-no-fallback
contract are preserved (wrong-type CPtr → `ACCESS_DENIED`). This is the
prerequisite that lets device caps be pre-minted into a child's CSpace instead
of delivered over a KChannel — unblocking full KChannel retirement.
Runtime proof: **T069**.  (At the time, `KChannel` and `KProcess` were still
handle-only; both objects are gone.)

(Historical: the handle namespace is gone since Stage 4, and Stage 5 split the
single `KBootstrapCap` into one capability per authority — `SYS_BOOTCAP_RESTRICT`
retired with it.  This section records the Phase 13 step as it was.)
