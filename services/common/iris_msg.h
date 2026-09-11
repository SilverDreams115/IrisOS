#ifndef IRIS_COMMON_MSG_H
#define IRIS_COMMON_MSG_H

#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/ipc_msg.h>

/*
 * iris_msg.h — marshalling, for ring 3 (ledger A-33).
 *
 * A message is a MessageInfo word and message registers.  `struct iris_msg` is
 * a place to compose one before it goes into those registers and to unpack one
 * after it comes back — a CONVENIENCE, never the ABI.  The kernel has never
 * seen it and cannot: it holds no pointer to it, which is the whole of what
 * A-33 changed.  seL4 has the same split and calls its half `seL4_SetMR` and
 * `seL4_GetMR`.
 *
 * What this replaced WAS the ABI: the kernel took a pointer to an 80-byte
 * struct, validated the range, and copied it each way for a message that was
 * usually two words.
 */
struct iris_msg {
    uint64_t label;
    uint64_t words[IRIS_MSG_WORDS];
    uint32_t word_count;
    uint32_t buf_len;     /* bulk bytes, in the thread's registered IPC buffer */

    /*
     * The three capability fields, which used to be two.
     *
     * `attached_handle` meant a capability being SENT on a Send, the slot a
     * Call wanted the reply's capability delivered INTO, and the capability a
     * receive HAD been given — three things in one field, told apart by which
     * call was looking at it.  The register map separates them (`ipc_msg.h`),
     * so the struct does too, and the compiler finds every site that meant
     * something other than what it said.
     */
    long     cap;         /* a capability to hand over, or 0 for none */
    uint32_t cap_rights;
    long     recv_slot;   /* where a capability delivered to US should land */
    long     reply;       /* the reply object a receive stages, or 0 */

    /* filled in by anything that receives: */
    uint64_t sender_badge;
    long     got_cap;     /* the reply object owed, or what a reply delivered */
    uint32_t got_caps;    /* the RIGHTS of the capability that landed in
                           * `recv_slot`, or 0 for none — a capability with no
                           * rights cannot be transferred, so 0 is unambiguous */
};

static inline void iris_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (unsigned i = 0; i < sizeof(*m); i++) b[i] = 0;
}

/*
 * The message syscall: nine words out, seven back.
 *
 * Every argument register is "+r" because the kernel writes the return message
 * into them, so the compiler must not assume any survived.  r15, r14 and r13
 * are callee-saved in the C ABI and GCC will spill them around this — the
 * price of a message that never touches memory, and the same price seL4 pays
 * for using r15 as a message register.
 *
 * Each operation below fills the words by its OWN map, because a send's map is
 * not a receive's.  `ipc_msg.h` is where both are written down.
 */
/* A send's map: MessageInfo, four message registers, the capability word, and
 * the slot a Call wants the REPLY's capability delivered into.
 *
 * Every value is computed into an ORDINARY local first and the register
 * variables are assigned immediately before the asm.  That ordering is not
 * style: a register variable must not live across a function call, and
 * `iris_mi` and `iris_capw` are calls whenever the compiler declines to inline
 * them.  Written the other way round this clobbered r10, r8 and r9 between
 * being set and being read — the syscall arrived with a message made of
 * whatever those calls left behind, and the kernel answered NOT_SUPPORTED
 * because even the label was gone. */
static inline long iris_msg_op(long cptr, unsigned long label,
                               struct iris_msg *m, int unpack) {
    long ret;
    const long v_mi  = (long)iris_mi(m->label, m->word_count,
                                     m->cap ? 1u : 0u, m->buf_len);
    const long v_m0  = (long)m->words[0];
    const long v_m1  = (long)m->words[1];
    const long v_m2  = (long)m->words[2];
    const long v_m3  = (long)m->words[3];
    const long v_cap = (long)iris_capw((uint64_t)m->cap, m->cap_rights);
    const long v_rcv = m->recv_slot;
    const long v_lab = (long)label;

    register long r_di __asm__("rdi") = cptr;
    register long r_si __asm__("rsi") = v_lab;
    register long r_dx __asm__("rdx") = v_mi;
    register long r_10 __asm__("r10") = v_m0;
    register long r_8  __asm__("r8")  = v_m1;
    register long r_9  __asm__("r9")  = v_m2;
    register long r_15 __asm__("r15") = v_m3;
    register long r_14 __asm__("r14") = v_cap;
    register long r_13 __asm__("r13") = v_rcv;

    __asm__ volatile ("syscall"
        : "=a"(ret), "+r"(r_di), "+r"(r_si), "+r"(r_dx), "+r"(r_10),
          "+r"(r_8), "+r"(r_9), "+r"(r_15)
        : "a"((long)SYS_INVOKE), "r"(r_14), "r"(r_13)
        : "rcx", "r11", "memory");

    /* A-33: a call that FAILED delivered no message, and the kernel wrote no
     * return words — the argument registers still hold what went out.  Reading
     * them as a message is how a refused receive came back carrying its own
     * MessageInfo and looking like a delivered capability. */
    if (unpack && ret == 0) {
        uint64_t mi = (uint64_t)r_si;
        long b = r_di, c = r_dx, w0 = r_10, w1 = r_8, w2 = r_9, w3 = r_15;
        m->sender_badge = (uint64_t)b;
        m->got_cap      = c;
        m->label        = iris_mi_label(mi);
        m->word_count   = iris_mi_len(mi);
        m->buf_len      = iris_mi_buf(mi);
        m->got_caps     = iris_mi_extra(mi);
    m->cap_rights   = m->got_caps;
        m->cap_rights   = m->got_caps;
        m->words[0] = (uint64_t)w0; m->words[1] = (uint64_t)w1;
        m->words[2] = (uint64_t)w2; m->words[3] = (uint64_t)w3;
        m->cap = 0;
    }
    return ret;
}

