#ifndef IRIS_CONSOLE_CLIENT_H
#define IRIS_CONSOLE_CLIENT_H

#include <stdint.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/console_proto.h>
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

#endif
