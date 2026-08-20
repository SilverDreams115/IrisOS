/*
 * endpoint_proto.h — IrisMsg-based service IPC protocol definitions.
 *
 * This header defines the wire protocol for services that communicate via
 * KEndpoint (seL4-style synchronous IPC) rather than KChannel ring buffers.
 *
 * Protocol model:
 *   - Clients call SYS_EP_CALL(service_ep, msg) to send a request and block.
 *   - Servers loop on SYS_EP_RECV, process requests, then SYS_REPLY(reply_h, reply).
 *   - msg.label identifies the operation.
 *   - msg.words[0..3] carry fixed-size arguments (up to 4 × uint64_t).
 *   - Variable-length data goes in the bulk kbuf (msg.buf_uptr / msg.buf_len).
 *
 * Reply format:
 *   - reply.label == IRIS_EP_REPLY_OK for success.
 *   - reply.label == IRIS_EP_REPLY_ERR for failure; reply.words[0] = iris_error_t.
 *   - reply.words[1..3] and kbuf carry operation-specific return values.
 *   - reply.attached_handle carries a transferred capability when applicable.
 *
 * Opcodes 0x0000–0x00FF are reserved for the standard protocol.
 * Opcodes 0x0100–0xEFFF are for individual services.
 * Opcodes 0xF000–0xFEFF are for svcmgr endpoint protocol.
 * Opcodes 0xFF00–0xFFFF are generic service management (ping, shutdown).
 */

#ifndef IRIS_ENDPOINT_PROTO_H
#define IRIS_ENDPOINT_PROTO_H

#include <stdint.h>

/* ── Standard reply labels ──────────────────────────────────────────── */

#define IRIS_EP_REPLY_OK   UINT64_C(0x0000000000000000)   /* success */
#define IRIS_EP_REPLY_ERR  UINT64_C(0x0000000000000001)   /* failure; words[0] = error code */

/* ── Generic service opcodes ────────────────────────────────────────── */

#define IRIS_EP_OP_PING     UINT64_C(0xFF01)  /* health check; server replies REPLY_OK */
#define IRIS_EP_OP_SHUTDOWN UINT64_C(0xFF02)  /* request graceful shutdown */

/* ── Svcmgr endpoint protocol ───────────────────────────────────────── */

/*
 * IRIS_SVCMGR_EP_LOOKUP_NAME — resolve a service by name.
 *   Request:  kbuf = NUL-terminated service name; buf_len includes the NUL.
 *   Reply OK: attached_handle = endpoint cap for the service (caller-owned).
 *             words[0] = service_id (uint32_t) for future reference.
 *   Reply ERR: words[0] = IRIS_ERR_NOT_FOUND or other error code.
 */
#define IRIS_SVCMGR_EP_LOOKUP_NAME  UINT64_C(0xF001)

/*
 * IRIS_SVCMGR_EP_REGISTER — register a service endpoint.
 *   Request:  attached_handle = endpoint cap to register (transferred to svcmgr).
 *             kbuf = NUL-terminated service name; buf_len includes the NUL.
 *   Reply OK: words[0] = assigned service_id (uint32_t).
 *   Reply ERR: words[0] = error code (e.g. IRIS_ERR_BUSY if name taken).
 */
#define IRIS_SVCMGR_EP_REGISTER     UINT64_C(0xF002)

/*
 * IRIS_SVCMGR_EP_UNREGISTER — unregister a previously registered service.
 *   Request:  words[0] = service_id (uint32_t) from REGISTER reply.
 *   Reply OK: (no payload)
 *   Reply ERR: words[0] = error code.
 */
#define IRIS_SVCMGR_EP_UNREGISTER   UINT64_C(0xF003)

/*
 * IRIS_SVCMGR_EP_LOOKUP_ID — resolve a service by numeric ID.
 *   Request:  words[0] = service_id (uint32_t).
 *   Reply OK: attached_handle = endpoint cap for the service.
 *   Reply ERR: words[0] = IRIS_ERR_NOT_FOUND or other error code.
 */
#define IRIS_SVCMGR_EP_LOOKUP_ID    UINT64_C(0xF004)

