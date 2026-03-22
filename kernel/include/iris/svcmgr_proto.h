#ifndef IRIS_SVCMGR_PROTO_H
#define IRIS_SVCMGR_PROTO_H

#include <stdint.h>

/*
 * IRIS Service Manager Bootstrap Protocol
 *
 * ── Overview ─────────────────────────────────────────────────────
 * Messages flow over the svcmgr bootstrap KChannel between the kernel
 * and the ring-3 service manager process.
 *
 *   kernel → svcmgr : SVCMGR_MSG_SPAWN_SERVICE
 *   svcmgr → kernel : SVCMGR_MSG_ACK (phase 2)
 *
 * The kernel sends spawn requests via the retained svcmgr_bootstrap_ch
 * pointer in svcmgr_bootstrap.c; svcmgr reads via SYS_CHAN_RECV.
 *
 * ── KChanMsg wire layout (76 bytes) ─────────────────────────────
 *   offset  0: uint32_t type      → SVCMGR_MSG_* opcode
 *   offset  4: uint32_t sender_id → 0 = kernel, N = task_id
 *   offset  8: uint8_t  data[64]  → payload (see below)
 *   offset 72: uint32_t data_len  → payload length in bytes
 *
 * ── SVCMGR_MSG_SPAWN_SERVICE (kernel → svcmgr) ──────────────────
 * Asks svcmgr to spawn a compiled-in ring-3 service and register it
 * in the nameserver.  IRQ routing is handled kernel-side before this
 * message is sent (irq_routing_register is a kernel-only operation).
 *
 *   data[SVCMGR_SPAWN_OFF_ENTRY]      uint64_t    ring-3 entry_vaddr
 *   data[SVCMGR_SPAWN_OFF_NAME]       char[16]    NS service name (NS_NAME_LEN)
 *   data[SVCMGR_SPAWN_OFF_RIGHTS]     uint32_t    rights for NS registration
 *   data[SVCMGR_SPAWN_OFF_REG_CHAN]   handle_id_t pre-created channel handle
 *                                                 already inserted into svcmgr's
 *                                                 handle table by the kernel.
 *                                                 svcmgr registers this channel
 *                                                 under NS name with rights.
 *                                                 0 = no pre-created channel
 *                                                 (use SYS_SPAWN bootstrap ch).
 *   data[SVCMGR_SPAWN_OFF_REPLY_NAME] char[16]    NS name for the reply channel.
 *                                                 svcmgr creates a new KChannel
 *                                                 via SYS_CHAN_CREATE, registers
 *                                                 it here with RIGHT_READ|WRITE,
 *                                                 then releases its own handle.
 *                                                 Empty string = no reply channel.
 *
 * ── SVCMGR_MSG_ACK (svcmgr → kernel, phase 2) ───────────────────
 *   data[SVCMGR_ACK_OFF_TASK_ID]  uint32_t  spawned task id (0 = failed)
 *   data[SVCMGR_ACK_OFF_ERR]      int32_t   0 = OK, negative = iris_error_t
 *
 * ── Phase status ─────────────────────────────────────────────────
 * Phase 2 (current): svcmgr dispatches SVCMGR_MSG_SPAWN_SERVICE.
 *   For each compiled-in service the kernel sends this message; svcmgr
 *   calls SYS_SPAWN + SYS_NS_REGISTER.  kbd_bootstrap_init() has been
 *   removed.  IRQ routing stays kernel-side (permanent kernel concern).
 *   irq_routing owner = svcmgr's KProcess; auto-cleanup fires when
 *   svcmgr exits via kprocess_teardown → irq_routing_unregister_owner.
 */

#define SVCMGR_MSG_SPAWN_SERVICE  0x0001u
#define SVCMGR_MSG_ACK            0x8001u

/* Byte offsets within KChanMsg.data[64] */
#define SVCMGR_SPAWN_OFF_ENTRY      0  /* uint64_t:    ring-3 entry_vaddr          */
#define SVCMGR_SPAWN_OFF_NAME       8  /* char[16]:    service name (NS_NAME_LEN)  */
#define SVCMGR_SPAWN_OFF_RIGHTS    24  /* uint32_t:    ns registration rights      */
#define SVCMGR_SPAWN_OFF_REG_CHAN  28  /* handle_id_t: pre-created channel handle
                                        *   in svcmgr's table; registered under
                                        *   NS name.  0 = use SYS_SPAWN ch.       */
#define SVCMGR_SPAWN_OFF_REPLY_NAME 32 /* char[16]:    NS name for reply channel.
                                        *   svcmgr creates via SYS_CHAN_CREATE,
                                        *   registers, then closes its handle.
                                        *   Empty string = no reply channel.      */

#define SVCMGR_ACK_OFF_TASK_ID    0    /* uint32_t: spawned task id        */
#define SVCMGR_ACK_OFF_ERR        4    /* int32_t:  0=OK, <0=iris_error_t  */

/* data_len values */
#define SVCMGR_SPAWN_MSG_LEN      48u  /* 8 + 16 + 4 + 4 + 16 */
#define SVCMGR_ACK_MSG_LEN        8u   /* 4 + 4 */

#endif
