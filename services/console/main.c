/*
 * console/main.c — ring-3 serial console service.
 *
 * Bootstrap deliveries from svc_loader:
 *   recv SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP → ioport_h (KIoPort for 0x3F8..0x3FF)
 *   recv SVCMGR_BOOTSTRAP_KIND_SERVICE_EP → ep_h (KEndpoint recv, Phase 7.3)
 *
 * Main loop: endpoint-only. Drain EP requests (CONSOLE_EP_OP_WRITE / SYNC /
 * PING — iris/console_ep_proto.h). EP WRITE replies only after the bytes hit
 * the UART; EP SYNC is an explicit flush barrier. The legacy KChannel write
 * path (CONSOLE_MSG_WRITE/SYNC) is retired, header deleted, and no longer
 * served — every writer, including svcmgr's klog drain, uses console.ep
 * (Phase 13/Track G).
 */

#include <stdint.h>
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/console_ep_proto.h>
#include <iris/ipc_msg.h>
#include <iris/nc/error.h>
#include "../common/iris_ipc_buffer.h"

/* Poll the UART Line Status Register (offset 5) until bit 5 (THRE) is set,
 * then write one byte to the Transmit Holding Register (offset 0). */
static void con_uart_write_byte(iris_cptr_t ioport_h, uint8_t byte) {
    long v;
    /* Wait for THRE (bit 5 of LSR at offset 5). */
    do {
        v = iris_invoke1((long)ioport_h, INV_IOPORT_IN, 5);
    } while (v < 0 || !((uint8_t)v & 0x20u));
    (void)iris_invoke2((long)ioport_h, INV_IOPORT_OUT, 0, (long)byte);
}

/* Phase 13 (Track I): the legacy KChannel write path (con_serve_chan_msg /
 * con_drain_chan, CONSOLE_MSG_WRITE/SYNC) is retired — console is endpoint-only.
 * Its sole writers (svcmgr klog drain, init logging) now use console.ep. */

/*
 * The request buffer, and the two forms it can take (ledger D-4).
 *
 * `g_con_ep_buf` is the fallback: 256 bytes of BSS matched to the kernel's own
 * staging, which is what a thread with no registered IPC buffer gets.  At
 * startup console retypes a page from the Untyped it owns and registers it,
 * and `g_con_buf` points there instead — the kernel then writes incoming
 * payloads straight into that page and no user pointer is named on either
 * side.
 *
 * The pointer indirection is the whole migration: everything below reads
 * `g_con_buf` and `g_con_buf_cap`, so the two paths differ in one assignment
 * at startup rather than in every use.
 */
static uint8_t  g_con_ep_buf[IRIS_IPC_BUF_SIZE];
static uint8_t *g_con_buf     = g_con_ep_buf;
static uint32_t g_con_buf_cap = IRIS_IPC_BUF_SIZE;

/* Spare slots from the per-service range (22..29 are unassigned). */
#define CON_SLOT_IPCBUF_FRAME  22u
#define CON_SLOT_IPCBUF_PT     23u

static void con_ipc_buffer_init(void) {
    void *b = iris_ipc_buffer_init(CON_SLOT_IPCBUF_FRAME, CON_SLOT_IPCBUF_PT,
                                   IRIS_IPC_BUFFER_VA);
    if (!b) return;                 /* keep the staging path; not fatal */
    g_con_buf     = (uint8_t *)b;
    g_con_buf_cap = 4096u;
}

static void con_imsg_zero(struct iris_msg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

static void con_ep_reply_err(struct iris_msg *reply, int32_t err) {
    con_imsg_zero(reply);
    reply->label      = IRIS_EP_REPLY_ERR;
    reply->words[0]   = (uint64_t)(uint32_t)err;
    reply->word_count = 1u;
}

/* Serve one endpoint request; exactly one reply per request. */
static void con_serve_ep_msg(iris_cptr_t ioport_h, struct iris_msg *req) {
    struct iris_msg reply;
    iris_cptr_t reply_h = (iris_cptr_t)req->got_cap;

    switch (req->label) {
    case CONSOLE_EP_OP_WRITE: {
        uint32_t len = req->buf_len;
        if (len > g_con_buf_cap) len = g_con_buf_cap;
        for (uint32_t i = 0; i < len; i++)
            con_uart_write_byte(ioport_h, g_con_buf[i]);
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        reply.word_count = 1u;
        break;
    }
    case CONSOLE_EP_OP_SYNC:
        if (req->buf_len > 0u) {
            con_ep_reply_err(&reply, IRIS_ERR_INVALID_ARG);
            break;
        }
        /* Phase 13 (Track I): no legacy KChannel writers remain — EP writes are
         * synchronous by construction, so SYNC is a trivial acknowledge. */
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        reply.word_count = 1u;
        break;
    case IRIS_EP_OP_PING:
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        /* Phase 9 PING convention: echo the kernel-stamped sender badge. */
        reply.words[1]   = req->sender_badge;
        reply.word_count = 2u;
        break;
    default:
        con_ep_reply_err(&reply, IRIS_ERR_NOT_SUPPORTED);
        break;
    }

    /* Phase S1: reply_h is the console's OWN reply-object CPtr (echoed by the
     * kernel from the recv arg2).  The object is reusable — nothing to close. */
    if (reply_h != IRIS_CPTR_NULL)
        (void)iris_msg_reply((long)reply_h, &reply);
}

void console_main_c(iris_cptr_t rbx_unused) {
    /* Phase 13 (Track I): console is endpoint-only and fully CPtr-provisioned —
     * the endpoint recv side is the IRIS_CPTR_OWN_EP mint (slot 5) and the
     * KIoPort for 0x3F8..0x3FF the IRIS_CPTR_IOPORT mint (slot 10), named by
     * CPtr like everything else (INV_IOPORT_IN/OUT).  No bootstrap KChannel
     * recv, no legacy service channel. */
    iris_cptr_t ioport_h = (iris_cptr_t)IRIS_CPTR_IOPORT;
    iris_cptr_t ep_h     = (iris_cptr_t)IRIS_CPTR_OWN_EP;

    (void)rbx_unused;   /* RBX = 0 since the KChannel bootstrap retired */

    /* D-4: a page console owns, registered as its IPC buffer.  Best-effort —
     * failing leaves the kernel staging path, which still works. */
    con_ipc_buffer_init();

    /* Endpoint-only main loop: block on the KEndpoint, serve, reply.
     * Phase S1: the explicit reply object (init retypes it from its untyped
     * pool and mints it at IRIS_CPTR_OWN_REPLY) rides in recv arg2. */
    for (;;) {
        struct iris_msg req;
        con_imsg_zero(&req);
        if ((req.reply = (long)IRIS_CPTR_OWN_REPLY, iris_msg_recv((long)ep_h, &req)) != IRIS_OK)
            continue;
        con_serve_ep_msg(ioport_h, &req);
    }
}
