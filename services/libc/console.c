/* SPDX-License-Identifier: Apache-2.0 */
/*
 * console.c — where a program's output goes, and the capability it goes over.
 *
 * `IRIS_CPTR_CONSOLE_EP` is a send-only endpoint the spawner minted into slot
 * 3 before this program existed.  There is no device here, no driver and no
 * file: the console is a service, and a program that was given no capability
 * to it simply cannot print.  That is a thing a spawner may choose, so a write
 * with no console is SHORT rather than fatal.
 */
#include "libc_internal.h"
#include "../common/iris_msg.h"
#include "../common/iris_ipc_buffer.h"
#include "../common/console_client.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/endpoint_proto.h>

/* This libc's own slots, out of the range docs/contracts/program.md §2 gives
 * the program (32..63).  A C program never names a slot, so these are the
 * library's to choose; they are grouped here so a future one is added in an
 * obvious place rather than found by collision. */
#define LIBC_SLOT_IPCBUF     36u
#define LIBC_SLOT_IPCBUF_PT  37u

static uint8_t *g_buf;

uint8_t *__libc_ipc_buf(void) { return g_buf; }

void __libc_console_init(void) {
    if (!__libc.untyped) return;
    g_buf = (uint8_t *)iris_ipc_buffer_init_from(__libc.untyped,
                                                 IRIS_CPTR_OWN_VSPACE,
                                                 IRIS_CPTR_OWN_TCB,
                                                 LIBC_SLOT_IPCBUF,
                                                 LIBC_SLOT_IPCBUF_PT,
                                                 IRIS_IPC_BUFFER_VA);
    if (!g_buf) return;
    /* Ask the console whether it is there.  A program that will print a
     * thousand lines should not discover on the first one that slot 3 was
     * empty — and `console` answers a PING like every endpoint here. */
    {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label = IRIS_EP_OP_PING;
        if (iris_msg_call((long)IRIS_CPTR_CONSOLE_EP, &m) == 0 &&
            m.label == IRIS_EP_REPLY_OK)
            __libc.console = 1;
    }
}

void __libc_console_write(const char *s, size_t n) {
    if (!__libc.console || !g_buf || !s) return;
    while (n) {
        uint32_t chunk = (n > IRIS_IPC_BUF_SIZE) ? IRIS_IPC_BUF_SIZE : (uint32_t)n;
        struct iris_msg m;
        for (uint32_t i = 0; i < chunk; i++) g_buf[i] = (uint8_t)s[i];
        iris_msg_zero(&m);
        m.label   = CONSOLE_EP_OP_WRITE;
        m.buf_len = chunk;
        if (iris_msg_call((long)IRIS_CPTR_CONSOLE_EP, &m) != 0) return;
        s += chunk;
        n -= chunk;
    }
}