/*
 * Phase 10 lifecycle opcodes.
 *
 * IRIS_SVCMGR_EP_STATUS — query the liveness/generation of a service.
 *   Request:  words[0] = service_id  (catalog id, or a dynamic id >= 0x40).
 *   Reply OK: words[0] = alive (1/0), words[1] = generation (bumps on every
 *             restart/revoke so a client can detect a stale cap), word_count=2.
 *   Reply ERR: words[0] = IRIS_ERR_NOT_FOUND.
 *   Available to any caller — STATUS is a read-only liveness oracle and is
 *   what lets a client poll a restart without blocking on a dead endpoint.
 *
 * IRIS_SVCMGR_EP_RESTART — force a service to be killed and respawned.
 *   Request:  words[0] = service_id.
 *   Reply OK: words[0] = new generation.
 *   Reply ERR: words[0] = IRIS_ERR_ACCESS_DENIED (caller badge not a
 *             supervisor) or IRIS_ERR_NOT_FOUND / IRIS_ERR_INVALID_ARG.
 *   PRIVILEGED: only iris_badge_is_supervisor() caps may invoke it.
 */
#define IRIS_SVCMGR_EP_STATUS       UINT64_C(0xF005)
#define IRIS_SVCMGR_EP_RESTART      UINT64_C(0xF006)

/*
 * IRIS_SVCMGR_EP_DIAG — endpoint-native svcmgr snapshot (Phase 12).  Replaces
 * the legacy KChannel SVCMGR_MSG_DIAG as the productive diagnostics path; no
 * KChannel round-trip.  Open to any caller (read-only).
 *   Reply OK: words[0] = catalog service count, words[1] = ready services,
 *             words[2] = active dynamic registrations,
 *             words[3] = catalog version.  word_count = 4.
 */
#define IRIS_SVCMGR_EP_DIAG         UINT64_C(0xF007)

/* ── Bootstrap kind for svcmgr endpoint ────────────────────────────── */

/*
 * SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP — bootstrap handle kind carrying the
 * svcmgr endpoint cap.  Services that receive this in the bootstrap phase
 * can use IRIS_SVCMGR_EP_LOOKUP_NAME for EP-based service discovery.
 * Coexists with the legacy SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP / VFS_CAP etc.
 */
/* RETIRED in Phase 8: the discovery endpoint now arrives as the pre-start
 * CSpace mint IRIS_CPTR_SVCMGR_EP.  Kind value reserved; do not reuse. */
#define SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP  UINT32_C(0x20)

/*
 * SVCMGR_BOOTSTRAP_KIND_SERVICE_EP — bootstrap handle kind carrying the
 * receive side (RIGHT_READ) of the service's OWN KEndpoint.  svcmgr creates
 * one KEndpoint per catalog service with own_service_ep=1 and keeps the
 * master across restarts, so client caps stay valid when the service is
 * respawned.  Clients obtain the send side via service-name lookup of
 * "<image_name>.ep" (see below).
 */
/* RETIRED in Phase 8: the service's own endpoint recv side now arrives as
 * the pre-start CSpace mint IRIS_CPTR_OWN_EP.  Reserved; do not reuse. */
#define SVCMGR_BOOTSTRAP_KIND_SERVICE_EP UINT32_C(0x21)

/*
 * SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP — bootstrap handle kind carrying the
 * SEND side (RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER) of the console
 * KEndpoint (Phase 7.3).  The console service is spawned by init (not
 * svcmgr), so init creates the endpoint, hands the recv side to console
 * (kind 0x21) and delivers the send side to svcmgr with this kind; svcmgr
 * publishes it as "console.ep".  Bootstrap-delivered like every ".ep"
 * master, so the anti-spoof rule (no runtime registration of ".ep" names)
 * holds for the console too.
 */
/* RETIRED in Phase 8: init now mints the console endpoint send side into
 * svcmgr's root CNode at IRIS_CPTR_CONSOLE_EP.  Reserved; do not reuse. */
#define SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP UINT32_C(0x22)

