#ifndef IRIS_CONSOLE_CLIENT_H
#define IRIS_CONSOLE_CLIENT_H

#include <stdint.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/console_proto.h>
#include <iris/console_ep_proto.h>
#include <iris/ipc_msg.h>
#include <iris/syscall.h>

/*
 * console_write — send a NUL-terminated string to the console service.
 *
 * Silently drops the call if h is HANDLE_INVALID (e.g. before bootstrap
 * completes).  Splits long strings into CONSOLE_WRITE_MAX-byte chunks and
 * sends each as a separate CONSOLE_MSG_WRITE message.
 *
 * Wire layout of msg.data for CONSOLE_MSG_WRITE:
 *   bytes 0-3: payload length (little-endian uint32_t)
 *   bytes 4..4+len-1: character payload
 */
static inline void console_write(handle_id_t h, const char *s) {
    if (h == HANDLE_INVALID || !s) return;

    uint32_t len = 0;
    while (s[len]) len++;

    while (len > 0) {
        struct KChanMsg msg;
        uint8_t *raw = (uint8_t *)&msg;
        uint32_t i;
        for (i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;

        uint32_t chunk = (len < CONSOLE_WRITE_MAX) ? len : CONSOLE_WRITE_MAX;
        msg.type = CONSOLE_MSG_WRITE;
        msg.data[0] = (uint8_t)(chunk & 0xFFu);
        msg.data[1] = (uint8_t)((chunk >> 8) & 0xFFu);
        msg.data[2] = 0;
        msg.data[3] = 0;
        for (i = 0; i < chunk; i++) msg.data[4 + i] = (uint8_t)s[i];
        msg.data_len = 4u + chunk;
        msg.attached_handle = HANDLE_INVALID;
        msg.attached_rights = 0;

        long ret;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((long)SYS_CHAN_SEND),
              "D"((long)(uint32_t)h),
              "S"((long)(uintptr_t)&msg)
            : "rcx", "r11", "memory");
        (void)ret;

        s   += chunk;
        len -= chunk;
    }
}

static inline long console_client_sys3_(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

/*
 * console_sync — flush barrier (CONSOLE_MSG_SYNC, protocol v2).
 *
 * Blocks until every console_write queued on h before this call has been
 * emitted to the UART, or until a 2 s timeout expires (console dead, or a
 * pre-v2 console that drops unknown message types).  Best-effort: silently
 * returns on any failure; it never leaves the caller hung.
 */
static inline void console_sync(handle_id_t h) {
    if (h == HANDLE_INVALID) return;

    long ch_raw = console_client_sys3_((long)SYS_CHAN_CREATE, 0, 0, 0);
    if (ch_raw < 0) return;

    long rd_raw = console_client_sys3_((long)SYS_HANDLE_DUP, ch_raw,
                                       (long)RIGHT_READ, 0);
    long wr_raw = console_client_sys3_((long)SYS_HANDLE_DUP, ch_raw,
                                       (long)(RIGHT_WRITE | RIGHT_TRANSFER), 0);
    (void)console_client_sys3_((long)SYS_HANDLE_CLOSE, ch_raw, 0, 0);
    if (rd_raw < 0 || wr_raw < 0) {
        if (rd_raw >= 0)
            (void)console_client_sys3_((long)SYS_HANDLE_CLOSE, rd_raw, 0, 0);
        if (wr_raw >= 0)
            (void)console_client_sys3_((long)SYS_HANDLE_CLOSE, wr_raw, 0, 0);
        return;
    }

    struct KChanMsg msg;
    uint8_t *raw = (uint8_t *)&msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
    msg.type            = CONSOLE_MSG_SYNC;
    msg.attached_handle = (handle_id_t)wr_raw;
    msg.attached_rights = RIGHT_WRITE;

    long r = console_client_sys3_((long)SYS_CHAN_SEND, (long)(uint32_t)h,
                                  (long)(uintptr_t)&msg, 0);
    if (r == 0) {
        /* wr_raw consumed by the send; wait for the ack (bounded). */
        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        (void)console_client_sys3_((long)SYS_CHAN_RECV_TIMEOUT, rd_raw,
                                   (long)(uintptr_t)&msg, 2000000000L);
    } else {
        (void)console_client_sys3_((long)SYS_HANDLE_CLOSE, wr_raw, 0, 0);
    }
    (void)console_client_sys3_((long)SYS_HANDLE_CLOSE, rd_raw, 0, 0);
}

/*
 * console_ep_write — synchronous write over the console KEndpoint
 * (Fase 7.3).  Chunks by IRIS_IPC_BUF_SIZE; each EP_CALL returns only after
 * the chunk has been emitted to the UART.  buf must be a caller-provided
 * staging buffer of at least IRIS_IPC_BUF_SIZE bytes (EP bulk payloads are
 * read from user memory).  Returns 0 on success, negative on first failure.
 */
static inline long console_ep_write(handle_id_t ep_h, uint8_t *buf,
                                    const char *s) {
    if (ep_h == HANDLE_INVALID || !s || !buf) return -1;

    uint32_t len = 0;
    while (s[len]) len++;

    uint32_t off = 0;
    do {
        uint32_t chunk = len - off;
        if (chunk > IRIS_IPC_BUF_SIZE) chunk = IRIS_IPC_BUF_SIZE;
        for (uint32_t i = 0; i < chunk; i++) buf[i] = (uint8_t)s[off + i];

        struct IrisMsg msg;
        uint8_t *raw = (uint8_t *)&msg;
        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        msg.label    = CONSOLE_EP_OP_WRITE;
        msg.buf_uptr = (uint64_t)(uintptr_t)buf;
        msg.buf_len  = chunk;

        long ret;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((long)SYS_EP_CALL), "D"((long)ep_h), "S"((long)&msg)
            : "rcx", "r11", "memory");
        if (ret != 0) return ret;
        if (msg.label != IRIS_EP_REPLY_OK) return -1;
        off += chunk;
    } while (off < len);
    return 0;
}

/*
 * console_ep_sync — cross-path flush barrier over the console KEndpoint.
 * Returns after the console has drained all legacy KChannel writes queued
 * before this call (EP writes are already synchronous).
 */
static inline long console_ep_sync(handle_id_t ep_h) {
    if (ep_h == HANDLE_INVALID) return -1;
    struct IrisMsg msg;
    uint8_t *raw = (uint8_t *)&msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
    msg.label = CONSOLE_EP_OP_SYNC;
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"((long)SYS_EP_CALL), "D"((long)ep_h), "S"((long)&msg)
        : "rcx", "r11", "memory");
    if (ret != 0) return ret;
    return (msg.label == IRIS_EP_REPLY_OK) ? 0 : -1;
}

#endif
