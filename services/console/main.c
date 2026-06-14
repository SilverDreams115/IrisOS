/*
 * console/main.c — ring-3 serial console service.
 *
 * Bootstrap protocol (over bootstrap channel from svc_loader):
 *   recv SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP → ioport_h (KIoPort for 0x3F8..0x3FF)
 *   recv SVCMGR_BOOTSTRAP_KIND_SERVICE    → service_h (KChannel, RIGHT_READ)
 *   recv SVCMGR_BOOTSTRAP_KIND_SERVICE_EP → ep_h (KEndpoint recv, Fase 7.3)
 *
 * Main loop (Fase 7.3): endpoint-first. Drain pending EP requests
 * (CONSOLE_EP_OP_WRITE / SYNC / PING — iris/console_ep_proto.h), then wait
 * on the legacy KChannel with a 5 ms timeout for the remaining legacy
 * writer (svcmgr). EP WRITE replies only after the bytes hit the UART;
 * EP SYNC drains the legacy queue before replying (cross-path barrier).
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/console_proto.h>
#include <iris/console_ep_proto.h>
#include <iris/ipc_msg.h>
#include <iris/nc/error.h>

static inline long con_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long con_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long con_sys1(long nr, long a0) {
    return con_sys2(nr, a0, 0);
}


/* Poll the UART Line Status Register (offset 5) until bit 5 (THRE) is set,
 * then write one byte to the Transmit Holding Register (offset 0). */
static void con_uart_write_byte(handle_id_t ioport_h, uint8_t byte) {
    long v;
    /* Wait for THRE (bit 5 of LSR at offset 5). */
    do {
        v = con_sys2(SYS_IOPORT_IN, (long)ioport_h, 5);
    } while (v < 0 || !((uint8_t)v & 0x20u));
    (void)con_sys3(SYS_IOPORT_OUT, (long)ioport_h, 0, (long)byte);
}

/* Fase 13 (Track I): the legacy KChannel write path (con_serve_chan_msg /
 * con_drain_chan, CONSOLE_MSG_WRITE/SYNC) is retired — console is endpoint-only.
 * Its sole writers (svcmgr klog drain, init logging) now use console.ep. */

static uint8_t g_con_ep_buf[IRIS_IPC_BUF_SIZE];

static void con_imsg_zero(struct IrisMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

static void con_ep_reply_err(struct IrisMsg *reply, int32_t err) {
    con_imsg_zero(reply);
    reply->label      = IRIS_EP_REPLY_ERR;
    reply->words[0]   = (uint64_t)(uint32_t)err;
    reply->word_count = 1u;
}

/* Serve one endpoint request; exactly one reply per request. */
static void con_serve_ep_msg(handle_id_t ioport_h, struct IrisMsg *req) {
    struct IrisMsg reply;
    handle_id_t reply_h = (handle_id_t)req->attached_handle;

    switch (req->label) {
    case CONSOLE_EP_OP_WRITE: {
        uint32_t len = req->buf_len;
        if (len > (uint32_t)sizeof(g_con_ep_buf)) len = (uint32_t)sizeof(g_con_ep_buf);
        for (uint32_t i = 0; i < len; i++)
            con_uart_write_byte(ioport_h, g_con_ep_buf[i]);
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
        /* Fase 13 (Track I): no legacy KChannel writers remain — EP writes are
         * synchronous by construction, so SYNC is a trivial acknowledge. */
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        reply.word_count = 1u;
        break;
    case IRIS_EP_OP_PING:
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        /* Fase 9 PING convention: echo the kernel-stamped sender badge. */
        reply.words[1]   = req->sender_badge;
        reply.word_count = 2u;
        break;
    default:
        con_ep_reply_err(&reply, IRIS_ERR_NOT_SUPPORTED);
        break;
    }

    if (reply_h != HANDLE_INVALID) {
        (void)con_sys2(SYS_REPLY, (long)reply_h, (long)&reply);
        (void)con_sys1(SYS_HANDLE_CLOSE, (long)reply_h);
    }
}

void console_main_c(handle_id_t bootstrap_h) {
    /* Fase 13 (Track I): console is endpoint-only and fully CPtr-provisioned —
     * the endpoint recv side is the IRIS_CPTR_OWN_EP mint (slot 5) and the
     * KIoPort for 0x3F8..0x3FF the IRIS_CPTR_IOPORT mint (slot 10), resolved by
     * CPtr through the device-cap dual resolver (SYS_IOPORT_IN/OUT).  No
     * bootstrap KChannel recv, no legacy service channel. */
    handle_id_t ioport_h = (handle_id_t)IRIS_CPTR_IOPORT;
    handle_id_t ep_h     = (handle_id_t)IRIS_CPTR_OWN_EP;

    (void)con_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_h);

    /* Endpoint-only main loop: block on the KEndpoint, serve, reply. */
    for (;;) {
        struct IrisMsg req;
        con_imsg_zero(&req);
        req.buf_uptr = (uint64_t)(uintptr_t)g_con_ep_buf;
        if (con_sys2(SYS_EP_RECV, (long)ep_h, (long)&req) != IRIS_OK)
            continue;
        con_serve_ep_msg(ioport_h, &req);
    }
}