/*
 * Well-known CSpace slots (Phase 8: CPtr-first bootstrap handoff).
 *
 * The spawner mints capabilities into the child's root CNode via
 * SYS_PROC_CSPACE_MINT; the child invokes them directly by CPtr — e.g.
 * SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg) — with no KChannel handle
 * transfer. CPtrs and handle_ids share one argument namespace: handles are
 * always >= 1024 (slot | generation<<10, generation >= 1).  Since Phase 8
 * the dual resolvers ENFORCE the split (kernel/new_core/src/cspace.c):
 * values < 1024 resolve through the CSpace ONLY (no handle-table fallback;
 * missing slot fails cleanly, ACCESS_DENIED is a hard stop) and values
 * >= 1024 resolve through the handle table ONLY (they never walk the
 * CSpace, so populated low slots cannot be aliased by handle bit patterns).
 * Slot 0 is the null slot.
 *
 * Layout (root CNode has KCNODE_DEFAULT_SLOTS = 256 slots):
 *   0          CPTR_NULL (always invalid)
 *   1..4       core service endpoints, client side (RIGHT_WRITE):
 *              svcmgr discovery, vfs, console, kbd.  svcmgr mints 1..4
 *              into every catalog child; init mints them into the
 *              processes it spawns itself (svcmgr gets slot 3 with
 *              DUPLICATE|TRANSFER so it can keep publishing "console.ep").
 *   5          the service's OWN endpoint, receive side (RIGHT_READ) —
 *              only for services that serve one (own_service_ep / console).
 *   6          reserved (future: initrd/bootstrap cap, once KBootstrapCap
 *              joins the dual resolver).
 *   7          IRQ KNotification WAIT side (irq_notify services: kbd).
 *   8..15      reserved for future core services.
 *   16..29     reserved (dynamic/per-service use, unassigned).
 *   30..31     test fixtures (iris_test only; minted by init): wrong-type
 *              cap and insufficient-rights cap for CPtr failure tests.
 *   32..47     runtime-test dynamic mints and A1.5 receive-slots (iris_test
 *              T083+ self-mints via IRIS_CPTR_TEST_PROC / T084+ IPC
 *              receive-slot deliveries; the 16..29 pool is exhausted).
 *
 * Phase 13: KIoPort, KIrqCap and KBootstrapCap NOW resolve through CSpace via
 * the generic dual resolver cspace_resolve_only_obj() — the device-access
 * syscalls (SYS_IOPORT_IN/OUT, SYS_IRQ_ROUTE_REGISTER, SYS_IRQ_ACK,
 * SYS_INITRD_*, SYS_PROCESS_CREATE, SYS_CAP_CREATE_*, SYS_BOOTCAP_RESTRICT,
 * SYS_FRAMEBUFFER_VMO) accept a CPtr slot or a handle.  This removes the last
 * reason device caps had to travel over a KChannel at bootstrap (the
 * prerequisite for full KChannel retirement).  Still outside the resolver:
 * KChannel and KProcess.
 */
/*
 * Well-known sender badges (Phase 9).
 *
 * A badge is per-cap metadata stamped by the KERNEL into
 * IrisMsg.sender_badge on every EP_SEND / EP_NB_SEND / EP_CALL — it is
 * taken from the capability the sender invoked, never from the payload,
 * so it cannot be forged by writing the field.  Badges are assigned at
 * mint time by the spawner (SYS_PROC_CSPACE_MINT arg3 high bits); a badged
 * cap can never be re-badged.  0 = unbadged (legacy / master caps; servers
 * treat it as "unidentified legacy client").
 *
 * Assignment scheme:
 *   0x100 + service_id  → core catalog services (kbd 0x101, vfs 0x102,
 *                         sh 0x103) — stamped on the slots svcmgr mints.
 *   IRIS_BADGE_SVCMGR   → svcmgr itself (its console.ep cap from init...
 *                         kept unbadged for redistribution; reserved).
 *   IRIS_BADGE_IRIS_TEST→ iris_test (slots minted by init).
 *   IRIS_BADGE_TEST_B   → iris_test secondary fixture (two caps to the
 *                         same endpoint must deliver different badges).
 *
 * PING convention (Phase 9): every core EP server replies to
 * IRIS_EP_OP_PING with words[1] = the sender_badge it observed, making
 * identity testable end to end (T047+).
 */
