#ifndef IRIS_CONSOLE_CLIENT_H
#define IRIS_CONSOLE_CLIENT_H

#include <stdint.h>
#include <iris/nc/handle.h>
#include <iris/console_ep_proto.h>
#include <iris/ipc_msg.h>
#include <iris/syscall.h>

/* Fase 13/Track G: console_write / console_sync (legacy KChannel) retired —
 * the console is endpoint-only; use console_ep_write / console_ep_sync. */

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
