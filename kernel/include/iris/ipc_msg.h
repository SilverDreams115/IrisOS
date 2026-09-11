#ifndef IRIS_IPC_MSG_H
#define IRIS_IPC_MSG_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/*
 * ipc_msg.h — the message ABI (ledger A-33).
 *
 * A message is a MESSAGE INFO word plus message registers, and for a payload
 * longer than that, the thread's registered IPC buffer.  It is not a struct in
 * user memory that the kernel dereferences, and that is the whole of what
 * A-33 changed:
 *
 *   - there is no user pointer on the message path, so there is no address to
 *     validate and none for a second thread to invalidate between the check
 *     and the copy;
 *   - a short message never touches memory at either end;
 *   - the shape of a message is a register map rather than a struct layout, so
 *     an assembly driver reads it the same way a C one does.
 *
 * seL4's `seL4_MessageInfo_t` is this word; its `seL4_SetMR`/`seL4_GetMR` are
 * the register accessors.  IRIS's differ in one respect and it is recorded
 * rather than implied: seL4 counts message registers in `length` and spills
 * past four into the IPC buffer, while IRIS has exactly four and counts BULK
 * PAYLOAD BYTES separately, because its bulk transfers are byte streams
 * (paths, file contents) rather than word arrays.
 */

#define IRIS_MSG_WORDS    4
#define IRIS_IPC_BUF_SIZE 256u  /* the floor a thread with no registered buffer
                                 * would have had; the real capacity is the
                                 * frame's (D-4) */
#define IRIS_MSG_NO_CAP   0     /* "no capability travelled" */

/*
 * ── MessageInfo ────────────────────────────────────────────────────────────
 *
 *   bits  0.. 3   length  — message words carried, 0..IRIS_MSG_WORDS
 *   bits  4..11   caps    — on a send, 1 if a capability travels; on a
 *                           receive, the RIGHTS of the capability that landed
 *   bits 12..24   buf     — bulk payload bytes in the registered IPC buffer
 *   bits 25..63   label   — the application's, 39 bits of it
 *
 * The `caps` field answers two questions with one number, and it can because
 * ZERO is unambiguous: a capability with no rights cannot be transferred at
 * all (the staging refuses `RIGHT_NONE`), so 0 means nothing arrived.  seL4
 * reports `extraCaps` and leaves rights to be inspected; IRIS reports the
 * rights, because a receiver that has to ask a second time about a capability
 * it was just handed is a receiver that can be told a different answer.
 *
 * The label is the high field so that reading it is a shift and nothing else,
 * which is what every server does on every message.
 */
/* No `u` suffixes: `services/kbd/main.S` shifts by these, and the assembler
 * reads a literal, not a C integer constant.  The C accessors below do the
 * widening they need. */
#define IRIS_MI_LEN_MASK    0xF
#define IRIS_MI_EXTRA_SHIFT 4
#define IRIS_MI_EXTRA_MASK  0xFF
#define IRIS_MI_BUF_SHIFT   12
#define IRIS_MI_BUF_MASK    0x1FFF
#define IRIS_MI_LABEL_SHIFT 25

#ifndef __ASSEMBLER__
static inline uint64_t iris_mi(uint64_t label, uint32_t len,
                               uint32_t extra, uint32_t buf) {
    return ((uint64_t)label << IRIS_MI_LABEL_SHIFT)
         | (((uint64_t)buf   & IRIS_MI_BUF_MASK)   << IRIS_MI_BUF_SHIFT)
         | (((uint64_t)extra & IRIS_MI_EXTRA_MASK) << IRIS_MI_EXTRA_SHIFT)
         |  ((uint64_t)len   & IRIS_MI_LEN_MASK);
}
static inline uint64_t iris_mi_label(uint64_t mi) { return mi >> IRIS_MI_LABEL_SHIFT; }
static inline uint32_t iris_mi_len(uint64_t mi)   { return (uint32_t)(mi & IRIS_MI_LEN_MASK); }
static inline uint32_t iris_mi_extra(uint64_t mi) {
    return (uint32_t)((mi >> IRIS_MI_EXTRA_SHIFT) & IRIS_MI_EXTRA_MASK);
}
static inline uint32_t iris_mi_buf(uint64_t mi) {
    return (uint32_t)((mi >> IRIS_MI_BUF_SHIFT) & IRIS_MI_BUF_MASK);
}
#endif