#define IRIS_BADGE_NONE       ((uint64_t)0)
#define IRIS_BADGE_SVC(id)    ((uint64_t)0x100 + (uint64_t)(id))
/* Phase 10: named reserved badges. The 0x100+service_id scheme (Phase 9) is
 * kept; these aliases make the service-identity policy in svcmgr explicit.
 *   kbd  = SVCMGR_SERVICE_KBD(1), vfs = VFS(2), sh = SH(3)  → 0x101..0x103.
 * console is spawned by init (not the catalog) so it gets its own value;
 * init is the root spawner and holds the unbadged masters but is also given
 * an explicit identity for the supervisor-authority policy below. */
#define IRIS_BADGE_KBD        IRIS_BADGE_SVC(1)   /* 0x101 */
#define IRIS_BADGE_VFS        IRIS_BADGE_SVC(2)   /* 0x102 */
#define IRIS_BADGE_SH         IRIS_BADGE_SVC(3)   /* 0x103 */
#define IRIS_BADGE_CONSOLE    ((uint64_t)0x104)
#define IRIS_BADGE_INIT       ((uint64_t)0x111)
#define IRIS_BADGE_SVCMGR     ((uint64_t)0x110)
#define IRIS_BADGE_IRIS_TEST  ((uint64_t)0x1F0)
#define IRIS_BADGE_TEST_B     ((uint64_t)0xB2)
/* Dynamic services registered at runtime are assigned badges from this base
 * upward (svcmgr owner-badge accounting); never overlaps the reserved range. */
#define IRIS_BADGE_DYNAMIC_BASE ((uint64_t)0x200)

/*
 * Phase 28.1: file-grant badge identities at the VFS.  These are BADGES, not
 * rights: the kernel stamps them into IrisMsg.sender_badge from the invoked
 * cap, so the VFS can classify a caller unforgeably.
 *   IRIS_BADGE_FILEGRANT_ADMIN — a pager supervisor's grant-admin identity
 *       (GRANT_OPEN / GRANT_REVOKE-by-name / GRANT_SESSION_RESET).
 *   IRIS_BADGE_FILEGRANT_S(s)  — grant session s: a pager instance's ONLY
 *       VFS identity.  Session badges are confined to the session-scoped
 *       grant ops and are denied every name-based op.
 * Session caps can only be minted from an UNBADGED duplicable vfs.ep cap
 * (fresh badges require an unbadged source), i.e. by a supervisor under the
 * Phase 10 grant-tightening rule.  Neither range overlaps service (0x100+),
 * dynamic (0x200+) or test badges. */
#define IRIS_BADGE_FILEGRANT_ADMIN  ((uint64_t)0x0F00)
#define IRIS_BADGE_FILEGRANT_BASE   ((uint64_t)0x0F10)
#define IRIS_FILEGRANT_SESSIONS     8u
#define IRIS_BADGE_FILEGRANT_S(s)   (IRIS_BADGE_FILEGRANT_BASE + (uint64_t)(s))
/* badge → session index, or -1 if not a session badge. */
static inline int iris_badge_filegrant_session(uint64_t badge) {
    if (badge < IRIS_BADGE_FILEGRANT_BASE ||
        badge >= IRIS_BADGE_FILEGRANT_BASE + (uint64_t)IRIS_FILEGRANT_SESSIONS)
        return -1;
    return (int)(badge - IRIS_BADGE_FILEGRANT_BASE);
}

/*
 * Supervisor authority (Phase 10).  Only these badges may receive a cap with
 * RIGHT_DUPLICATE/RIGHT_TRANSFER from a `.ep` lookup (re-minting authority)
 * or drive privileged lifecycle ops (restart/revoke).  Everyone else is an
 * ordinary client and gets WRITE-only caps.  Badge 0 = the unbadged bootstrap
 * caps held by init before it acquires its identity badge, and svcmgr's own
 * master — both legitimately re-mint service caps into children.
 */
static inline int iris_badge_is_supervisor(uint64_t badge) {
    return badge == IRIS_BADGE_NONE  ||
           badge == IRIS_BADGE_INIT  ||
           badge == IRIS_BADGE_SVCMGR;
}

