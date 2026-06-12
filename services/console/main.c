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
#include <iris/nc/kchannel.h>
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

static void con_msg_zero(struct KChanMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
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

/* Serve one legacy KChannel message (CONSOLE_MSG_WRITE / SYNC). */
static void con_serve_chan_msg(handle_id_t ioport_h, struct KChanMsg *msg) {
    if (msg->type == CONSOLE_MSG_SYNC) {
        /* Flush barrier: every WRITE queued before this SYNC has already
         * been emitted (the service channel is FIFO) — ack and move on. */
        if (msg->attached_handle != HANDLE_INVALID) {
            struct KChanMsg ack;
            con_msg_zero(&ack);
            ack.type = CONSOLE_MSG_SYNC_ACK;
            (void)con_sys2(SYS_CHAN_SEND,
                           (long)msg->attached_handle, (long)&ack);
            (void)con_sys1(SYS_HANDLE_CLOSE, (long)msg->attached_handle);
        }
        return;
    }
    if (msg->type != CONSOLE_MSG_WRITE) return;
    if (msg->data_len < 4u) return;

    uint32_t len = (uint32_t)msg->data[0] |
                   ((uint32_t)msg->data[1] << 8);
    if (len > CONSOLE_WRITE_MAX) len = CONSOLE_WRITE_MAX;

    uint32_t i;
    for (i = 0; i < len; i++)
        con_uart_write_byte(ioport_h, msg->data[4 + i]);
}

/* Drain every queued legacy message without blocking (EP SYNC barrier). */
static void con_drain_chan(handle_id_t ioport_h, handle_id_t service_h) {
    struct KChanMsg msg;
    for (;;) {
        con_msg_zero(&msg);
        if (con_sys2(SYS_CHAN_RECV_NB, (long)service_h, (long)&msg) != IRIS_OK)
            return;
        con_serve_chan_msg(ioport_h, &msg);
    }
}

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
static void con_serve_ep_msg(handle_id_t ioport_h, handle_id_t service_h,
                             struct IrisMsg *req) {
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
        /* Cross-path barrier: emit everything legacy writers queued
         * before this point, then acknowledge. */
        con_drain_chan(ioport_h, service_h);
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        reply.word_count = 1u;
        break;
    case IRIS_EP_OP_PING:
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        reply.word_count = 1u;
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
    handle_id_t ioport_h  = HANDLE_INVALID;
    handle_id_t service_h = HANDLE_INVALID;
    /* Fase 8: the endpoint recv side is minted by init at IRIS_CPTR_OWN_EP
     * (bootstrap kind 0x21 retired); the slot is invoked directly. */
    handle_id_t ep_h      = (handle_id_t)IRIS_CPTR_OWN_EP;
    struct KChanMsg msg;

    /* Bootstrap: receive IOPORT_CAP and SERVICE handles — the unavoidable
     * handle boundary (KIoPort and KChannel caps cannot live in CSpace
     * slots; see endpoint_proto.h). */
    while (ioport_h == HANDLE_INVALID || service_h == HANDLE_INVALID) {
        con_msg_zero(&msg);
        if (con_sys2(SYS_CHAN_RECV, (long)bootstrap_h, (long)&msg) != IRIS_OK)
            break;
        if (msg.type != SVCMGR_MSG_BOOTSTRAP_HANDLE ||
            msg.attached_handle == HANDLE_INVALID) {
            if (msg.attached_handle != HANDLE_INVALID)
                (void)con_sys1(SYS_HANDLE_CLOSE, (long)msg.attached_handle);
            continue;
        }
        uint32_t kind = (uint32_t)msg.data[0] |
                        ((uint32_t)msg.data[1] << 8) |
                        ((uint32_t)msg.data[2] << 16) |
                        ((uint32_t)msg.data[3] << 24);
        if (kind == SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP && ioport_h == HANDLE_INVALID) {
            ioport_h = msg.attached_handle;
        } else if (kind == SVCMGR_BOOTSTRAP_KIND_SERVICE && service_h == HANDLE_INVALID) {
            service_h = msg.attached_handle;
        } else {
            (void)con_sys1(SYS_HANDLE_CLOSE, (long)msg.attached_handle);
        }
    }
    (void)con_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_h);

    if (ioport_h == HANDLE_INVALID || service_h == HANDLE_INVALID)
        return;

    /* Main loop: endpoint-first, legacy KChannel with timeout (Fase 7.3). */
    for (;;) {
        while (ep_h != HANDLE_INVALID) {
            struct IrisMsg req;
            con_imsg_zero(&req);
            req.buf_uptr = (uint64_t)(uintptr_t)g_con_ep_buf;
            if (con_sys2(SYS_EP_NB_RECV, (long)ep_h, (long)&req) != IRIS_OK)
                break;
            con_serve_ep_msg(ioport_h, service_h, &req);
        }

        con_msg_zero(&msg);
        long r = con_sys3(SYS_CHAN_RECV_TIMEOUT, (long)service_h, (long)&msg,
                          5000000L /* 5 ms */);
        if (r != IRIS_OK)
            continue;  /* TIMED_OUT keeps the EP drain alive */
        con_serve_chan_msg(ioport_h, &msg);
    }
}
