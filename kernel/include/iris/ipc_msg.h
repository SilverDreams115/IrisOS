#ifndef IRIS_IPC_MSG_H
#define IRIS_IPC_MSG_H

#include <stdint.h>

#define IRIS_MSG_WORDS    4u
#define IRIS_IPC_BUF_SIZE 256u  /* max extra payload bytes per endpoint IPC */
#define IRIS_MSG_NO_CAP   0u    /* attached_handle value meaning "no capability" */

/*
 * Inline IPC message — 72 bytes (Fase 9: +sender_badge).
 *
 *   label / words[]: the primary inline payload (up to 4 machine words).
 *   word_count:      how many of words[] carry valid data (0..IRIS_MSG_WORDS).
 *   buf_len:         bytes of extra bulk payload (Ph69); 0 = none.
 *   buf_uptr:        On SEND: user pointer to the bulk source buffer.
 *                    On RECV (output): user pointer where bulk data was written.
 *   attached_handle: capability handle being transferred (Ph68); IRIS_MSG_NO_CAP = none.
 *   attached_rights: rights to grant on the received cap (Ph68).
 *   sender_badge:    KERNEL-STAMPED sender identity (Fase 9).  On every
 *                    EP_SEND / EP_NB_SEND / EP_CALL the kernel OVERWRITES
 *                    this field with the badge of the capability the sender
 *                    invoked — whatever the sender wrote here is discarded,
 *                    so a badge can never be forged from payload.  The
 *                    receiver reads it on EP_RECV / EP_NB_RECV.  Replies
 *                    (SYS_REPLY) deliver sender_badge = 0 to the caller:
 *                    reply identity is implied by the one-shot KReply.
 *                    0 = unbadged capability (legacy / master cap).
 */
struct IrisMsg {
    uint64_t label;
    uint64_t words[IRIS_MSG_WORDS];
    uint32_t word_count;
    uint32_t buf_len;
    uint64_t buf_uptr;
    uint32_t attached_handle;  /* handle_id_t — uint32_t; 0 = IRIS_MSG_NO_CAP */
    uint32_t attached_rights;  /* iris_rights_t — uint32_t */
    uint64_t sender_badge;     /* Fase 9 — kernel-stamped, never user-set */
};
/* sizeof = 8 + 32 + 4 + 4 + 8 + 4 + 4 + 8 = 72 bytes.
 * Field offsets are ABI (asm consumers: services/kbd/main.S). */
#ifndef __ASSEMBLER__
_Static_assert(sizeof(struct IrisMsg) == 72u, "IrisMsg ABI size");
_Static_assert(__builtin_offsetof(struct IrisMsg, sender_badge) == 64u,
               "sender_badge ABI offset");
#endif

#endif /* IRIS_IPC_MSG_H */