#define IRIS_CPTR_SVCMGR_EP   ((uint64_t)1)
#define IRIS_CPTR_VFS_EP      ((uint64_t)2)
#define IRIS_CPTR_CONSOLE_EP  ((uint64_t)3)
#define IRIS_CPTR_KBD_EP      ((uint64_t)4)
#define IRIS_CPTR_OWN_EP      ((uint64_t)5)
/* Phase 13 (Track C): the initrd capability is minted
 * into this slot before the child starts — replaces the post-spawn KChannel
 * SVCMGR_BOOTSTRAP_KIND_INITRD_CAP delivery.  Resolves via the device-cap dual
 * resolver (cspace_resolve_only_obj), so SYS_INITRD_* accept it by CPtr. */
/* Stage 5 Step 2: slot 6 held the MONOLITHIC boot capability — spawn,
 * hardware, debug and framebuffer authority at once.  It holds the PROCESS
 * CONTROL capability now, which authorises SYS_PROCESS_CREATE and nothing
 * else; the other authorities travel in their own slots.  The name
 * IRIS_CPTR_SPAWN_CAP is retired with the object it named. */
#define IRIS_CPTR_PROC_CONTROL ((uint64_t)6)
#define IRIS_CPTR_IRQ_NOTIFY  ((uint64_t)7)
/* Phase 13 (Track C): the legacy handle-boundary caps for a non-endpoint_only
 * service (kbd) — its service/reply KChannels and KIoPort/KIrqCap device caps —
 * now arrive as pre-start CSpace mints instead of post-spawn KChannel
 * SVCMGR_BOOTSTRAP_KIND_{SERVICE,REPLY,IOPORT_CAP,IRQ_CAP} messages.  The
 * KChannel slots are resolved to handles (CHAN_RECV/SEND need handle-table
 * handles); the device caps resolve by CPtr (cspace_resolve_only_obj). */
/* Slots 8 and 9 were IRIS_CPTR_SVC_CHAN / IRIS_CPTR_SVC_REPLY, the legacy
 * service/reply KChannel pair.  KChannel is REMOVED and every catalog service
 * is endpoint-only, so both constants had no live use — Stage 5 Step 2 reuses
 * them for split-out authorities rather than growing root CNodes that are
 * already nearly full. */
#define IRIS_CPTR_INITRD_CONTROL ((uint64_t)8)
#define IRIS_CPTR_DEBUG_CONTROL  ((uint64_t)9)
/* The framebuffer control capability: SYS_FRAMEBUFFER_VMO, one-shot.  Slot 99
 * is outside every service's well-known range and free in the three processes
 * that ever hold it (init, fb, the suite); the suite's root CNode kept it
 * empty for a NOT_FOUND probe that moved to a scratch slot. */
#define IRIS_CPTR_FB_CONTROL     ((uint64_t)99)
#define IRIS_CPTR_IOPORT      ((uint64_t)10)
#define IRIS_CPTR_IRQ_CAP     ((uint64_t)11)
/*
 * Stage 5 Step 2: boot CONTROL capabilities — the authority to CREATE device
 * capabilities, as opposed to IRIS_CPTR_IOPORT / IRIS_CPTR_IRQ_CAP above,
 * which are the device capabilities themselves.
 *
 * One capability per authority: holding the ioport control capability says
 * nothing about interrupt lines, and neither says anything about spawning or
 * debugging.  Their predecessor was a single IRIS_BOOTCAP_HW_ACCESS bit on the
 * monolithic boot capability at IRIS_CPTR_SPAWN_CAP, so every service that
 * needed a serial port also held the authority to claim any IRQ, spawn
 * processes and power the machine off.
 *
 * They are minted only into the processes that create device caps — init,
 * svcmgr and the suite.  Slot 15 is the last free slot of the well-known
 * bootstrap range.  Slot 26 was IRIS_CPTR_TEST_SPAWN, a second copy of the
 * monolith that existed so the suite could prove an authority capability
 * resolves by CPtr (T069); the ioport control capability proves the same thing
 * and is what that test was named for, so the slot changes meaning rather than
 * the suite growing one it does not have — its root CNode is full.  svcmgr's
 * own device-cap slots moved to 96..127 to keep both slots free there.
 */
