# CPtr-first services (Fase 8)

Fase 8 moves the service ecosystem from "bootstrap bag of KChannel-delivered
handles" to **well-known CSpace slots minted by the spawner before the child
runs**. This document is the operational guide: slot layout, bootstrap flow
per service, namespace rules, and the remaining handle boundary.

## The namespace rule (kernel-enforced)

`handle_id`s are `slot | generation << 10` with generation ≥ 1, so every
live handle is ≥ 1024. Since Fase 8 the dual resolvers
(`cspace_or_handle_resolve_*`, kernel/new_core/src/cspace.c) **enforce** the
split:

| Argument value | Namespace | Behavior |
|---|---|---|
| 0 (`CPTR_NULL`) | — | always invalid |
| 1..1023 | CSpace | resolved against the root CNode ONLY; missing slot → clean error; `ACCESS_DENIED` → hard stop; **no handle-table fallback** |
| ≥ 1024 | handle table | handle table ONLY; **never walks the CSpace** |

History: before the split, the dual resolvers fed handle values into the
radix walker, which masks the index (`cptr & (slot_count-1)`); once Fase 8
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
process creation and `SYS_THREAD_START`** — the child observes its slots
populated from its first instruction. This is what allows sh to run with an
EMPTY bootstrap bag and zero `SYS_CHAN_*` call sites: there is no message to
wait for, hence no ordering race and no retry loop.

`SYS_PROC_CSPACE_MINT` (104) is exclusive: an occupied destination slot
fails with `ALREADY_EXISTS` (`kcnode_mint_excl`) — a spawner cannot clobber
a child's slots. Mint failures are non-fatal by design; every consumer
verifies its slot with a PING and prints a smoke-gated marker.

## Bootstrap flow per service (after Fase 8)

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

Cap kinds that cannot live in CSpace slots because the dual resolver only
covers IPC objects (endpoint/reply/notification), CNode, Untyped and Frame:

- **KChannel** (legacy IPC, svcmgr legacy loop and console legacy writer);
- **KIoPort / KIrqCap** (device authority: console, kbd);
- **KBootstrapCap** (initrd/spawn authority: vfs, init, iris_test);
- **KProcess** (spawner-side authority).

Services needing those keep a (shrunken) bootstrap one-shot. Extending the
resolver to more types is future work (slot 6 is reserved for the initrd
cap when that happens).

## Runtime verification

Smoke-gated markers: `[SH] svcmgr/vfs/console/kbd cptr OK`,
`[VFS] console cptr OK`, `[IRIS][TEST] console cptr write OK`.
Runtime tests: T039 (call via slot), T040 (null/wrong-type/denied — no
fallback), T041 (slots resolve with right type; empty slot fails),
T042–T044 (vfs/console/kbd served via slots), T045 (client slots are
WRITE-only: recv denied), T046 (legacy lookup still yields real handles
≥ 1024 that work and close).
