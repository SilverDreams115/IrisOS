# Badges & sender identity (Fase 9)

Fase 9 gives IRIS seL4-style **sender identity**: every endpoint message
carries a kernel-stamped badge identifying the *capability* the sender
invoked. This closes the S0 P1 debt "no sender identity" and is the
foundation for service death/relookup, secure REGISTER, per-client state
and the retirement of the svcmgr legacy loop.

## Model

- A **badge** is per-capability metadata, not a property of the endpoint:
  two caps to the same endpoint can carry different badges.
- **Where it lives**: in the CNode slot (`KCSlot.badge`, 64-bit storage)
  for CPtr-namespace caps, and in a parallel `uint32_t badge[slot]` array
  inside `HandleTable` for handle-namespace caps (kept out of
  `HandleEntry` so `sizeof(KProcess)` stays within the largest kslab
  class). The effective badge space is **32 bits** — it must fit the
  `SYS_PROC_CSPACE_MINT` arg3 packing.
- **Badge 0 = unbadged** (masters, legacy caps). Servers treat it as
  "unidentified legacy client".
- **Rights and badge are orthogonal**: rights gate what an invocation may
  do; the badge says which cap was used. `ACCESS_DENIED` remains a hard
  stop on both namespaces, badge or no badge.

## Delivery (anti-spoofing)

`struct IrisMsg` grew to 72 bytes with a final `uint64_t sender_badge`
field (offsets of all prior fields unchanged — asm consumers verified by
`_Static_assert`s in `iris/ipc_msg.h`).

- On **EP_SEND / EP_NB_SEND / EP_CALL** the kernel resolves the invoked
  cap through the badge-aware dual resolver
  (`cspace_or_handle_resolve_endpoint_badged`) and **overwrites**
  `msg.sender_badge` with the cap's badge after copying the message from
  user space. Whatever the sender wrote there is discarded — a badge can
  never be forged from payload (runtime-proven by T051).
- On **EP_RECV / EP_NB_RECV** the receiver reads the stamped value.
- On **SYS_REPLY** the kernel forces `sender_badge = 0` in the caller's
  reply: reply identity is implied by the one-shot KReply, and a server
  cannot spoof a badge into its caller.

## Minting and derivation rules

`SYS_PROC_CSPACE_MINT(proc, slot, src, arg3)` packs arg3 as
`rights | badge << 32`:

| Case | Result |
|---|---|
| badge 0 | inherit the source cap's badge (preservation) |
| badge != 0, source unbadged, source is endpoint/notification | assign the badge |
| badge != 0, source already badged differently | **ACCESS_DENIED** — a badged cap can never be re-badged (no identity forging by holders) |
| badge != 0, wrong object type | INVALID_ARG |
| occupied destination slot | ALREADY_EXISTS (no clobber) |

Preservation everywhere else: `SYS_HANDLE_DUP` copies the badge;
CNode MOVE/SWAP preserve slot badges; `SYS_CSPACE_RESOLVE` materializes a
slot into a handle **with** its badge; cap transfer (send-side attach and
reply-cap transfer) carries the badge through staging
(`syscall_ipc_stage_cap_badged` → `task.ep_cap_badge` →
`syscall_ipc_deliver_cap_badged`). Closing a cap never affects the badge
of any other cap; slot/handle reuse always clears the badge.

## Trust model (current phase)

Badges are authentic for the **spawner-minted well-known slots**: init and
svcmgr hold the unbadged masters and stamp each child's slots 1–4 with the
child's identity badge (`IRIS_BADGE_SVC(service_id)`; iris_test =
`IRIS_BADGE_IRIS_TEST`). The no-re-badge rule prevents any badged holder
from forging another identity.

**Known boundary (documented debt):** name lookup (`.ep`) still returns
unbadged caps with `WRITE|DUPLICATE`, so a holder of a looked-up master
dup could mint itself an arbitrary badge. Tightening lookup grants (now
possible, since svcmgr sees the requester's badge) belongs to the service
lifecycle phase together with death/relookup.

## Well-known badges (iris/endpoint_proto.h)

| Badge | Holder |
|---|---|
| 0 | unbadged / legacy / masters |
| `IRIS_BADGE_SVC(id)` = 0x100+id | catalog services (kbd 0x101, vfs 0x102, sh 0x103) |
| `IRIS_BADGE_SVCMGR` = 0x110 | reserved for svcmgr |
| `IRIS_BADGE_IRIS_TEST` = 0x1F0 | iris_test (slots minted by init) |
| `IRIS_BADGE_TEST_B` = 0xB2 | iris_test fixture slot 28 (second cap to the svcmgr EP) |

## PING convention

Every core EP server (svcmgr, vfs, console, kbd — the last one in
assembly) replies to `IRIS_EP_OP_PING` with
`words[1] = observed sender_badge`, making identity verifiable end to end.

## Runtime coverage

| Test | Proves |
|---|---|
| T047–T050 | svcmgr/vfs/console/kbd each observe `IRIS_BADGE_IRIS_TEST` on the minted slots |
| T051 | payload spoofing impossible (forged field overwritten by kernel) |
| T052 | legacy lookup caps stay unbadged (badge 0) and functional |
| T053 | two caps to the SAME endpoint deliver different badges (slots 1 vs 28); badged TRANSFER-only cap still fails EP_CALL with ACCESS_DENIED, no fallback |
| Host tests | slot/handle badge storage, swap/delete isolation, badged resolver, unbadged = 0, no badge leak on ACCESS_DENIED (test_ipc_cspace.c) |

## What this unlocks

- **Service death/relookup**: a server can track clients by badge and the
  supervisor can revoke/re-mint per-identity caps.
- **svcmgr legacy loop retirement**: REGISTER/UNREGISTER over EP can now
  be authenticated by badge.
- **Stateful protocols**: per-client server state keyed by badge becomes
  safe (the Fase 7 "stateless because no identity" constraint is lifted).
- **Revocation**: per-badge cap accounting.

## Fase 10 follow-up

Fase 10 consumes these badges as **policy** (see
[service-lifecycle.md](service-lifecycle.md)): `iris_badge_is_supervisor()`
gates `.ep` lookup DUPLICATE grants and the privileged RESTART op; EP REGISTER
binds an `owner_badge`; UNREGISTER checks it; and the STATUS/generation oracle
plus real `SYS_PROCESS_KILL`-driven respawn give death detection and relookup.