#define IRIS_CPTR_IRQ_CONTROL    ((uint64_t)15)
#define IRIS_CPTR_IOPORT_CONTROL ((uint64_t)26)
#define IRIS_CPTR_TEST_FIX_A  ((uint64_t)30)
#define IRIS_CPTR_TEST_FIX_B  ((uint64_t)31)
/* Phase 9: second badged cap to the svcmgr endpoint (badge IRIS_BADGE_TEST_B)
 * — proves two caps to ONE endpoint deliver different badges (T053). */
#define IRIS_CPTR_TEST_FIX_C  ((uint64_t)28)
/* Phase 10: a THIRD cap to the svcmgr endpoint carrying a SUPERVISOR badge
 * (IRIS_BADGE_INIT), minted by init into iris_test so the lifecycle tests can
 * drive the privileged IRIS_SVCMGR_EP_RESTART path.  Slot 27 (slot 29 stays
 * the reserved-but-unminted probe used by T041). */
#define IRIS_CPTR_TEST_SUPER  ((uint64_t)27)
/* Phase 13: a device/authority cap (the spawn KBootstrapCap) minted into a
 * CPtr slot, proving device caps resolve via CSpace (cspace_resolve_only_obj)
 * and are invocable by CPtr — the prerequisite for KChannel-free bootstrap. */
/* Slot 26 is IRIS_CPTR_IOPORT_CONTROL since Stage 5 Step 2 (see above).
 * IRIS_CPTR_TEST_SPAWN — a second cap to the monolithic boot capability —
 * is retired: what it existed to prove is now proven with a capability that
 * authorises exactly one thing. */
/* A1 Increment 1: iris_test's OWN process cap (RIGHT_WRITE|RIGHT_DUPLICATE),
 * minted by init post-load, so the suite can SYS_PROC_CSPACE_MINT runtime-made
 * caps into its own CSpace slots (T079 mints a VMO and maps it by CPtr). */
#define IRIS_CPTR_TEST_PROC   ((uint64_t)25)
/* Phase 18: one boot KUntyped forwarded down the boot chain (userboot → init →
 * iris_test) so the ring-3 authority suite (T125–T131) can exercise
 * SYS_UNTYPED_RETYPE / SYS_CAP_REVOKE end to end.  IRIS_CPTR_INIT_UNTYPED is
 * init's receiving slot; IRIS_CPTR_TEST_UNTYPED is iris_test's.  Full rights
 * (READ|WRITE|DUPLICATE|TRANSFER) at each hop so the mint (needs DUPLICATE) and
 * retype (needs WRITE) both succeed. */
#define IRIS_CPTR_INIT_UNTYPED ((uint64_t)12)
/* Stage 6: a SECOND boot block for init.
 *
 * Once memory is charged rather than assumed — address spaces, process state,
 * image copies, VMO pages — one boot block is the ceiling on everything init's
 * subtree can spend, and the drain produces a dozen of them.  init carves the
 * suite's budget from this one and svcmgr's from the first, so neither starves
 * the other.  Slot 24 is free in init and is minted to nobody else. */
#define IRIS_CPTR_INIT_UNTYPED2 ((uint64_t)24)
#define IRIS_CPTR_TEST_UNTYPED ((uint64_t)55)
/* Phase S1: slot 12 is the GENERIC "delegated untyped pool" slot — the parent
 * (userboot → init → svcmgr) delegates a bounded sub-untyped here so the child
 * can SYS_UNTYPED_RETYPE2 its own kernel objects.  IRIS_CPTR_INIT_UNTYPED is
 * the historical name for init's instance of the same slot. */
#define IRIS_CPTR_OWN_UNTYPED  ((uint64_t)12)
/*
 * Stage 6-pure Step 2 gave this slot a second, guaranteed occupant.
 *
 * The kernel no longer creates paging levels, so a task that maps anything
 * must be able to retype one — and a level for its own address space belongs
 * in the region that address space was already charged to.  svc_loader
 * therefore mints the child's OWN address-space budget here whenever the
 * spawner's manifest has not already claimed the slot for a pool of its own.
 * Either way the meaning is unchanged and services need no new constant: slot
 * 12 is "the untyped this task may allocate from".
 *
 * It is deliberately NOT a new well-known slot.  Two attempts at one collided
 * with things a grep does not show — init's vfs.ep receive slot, the suite's
 * "always empty between tests" scratch pool, a pager target slot, and two
 * slots whose whole purpose is to never resolve.  A 256-slot namespace shared
 * by nine services has no free number that stays free; it has slots with
 * meanings, and this is the one whose meaning already fits.
 */
