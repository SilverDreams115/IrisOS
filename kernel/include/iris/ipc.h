#ifndef IRIS_IPC_H
#define IRIS_IPC_H

/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  LEGACY IPC — kernel-internal use only.  Do not include this   ║
 * ║  header from new code.  Do not add new callers.                ║
 * ║                                                                 ║
 * ║  This API is a pre-capability IPC mechanism used exclusively   ║
 * ║  by the producer/consumer demo tasks in kernel_main.c.         ║
 * ║  It is not accessible from user space (SYS_IPC_* syscalls      ║
 * ║  were retired).                                                 ║
 * ║                                                                 ║
 * ║  Modern IPC:  KChannel + handle table (iris/nc/kchannel.h)     ║
 * ║  Retirement:  remove together with the demo tasks when the     ║
 * ║               nameserver-based service model is stable.        ║
 * ╚══════════════════════════════════════════════════════════════════╝
 */

#include <stdint.h>

struct task; /* forward declaration — evita dependencia circular con task.h */

#define IPC_MSG_SIZE     64
#define IPC_CHANNEL_CAP  16
#define IPC_MAX_CHANNELS 8

/* tipos de mensaje */
#define IPC_MSG_EMPTY    0
#define IPC_MSG_DATA     1
#define IPC_MSG_REQUEST  2
#define IPC_MSG_REPLY    3
#define IPC_MSG_SIGNAL   4

/* códigos de error */
#define IPC_OK           0
#define IPC_ERR_FULL    -1
#define IPC_ERR_EMPTY   -2
#define IPC_ERR_INVALID -3

struct ipc_message {
    uint32_t type;
    uint32_t sender_id;
    uint32_t receiver_id;
    uint32_t seq;
    uint8_t  data[IPC_MSG_SIZE];
    uint32_t data_len;
    uint32_t reserved;
} __attribute__((packed));

struct ipc_channel {
    uint32_t         id;
    uint32_t         owner_id;
    struct ipc_message buf[IPC_CHANNEL_CAP];
    uint32_t         head;
    uint32_t         tail;
    uint32_t         count;
    uint32_t         reserved;
    struct task     *waiter; /* tarea bloqueada esperando recv, o NULL */
};

void     ipc_init(void);
int32_t  ipc_channel_create(uint32_t owner_id);
int32_t  ipc_send(uint32_t channel_id, struct ipc_message *msg);
int32_t  ipc_recv(uint32_t channel_id, struct ipc_message *out);
int32_t  ipc_try_recv(uint32_t channel_id, struct ipc_message *out);
uint32_t ipc_channel_count(uint32_t channel_id);

#endif
