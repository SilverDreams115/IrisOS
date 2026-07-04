#ifndef IRIS_KBD_EP_PROTO_H
#define IRIS_KBD_EP_PROTO_H

/*
 * kbd_ep_proto.h — stateless KEndpoint protocol for the keyboard service
 * (Fase 7.4).
 *
 * Replaces the legacy KBD_MSG_SUBSCRIBE push channel (sh ← kbd) with a
 * seL4-style pull: the client EP_CALLs the kbd endpoint and the service
 * replies through the per-call KReply capability.
 *
 * Wire format: struct IrisMsg (iris/ipc_msg.h), msg.label = opcode.
 *   - Requests carry no bulk payload; buf_len > 0 → IRIS_ERR_INVALID_ARG.
 *   - Reply OK : label = IRIS_EP_REPLY_OK,  words[0] = 0,
 *                words[1] = raw scancode (POLL/READ), word_count = 2.
 *   - Reply ERR: label = IRIS_EP_REPLY_ERR, words[0] = (uint32_t)iris_error_t,
 *                word_count = 1.
 *   - Unknown opcode → IRIS_ERR_NOT_SUPPORTED.
 *
 * Event buffering: kbd keeps a KBD_EP_RING_CAP-deep scancode ring fed by the
 * IRQ path. On overflow the OLDEST event is dropped (newest always kept).
 *
 * KBD_EP_OP_POLL — non-blocking fetch.
 *   Reply OK with the oldest buffered scancode, or ERR IRIS_ERR_WOULD_BLOCK
 *   when the ring is empty. Never parks the reply.
 *
 * KBD_EP_OP_READ — blocking pull (deferred reply).
 *   If the ring has an event, replies immediately. Otherwise kbd PARKS the
 *   per-call KReply capability and answers it from the next IRQ scancode:
 *   the caller stays blocked in SYS_EP_CALL until a key arrives. Exactly one
 *   reply may be parked; a second READ while one is parked gets ERR
 *   IRIS_ERR_WOULD_BLOCK (single interactive consumer by design — sh).
 *   If the parked caller dies, kbd's deferred SYS_REPLY fails and the
 *   capability is closed; no event is delivered twice.
 *
 * IRIS_EP_OP_PING (0xFF01, endpoint_proto.h) is also served: reply OK.
 *
 * Liveness: kbd never blocks on event delivery. IRQ scancodes (KNotification)
 * and endpoint requests are multiplexed in one loop (EP drain + notification
 * poll).  The legacy KChannel HELLO/STATUS path (iris/kbd_proto.h) is retired
 * and no longer part of this loop (Fase 13/Track G).
 *
 * Discovery: svcmgr publishes the endpoint as "kbd.ep"
 * (IRIS_SVCMGR_EP_LOOKUP_NAME); the recv side reaches kbd at bootstrap via
 * SVCMGR_BOOTSTRAP_KIND_SERVICE_EP (endpoint_proto.h).
 *
 * NOTE: constants are plain hex (no casts/enums) — this header is included
 * from assembly (services/kbd/main.S).
 */

#define KBD_EP_OP_POLL 0x0201
#define KBD_EP_OP_READ 0x0202

/* Scancode ring depth (power of two; index mask = KBD_EP_RING_CAP - 1). */
#define KBD_EP_RING_CAP 16

/* Service name under which svcmgr publishes the kbd endpoint. */
#define KBD_EP_SVC_NAME "kbd.ep"

/* uint32_t images of iris_error_t values used on the wire / in assembly
 * (kept in sync with iris/nc/error.h; enums are unusable from .S files). */
#define KBD_EP_E_INVALID_ARG   0xFFFFFFFF /* (uint32_t)IRIS_ERR_INVALID_ARG  (-1)  */
#define KBD_EP_E_NOT_SUPPORTED 0xFFFFFFF5 /* (uint32_t)IRIS_ERR_NOT_SUPPORTED(-11) */
#define KBD_EP_E_WOULD_BLOCK   0xFFFFFFF3 /* (uint32_t)IRIS_ERR_WOULD_BLOCK  (-13) */

/* Assembly images of endpoint_proto.h constants (UINT64_C/UINT32_C macros do
 * not expand under the assembler preprocessor). Sync-checked below in C. */
#define KBD_EP_PING_OP            0xFF01 /* = IRIS_EP_OP_PING */
#define KBD_EP_REPLY_OK_LABEL     0      /* = IRIS_EP_REPLY_OK */
#define KBD_EP_REPLY_ERR_LABEL    1      /* = IRIS_EP_REPLY_ERR */
#define KBD_EP_KIND_SERVICE_EP    0x21   /* = SVCMGR_BOOTSTRAP_KIND_SERVICE_EP */

#ifndef __ASSEMBLER__
#include <iris/endpoint_proto.h>
#include <iris/nc/error.h>
_Static_assert(KBD_EP_PING_OP == IRIS_EP_OP_PING, "ping op out of sync");
_Static_assert(KBD_EP_REPLY_OK_LABEL == IRIS_EP_REPLY_OK, "reply ok out of sync");
_Static_assert(KBD_EP_REPLY_ERR_LABEL == IRIS_EP_REPLY_ERR, "reply err out of sync");
_Static_assert(KBD_EP_KIND_SERVICE_EP == SVCMGR_BOOTSTRAP_KIND_SERVICE_EP,
               "bootstrap kind out of sync");
_Static_assert(KBD_EP_E_INVALID_ARG == (uint32_t)IRIS_ERR_INVALID_ARG,
               "invalid_arg image out of sync");
_Static_assert(KBD_EP_E_NOT_SUPPORTED == (uint32_t)IRIS_ERR_NOT_SUPPORTED,
               "not_supported image out of sync");
_Static_assert(KBD_EP_E_WOULD_BLOCK == (uint32_t)IRIS_ERR_WOULD_BLOCK,
               "would_block image out of sync");
#endif /* !__ASSEMBLER__ */

#endif /* IRIS_KBD_EP_PROTO_H */
