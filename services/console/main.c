/*
 * console/main.c — ring-3 serial console service.
 *
 * Bootstrap protocol (over bootstrap channel from svc_loader):
 *   recv SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP → ioport_h (KIoPort for 0x3F8..0x3FF)
 *   recv SVCMGR_BOOTSTRAP_KIND_SERVICE    → service_h (KChannel, RIGHT_READ)
 *
 * Main loop: receive CONSOLE_MSG_WRITE messages on service_h and write each
 * byte to the UART THR (offset 0) after polling LSR (offset 5) for THRE.
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/console_proto.h>
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

void console_main_c(handle_id_t bootstrap_h) {
    handle_id_t ioport_h  = HANDLE_INVALID;
    handle_id_t service_h = HANDLE_INVALID;
    struct KChanMsg msg;

    /* Bootstrap: receive IOPORT_CAP and SERVICE handles. */
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

    /* Main loop: receive CONSOLE_MSG_WRITE, emit bytes to UART. */
    for (;;) {
        con_msg_zero(&msg);
        if (con_sys2(SYS_CHAN_RECV, (long)service_h, (long)&msg) != IRIS_OK)
            continue;
        if (msg.type != CONSOLE_MSG_WRITE) continue;
        if (msg.data_len < 4u) continue;

        uint32_t len = (uint32_t)msg.data[0] |
                       ((uint32_t)msg.data[1] << 8);
        if (len > CONSOLE_WRITE_MAX) len = CONSOLE_WRITE_MAX;

        uint32_t i;
        for (i = 0; i < len; i++)
            con_uart_write_byte(ioport_h, msg.data[4 + i]);
    }
}