/* Phase S1: explicit MCS-style reply objects.  The kernel no longer fabricates
 * a KReply at EP_CALL rendezvous: a server passes its reply-object CPtr as
 * arg2 of SYS_EP_RECV / SYS_EP_NB_RECV and later invokes SYS_REPLY on the
 * value echoed in msg.attached_handle.  The supervisor that boots a serving
 * child retypes the reply object(s) from its untyped pool and mints them
 * here.  OWN_REPLY2 exists for servers that PARK one reply while continuing
 * to serve (kbd): they alternate between the two slots. */
#define IRIS_CPTR_OWN_REPLY    ((uint64_t)13)
#define IRIS_CPTR_OWN_REPLY2   ((uint64_t)14)
/* Phase 28: a DUPLICABLE vfs.ep cap (RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER,
 * badge IRIS_BADGE_IRIS_TEST) minted by init — supervisor authority — into this
 * slot.  The ordinary svcmgr lookup path strips DUPLICATE/TRANSFER from
 * non-supervisor clients (grant tightening), so iris_test cannot re-mint a
 * looked-up vfs cap into a file-backed pager it supervises.  This mirrors
 * IRIS_CPTR_TEST_SUPER (a supervisor-badged svcmgr cap for the privileged
 * RESTART path): init explicitly hands the test harness the file-read authority
 * a real pager supervisor would hold.  iris_test materializes it to a handle via
 * SYS_CSPACE_RESOLVE (rights preserved) before using it as a mint source.
 * Slot 58: clear of iris_test's scratch CPtr slots (23/24, 57, 60–63).
 *
 * Phase 28.1: slot 58 now carries the file-grant ADMIN identity
 * (IRIS_BADGE_FILEGRANT_ADMIN, call-only WRITE): it drives GRANT_OPEN /
 * GRANT_REVOKE / GRANT_SESSION_RESET at the VFS and nothing else.  The mint
 * SOURCE for session-badged pager caps moved to its own slot 59
 * (IRIS_CPTR_TEST_VFS_MINT): an UNBADGED WRITE|DUPLICATE|TRANSFER vfs.ep cap.
 * The split exists because a badged cap can never be re-badged — one slot
 * cannot be both a positive admin identity and a fresh-badge mint source.
 * Invoked directly (badge 0), slot 59 is an ordinary named-export client at
 * the VFS — no grant-admin authority rides on unbadged caps. */
#define IRIS_CPTR_TEST_VFS_DUP  ((uint64_t)58)
#define IRIS_CPTR_TEST_VFS_MINT ((uint64_t)59)
/* Phase 19: iris_test mints a cap to its OWN VSpace (SYS_VSPACE_SELF) into this
 * slot so the VM suite (T132–T139) can drive SYS_FRAME_MAP / SYS_FRAME_UNMAP on
 * itself by CPtr.  Self-authority only — not a general authority door. */
#define IRIS_CPTR_TEST_VSPACE  ((uint64_t)56)

/*
 * Reserved name suffix ".ep": IRIS_SVCMGR_EP_LOOKUP_NAME and the legacy
 * SVCMGR_MSG_LOOKUP_NAME resolve "<image_name>.ep" to the service's
 * KEndpoint (RIGHT_WRITE) and "svcmgr.ep" to svcmgr's own endpoint.
 * Dynamic registration (SVCMGR_MSG_REGISTER) of names ending in ".ep"
 * is rejected with IRIS_ERR_INVALID_ARG to prevent endpoint spoofing.
 */
#define IRIS_EP_NAME_SUFFIX  ".ep"

/* Maximum service name length (including NUL) for endpoint protocol */
#define IRIS_EP_SVCNAME_MAX  128U

#endif /* IRIS_ENDPOINT_PROTO_H */