/*
 * ── where a message lives in an invocation's argument words ────────────────
 *
 * One definition, because the kernel reads these out of the thread and ring 3
 * writes them into registers, and the two agreeing is the ABI.
 *
 * SEND (`EP_Send`, `EP_NBSend`, `EP_Call`, `Reply`):
 *   a1 = MessageInfo   a2..a5 = mr0..mr3
 *   a6 = the capability to transfer, packed with its rights
 *   a7 = the slot to receive INTO — a Call declares where the reply's
 *        capability should land, which a Send has no use for
 *
 * RECEIVE (`EP_Recv`, `EP_NBRecv`) — a different map, because it is a
 * different operation:
 *   a1 = the slot to receive into    a2 = the reply object to stage
 *
 * `ReplyRecv` uses the SEND map for both halves, including its receive slot,
 * because it IS a send followed by a receive.  Getting that wrong is subtle:
 * a receive's slot and a send's MessageInfo are the same argument word.
 *
 * and every receive RETURNS, in the SAME registers the message registers went
 * out in — which is seL4's arrangement and is what makes a reply loop free of
 * shuffling:
 *   r0 = sender badge   r1 = MessageInfo   r2 = the capability delivered
 *   r3..r6 = mr0..mr3
 */
#define IRIS_MSGA_INFO   1u
#define IRIS_MSGA_MR0    2u
#define IRIS_MSGA_MR1    3u
#define IRIS_MSGA_MR2    4u
#define IRIS_MSGA_MR3    5u
#define IRIS_MSGA_CAP    6u
#define IRIS_MSGA_RECV   7u


#define IRIS_MSGR_BADGE  0u
#define IRIS_MSGR_INFO   1u
#define IRIS_MSGR_CAP    2u
#define IRIS_MSGR_MR0    3u
#define IRIS_MSGR_MR1    4u
#define IRIS_MSGR_MR2    5u
#define IRIS_MSGR_MR3    6u

/* The capability word: a CPtr and the rights to hand over with it.  Rights are
 * seven bits, so they ride in the high half and the CPtr keeps a full 32. */
#define IRIS_CAPW_RIGHTS_SHIFT 32u
#ifndef __ASSEMBLER__
static inline uint64_t iris_capw(uint64_t cptr, uint32_t rights) {
    return (cptr & 0xFFFFFFFFull) | ((uint64_t)rights << IRIS_CAPW_RIGHTS_SHIFT);
}
static inline uint32_t iris_capw_cptr(uint64_t w)   { return (uint32_t)(w & 0xFFFFFFFFull); }
static inline uint32_t iris_capw_rights(uint64_t w) { return (uint32_t)(w >> IRIS_CAPW_RIGHTS_SHIFT); }
#endif

/*
 * Ledger A-23 — the label a BOUND NOTIFICATION arrives under.
 *
 * A thread with a bound notification can be blocked receiving on an ENDPOINT
 * and still take signals: the signal wakes it out of the endpoint queue and is
 * delivered as a message.  A server therefore has to be able to tell "somebody
 * called me" from "somebody signalled me" on one receive, and the label is how
 * — the same way FAULT_MSG_NOTIFY distinguishes a fault (A-22).  mr0 carries
 * the signal bits.
 */
/* No `ull`: the assembler reads this too (kbd is a driver in asm). */
#define IRIS_MSG_LABEL_NOTIFICATION 0xF0000002

#endif /* IRIS_IPC_MSG_H */