/* A receive's map: the slot to deliver into, and the reply object to stage. */
static inline long iris_msg_recv_op(long ep, unsigned long label,
                                    struct iris_msg *m) {
    long ret;
    long slot = m->recv_slot, reply = m->reply;
    register long r_di __asm__("rdi") = ep;
    register long r_si __asm__("rsi") = (long)label;
    register long r_dx __asm__("rdx") = slot;
    register long r_10 __asm__("r10") = reply;
    register long r_8  __asm__("r8")  = 0;
    register long r_9  __asm__("r9")  = 0;
    register long r_15 __asm__("r15") = 0;

    __asm__ volatile ("syscall"
        : "=a"(ret), "+r"(r_di), "+r"(r_si), "+r"(r_dx), "+r"(r_10),
          "+r"(r_8), "+r"(r_9), "+r"(r_15)
        : "a"((long)SYS_INVOKE)
        : "rcx", "r11", "memory");

    uint64_t mi = (uint64_t)r_si;
    long b = r_di, c = r_dx, w0 = r_10, w1 = r_8, w2 = r_9, w3 = r_15;
    iris_msg_zero(m);
    m->recv_slot    = slot;
    m->reply        = reply;
    /* A-33: a receive that failed delivered nothing, and the kernel wrote no
     * return words.  Everything below would be the arguments read back. */
    if (ret != 0) return ret;
    m->sender_badge = (uint64_t)b;
    m->got_cap      = c;
    m->label        = iris_mi_label(mi);
    m->word_count   = iris_mi_len(mi);
    m->buf_len      = iris_mi_buf(mi);
    m->got_caps     = iris_mi_extra(mi);
    m->cap_rights   = m->got_caps;
    m->words[0] = (uint64_t)w0; m->words[1] = (uint64_t)w1;
    m->words[2] = (uint64_t)w2; m->words[3] = (uint64_t)w3;
    return ret;
}

static inline long iris_msg_send(long ep, const struct iris_msg *m) {
    struct iris_msg c = *m; return iris_msg_op(ep, INV_EP_SEND, &c, 0);
}
static inline long iris_msg_nb_send(long ep, const struct iris_msg *m) {
    struct iris_msg c = *m; return iris_msg_op(ep, INV_EP_NB_SEND, &c, 0);
}
/* A Call declares where the REPLY's capability should land before it blocks,
 * so `recv_slot` travels with the send. */
static inline long iris_msg_call(long ep, struct iris_msg *m) {
    return iris_msg_op(ep, INV_EP_CALL, m, 1);
}
static inline long iris_msg_reply(long reply, const struct iris_msg *m) {
    struct iris_msg c = *m; return iris_msg_op(reply, INV_REPLY_SEND, &c, 0);
}
static inline long iris_msg_recv(long ep, struct iris_msg *m) {
    return iris_msg_recv_op(ep, INV_EP_RECV, m);
}
static inline long iris_msg_nb_recv(long ep, struct iris_msg *m) {
    return iris_msg_recv_op(ep, INV_EP_NB_RECV, m);
}

/*
 * ReplyRecv: reply from `m`, then receive into it.
 *
 * The reply object rides in the word a send would use for the capability it is
 * handing over, because a reply carries none — the syscall clears the count
 * itself, and says so in its contract.
 */
static inline long iris_msg_reply_recv(long ep, struct iris_msg *m) {
    long ret;
    const long v_mi = (long)iris_mi(m->label, m->word_count, 0u, m->buf_len);
    const long v_m0 = (long)m->words[0];
    const long v_m1 = (long)m->words[1];
    const long v_m2 = (long)m->words[2];
    const long v_m3 = (long)m->words[3];
    const long slot = m->recv_slot, reply = m->reply;

    register long r_di __asm__("rdi") = ep;
    register long r_si __asm__("rsi") = (long)INV_EP_REPLY_RECV;
    register long r_dx __asm__("rdx") = v_mi;
    register long r_10 __asm__("r10") = v_m0;
    register long r_8  __asm__("r8")  = v_m1;
    register long r_9  __asm__("r9")  = v_m2;
    register long r_15 __asm__("r15") = v_m3;
    register long r_14 __asm__("r14") = reply;
    register long r_13 __asm__("r13") = slot;

    __asm__ volatile ("syscall"
        : "=a"(ret), "+r"(r_di), "+r"(r_si), "+r"(r_dx), "+r"(r_10),
          "+r"(r_8), "+r"(r_9), "+r"(r_15)
        : "a"((long)SYS_INVOKE), "r"(r_14), "r"(r_13)
        : "rcx", "r11", "memory");

    uint64_t mi = (uint64_t)r_si;
    long b = r_di, c = r_dx, w0 = r_10, w1 = r_8, w2 = r_9, w3 = r_15;
    iris_msg_zero(m);
    m->recv_slot    = slot;
    m->reply        = reply;
    /* A-33: a receive that failed delivered nothing, and the kernel wrote no
     * return words.  Everything below would be the arguments read back. */
    if (ret != 0) return ret;
    m->sender_badge = (uint64_t)b;
    m->got_cap      = c;
    m->label        = iris_mi_label(mi);
    m->word_count   = iris_mi_len(mi);
    m->buf_len      = iris_mi_buf(mi);
    m->got_caps     = iris_mi_extra(mi);
    m->cap_rights   = m->got_caps;
    m->words[0] = (uint64_t)w0; m->words[1] = (uint64_t)w1;
    m->words[2] = (uint64_t)w2; m->words[3] = (uint64_t)w3;
    return ret;
}

#endif /* IRIS_COMMON_MSG_H */
