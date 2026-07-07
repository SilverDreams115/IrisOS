/*
 * lifecycle_probe/main.c — minimal ring-3 test child for the spawn/kill
 * lifecycle harness.
 *
 * NOT a productive service.  It is embedded in the initrd only so the ring-3
 * iris_test suite can spawn it (via svc_load, using iris_test's own spawn cap)
 * to exercise cross-process lifecycle paths that cannot be observed from a
 * single process: teardown of a killed child, cleanup of caps/mappings, and
 * reply-cap / endpoint cleanup on server death.
 *
 * Behaviour (deliberately tiny and deterministic):
 *   1. Block on SYS_EP_RECV of the command endpoint the parent minted into the
 *      well-known CPtr slot LP_CPTR_CMD_EP.
 *   2. If the parent sends/calls, the recv returns and the child SYS_EXITs with
 *      LP_EXIT_MARKER so the parent can confirm the child ran to completion.
 *      For an EP_CALL the parent's reply cap is intentionally NOT replied to —
 *      dropping it on exit is what exercises the server-death cleanup path.
 *   3. If the parent kills the child while it is blocked in the recv, the child
 *      never reaches the exit — the kernel's process teardown cancels the
 *      blocked wait, which is exactly what the harness verifies.
 *
 * A1.9 receive-slot mode (opt-in per run; every other label keeps the exact
 * legacy behaviour above): if the first message is LP_CMD_RSLOT_RECV, the
 * child performs a SECOND recv on the command endpoint declaring the
 * receive-slot the parent chose in words[0] (0 = legacy handle delivery),
 * then invokes the delivered cap by whatever it received — NOTIFY_SIGNAL
 * with bits 1 for a CSpace CPtr landing, bits 2 for a handle landing — and
 * exits with the raw attached_handle discriminator so the parent can assert
 * where the cap landed.  A failed declared recv (occupied / invalid slot)
 * exits with LP_EXIT_RECV_ERR_BASE | -err instead.
 *
 * The child holds no authority beyond the single command endpoint (and, for the
 * mapping-teardown test, whatever the parent maps into its address space).  It
 * has no spawn cap, no device caps, and never touches global state.
 */
#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/ipc_msg.h>

/* Well-known CPtr slot in the child's root CNode where the parent mints the
 * command endpoint (must match the value used by iris_test). */
#define LP_CPTR_CMD_EP   3u
/* Exit marker the parent checks — "arbitrary but recognisable". */
#define LP_EXIT_MARKER   0x1E57
/* A1.9: first-message label selecting receive-slot mode (words[0] = slot to
 * declare on the second recv; 0 = legacy).  Must match iris_test. */
#define LP_CMD_RSLOT_RECV      0x1099u
/* A1.9: exit code base when the declared second recv fails; low byte = -err
 * (e.g. occupied slot → 0x0B07 = base | -IRIS_ERR_ALREADY_EXISTS). */
#define LP_EXIT_RECV_ERR_BASE  0x0B00

/* Fase 16 lifecycle-hardening modes (opt-in per run; every other label keeps
 * the legacy behaviour).  After the first recv the child immediately does a
 * BLOCKING send / call back on the SAME command endpoint, becoming a queued
 * sender / caller so the parent can either rendezvous with it or kill it
 * mid-block.  Must match iris_test.
 *   LP_CMD_SEND_BLOCK: EP_SEND(cmd_ep) — blocks as a sender until a receiver
 *     arrives or the child is killed/endpoint closed.
 *   LP_CMD_CALL_BLOCK: EP_CALL(cmd_ep) — blocks as a caller (owns a reply
 *     cap once a server receives) until reply, kill, or close. */
#define LP_CMD_SEND_BLOCK      0x109Au
#define LP_CMD_CALL_BLOCK      0x109Bu
/* Exit-code base for a send/call that returned (was NOT killed while blocked);
 * low byte = -err (0 = success).  Lets a rendezvous-then-complete run report. */
#define LP_EXIT_IPC_BASE       0x0C00

static inline long lp_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long lp_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

void lp_main(handle_id_t bootstrap_ch_h);
void lp_main(handle_id_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;   /* RBX = 0 under the CPtr-mint bootstrap model */

    struct IrisMsg msg;
    uint8_t *p = (uint8_t *)&msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) p[i] = 0;

    /* Block until the parent sends/calls — or until the parent kills us. */
    (void)lp_sys2(SYS_EP_RECV, (long)LP_CPTR_CMD_EP, (long)&msg);

    /* A1.9 receive-slot mode: a second recv with the parent-chosen slot
     * declared, then invoke + report the delivered cap (see header). */
    if (msg.label == (uint64_t)LP_CMD_RSLOT_RECV) {
        uint32_t slot = (uint32_t)msg.words[0];
        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) p[i] = 0;
        msg.attached_cap = slot;               /* receive-slot declaration */
        long rr = lp_sys2(SYS_EP_RECV, (long)LP_CPTR_CMD_EP, (long)&msg);
        if (rr != 0)
            lp_sys1(SYS_EXIT, (long)(LP_EXIT_RECV_ERR_BASE | (uint32_t)-rr));
        uint32_t got = msg.attached_handle;    /* 0 / CPtr / handle */
        if (got != 0u)
            (void)lp_sys2(SYS_NOTIFY_SIGNAL, (long)got,
                          (got < 1024u) ? 1L : 2L);
        lp_sys1(SYS_EXIT, (long)got);
        for (;;) {}
    }

    /* Fase 16: block as a sender/caller on the command endpoint.  The child
     * is normally killed while blocked (the parent observes the cleanup); if
     * the parent instead rendezvouses and replies, the syscall returns and we
     * exit LP_EXIT_IPC_BASE | -err so a completing run is still observable. */
    if (msg.label == (uint64_t)LP_CMD_SEND_BLOCK ||
        msg.label == (uint64_t)LP_CMD_CALL_BLOCK) {
        int is_call = (msg.label == (uint64_t)LP_CMD_CALL_BLOCK);
        struct IrisMsg w;
        for (uint32_t i = 0; i < (uint32_t)sizeof(w); i++) ((uint8_t *)&w)[i] = 0;
        w.label = is_call ? 0x5CULL : 0x5BULL;
        long r = lp_sys2(is_call ? SYS_EP_CALL : SYS_EP_SEND,
                         (long)LP_CPTR_CMD_EP, (long)&w);
        lp_sys1(SYS_EXIT, (long)(LP_EXIT_IPC_BASE | ((uint32_t)-r & 0xFFu)));
        for (;;) {}
    }

    /* Reached only when the recv returned: exit with the marker.  Any reply cap
     * delivered by an EP_CALL is dropped unanswered here on purpose. */
    lp_sys1(SYS_EXIT, (long)LP_EXIT_MARKER);
    for (;;) {}
}
