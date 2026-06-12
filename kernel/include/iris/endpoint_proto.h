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

/* ── Bootstrap kind for svcmgr endpoint ────────────────────────────── */

/*
 * SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP — bootstrap handle kind carrying the
 * svcmgr endpoint cap.  Services that receive this in the bootstrap phase
 * can use IRIS_SVCMGR_EP_LOOKUP_NAME for EP-based service discovery.
 * Coexists with the legacy SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP / VFS_CAP etc.
 */
/* RETIRED in Fase 8: the discovery endpoint now arrives as the pre-start
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
/* RETIRED in Fase 8: the service's own endpoint recv side now arrives as
 * the pre-start CSpace mint IRIS_CPTR_OWN_EP.  Reserved; do not reuse. */
#define SVCMGR_BOOTSTRAP_KIND_SERVICE_EP UINT32_C(0x21)

/*
 * SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP — bootstrap handle kind carrying the
 * SEND side (RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER) of the console
 * KEndpoint (Fase 7.3).  The console service is spawned by init (not
 * svcmgr), so init creates the endpoint, hands the recv side to console
 * (kind 0x21) and delivers the send side to svcmgr with this kind; svcmgr
 * publishes it as "console.ep".  Bootstrap-delivered like every ".ep"
 * master, so the anti-spoof rule (no runtime registration of ".ep" names)
 * holds for the console too.
 */
/* RETIRED in Fase 8: init now mints the console endpoint send side into
 * svcmgr's root CNode at IRIS_CPTR_CONSOLE_EP.  Reserved; do not reuse. */
#define SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP UINT32_C(0x22)

/*
 * Well-known CSpace slots (Fase 8: CPtr-first bootstrap handoff).
 *
 * The spawner mints capabilities into the child's root CNode via
 * SYS_PROC_CSPACE_MINT; the child invokes them directly by CPtr — e.g.
 * SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg) — with no KChannel handle
 * transfer. CPtrs and handle_ids share one argument namespace: handles are
 * always >= 1024 (slot | generation<<10, generation >= 1).  Since Fase 8
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
 *
 * Cap kinds that CANNOT live in slots (handle/bootstrap boundary): KChannel,
 * KIoPort, KIrqCap, KBootstrapCap, KProcess — the dual resolver
 * (cspace_or_handle_resolve_*) only covers IPC objects (endpoint, reply,
 * notification), CNode, Untyped and Frame.  Services needing those caps
 * keep a bootstrap one-shot channel.
 */
#define IRIS_CPTR_SVCMGR_EP   ((uint64_t)1)
#define IRIS_CPTR_VFS_EP      ((uint64_t)2)
#define IRIS_CPTR_CONSOLE_EP  ((uint64_t)3)
#define IRIS_CPTR_KBD_EP      ((uint64_t)4)
#define IRIS_CPTR_OWN_EP      ((uint64_t)5)
#define IRIS_CPTR_IRQ_NOTIFY  ((uint64_t)7)
#define IRIS_CPTR_TEST_FIX_A  ((uint64_t)30)
#define IRIS_CPTR_TEST_FIX_B  ((uint64_t)31)

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
