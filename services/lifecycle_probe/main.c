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
#include <iris/fault_proto.h>
#include <iris/endpoint_proto.h>

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

/* Fase 20 fault-trigger modes (opt-in per run).  After the first recv the child
 * performs a faulting access so the parent (a supervisor that registered a fault
 * endpoint via SYS_EXCEPTION_HANDLER) observes fault delivery.  words[0] carries
 * the target VA for READ/WRITE.  The child never returns from the faulting
 * instruction unless the parent resumes it after fixing the condition.
 *   LP_CMD_FAULT_READ:  read  *(volatile*)words[0]  → #PF on an unmapped VA.
 *   LP_CMD_FAULT_WRITE: write *(volatile*)words[0]  → #PF (not-present, or
 *     write-protection if the VA is a present read-only page, e.g. own text).
 *   LP_CMD_FAULT_EXEC:  call an address on the NX stack → #PF instruction-fetch.
 * words[0] == 0 means "use my own code address" (&lp_main): services load
 * ASLR-biased ET_DYN, so the parent cannot name a child text VA — the child
 * resolves it itself.  READ of own text completes (RO is readable) and exits
 * with the marker; WRITE of own text is the write-protection fault fixture.
 * Must match iris_test. */
#define LP_CMD_FAULT_READ      0x109Cu
#define LP_CMD_FAULT_WRITE     0x109Du
#define LP_CMD_FAULT_EXEC      0x109Eu

/* Fase 28 multi-page fault-read: read the FIRST byte of each of `count` pages in
 * a caller-chosen (possibly out-of-order) sequence, faulting on each unmapped
 * page so a file-backed pager resolves them one at a time.  Proves a SINGLE
 * target drives multi-page, out-of-order, nonzero-offset resolution (no need for
 * N one-shot targets, which would exhaust the per-process notification quota).
 *   words[0] = base VA (page-aligned)
 *   words[1] = count (1..8)
 *   words[2] = visit order: nibble i (bits 4*i..4*i+3) = page index read at step i
 * The child accumulates acc ^= byte_at(base + page*4096) across all steps and
 * exits with LP_EXIT_MARKER ^ (acc & 0xFF), so the parent verifies exact content
 * independent of arrival order. */
#define LP_CMD_FAULT_READ_SEQ  0x109Fu

/* Fase 28 two-offset fault-read: read the byte at base+off0 and (if count==2) at
 * base+off1 — arbitrary byte granularity, not page-aligned — XOR-accumulating
 * the low bytes and exiting LP_EXIT_MARKER ^ acc.  Lets one target verify BOTH a
 * file byte and a zero-fill byte (e.g. the tail of a partial page, or past EOF)
 * in a single region without extra targets.  A fault occurs once per distinct
 * page touched; the parent drives that many resolutions.
 *   words[0]=base VA, words[1]=count (1..2), words[2]=off0, words[3]=off1. */
#define LP_CMD_FAULT_READ_OFFS 0x10A5u

/* Fase 22 least-authority self-report: resolve well-known CPtr slots 0..15 and
 * exit with a bitmask (bit i set = slot i resolves to a live cap).  Slot 3 (the
 * command endpoint, LP_CPTR_CMD_EP) is always present.  The parent mints a
 * KNOWN set of caps into the child and asserts the reported mask equals exactly
 * that set — proving delivery carries no phantom authority (A11) and that
 * removing a cap removes it from the child (A15).  Must match iris_test. */
#define LP_CMD_REPORT_SLOTS    0x10A0u

/* Fase 23 compromised-driver stand-in: attempt a battery of device-authority
 * escalations using only the caps the parent minted, and exit with a bitmask of
 * which ones (incorrectly) SUCCEEDED.  A contained driver exits 0 — every
 * escalation is denied by the kernel.  words[0] carries an out-of-range ioport
 * offset for the range-crossing attempt.  Attempts:
 *   bit 0: SYS_CAP_CREATE_IOPORT via slot 6 (needs HW_ACCESS bootcap) → forge port
 *   bit 1: SYS_IOPORT_IN  via slot 10 at words[0] (out-of-range offset) → cross range
 *   bit 2: SYS_IOPORT_OUT via slot 10 at words[0] (out-of-range offset) → cross range
 *   bit 3: SYS_CAP_CREATE_IRQCAP via slot 6 → forge IRQ authority
 *   bit 4: SYS_IRQ_ACK via slot 11 (may be empty) → ack an IRQ it holds no cap for
 * Must match iris_test. */
#define LP_CMD_DEV_PROBE       0x10A1u
/* Exit-code base for a send/call that returned (was NOT killed while blocked);
 * low byte = -err (0 = success).  Lets a rendezvous-then-complete run report. */
#define LP_EXIT_IPC_BASE       0x0C00

/* Fase 25 user-pager modes (opt-in per run).  The probe acts as an EXTERNAL
 * PAGER for a target process: the parent (supervisor) mints its exact
 * authority manifest into well-known slots BEFORE start — the pager never
 * acquires anything at runtime, so LP_CMD_REPORT_SLOTS doubles as the
 * manifest oracle (all pager slots are < 16).  Must match iris_test.
 *
 *   slot  8: (T184 only) victim process cap, deliberately under-privileged
 *   slot  9: (T184 only) victim VSpace cap, deliberately under-privileged
 *   slot 12: target process cap  (READ for fault info, MANAGE for resume)
 *   slot 13: target VSpace cap   (WRITE — the map-into-target authority)
 *   slot 14: page source cap    (Fase 25: a KFrame; Fase 26: a KVmo) — the
 *            page the pager may install
 *   slot 15: fault notification  (WAIT — the delivery wake-up)
 *
 * LP_CMD_PAGER_SERVE — serve fault(s) of the authorized target:
 *   words[0]: low 8 bits = per-fault subaction (1 = map raw frame at cr2 then
 *             seq-checked resume; 2 = seq-checked resume WITHOUT map — the
 *             clean-refault probe; 3 = seq-checked kill; 4 = map a VMO page
 *             (SYS_VMO_MAP_PAGE, Fase 26) at cr2 then seq-checked resume),
 *             bits 8..15 = number of faults to serve (0 → 1).
 *   words[1]: map flags (bit 0 = W, bit 1 = X) for subactions 1 and 4.
 *   words[2]: subaction 1 = map VA override (0 = cr2 page); subaction 4 =
 *             VMO byte offset (page-aligned) to source the page from.
 *   words[3]: expected cr2 (0 = don't validate) — the pager's own honesty
 *             check that the fault info matches what the target touched.
 *   Exits LP_EXIT_PGR_BASE on full success, else base | low byte of the
 *   first failure (-err, or the private validation markers below).
 *
 * LP_CMD_PAGER_XPROBE — unauthorized-resolution battery (T184): attempt to
 *   resolve a VICTIM's fault while holding only under-privileged victim caps
 *   (slots 8/9) plus full authority over an unrelated target (slots 12-15).
 *   words[0] = victim task id, words[1] = map VA, words[2] = victim fault
 *   generation.  Exits with a bitmask of attempts that (incorrectly)
 *   SUCCEEDED — a contained pager exits 0. */
#define LP_CMD_PAGER_SERVE     0x10A2u
#define LP_CMD_PAGER_XPROBE    0x10A3u
#define LP_PGR_SLOT_XPROC      8u
#define LP_PGR_SLOT_XVS        9u
#define LP_PGR_SLOT_TPROC      12u
#define LP_PGR_SLOT_TVS        13u
#define LP_PGR_SLOT_FRAME      14u
#define LP_PGR_SLOT_NOTIF      15u
#define LP_EXIT_PGR_BASE       0x0D00
#define LP_PGR_ERR_INFO        0x7Eu   /* fault info dishonest (vector/seq) */
#define LP_PGR_ERR_CR2         0x7Du   /* cr2 does not match expectation */

/* Fase 27 — persistent PAGER SERVICE mode.  On LP_CMD_PAGER_SERVICE the probe
 * stops being a one-shot child and becomes a supervised user-pager service: it
 * loops on its control endpoint (slot 3) serving fault-resolution requests for
 * a manifest of target grants and VMO grants, driven request/reply by its
 * supervisor.  Its ENTIRE authority is minted before start into these slots;
 * it acquires nothing at runtime.  Must match iris_test's PS_* constants.
 *
 *   slot 3            control endpoint (LP_CPTR_CMD_EP), served in a loop
 *   target i (0,1):   proc  LP_PS_TPROC(i)  = 8 + i*3   (READ|MANAGE)
 *                     vspace LP_PS_TVS(i)   = 9 + i*3   (WRITE)
 *                     notif  LP_PS_TNOTIF(i)= 10 + i*3  (WAIT)
 *   VMO j (0,1):      LP_PS_VMO(j) = 16 + j            (READ [+WRITE])
 *
 * Control op (words[0] bits [7:0]); words[1] = VMO offset; words[2] = expect cr2. */
#define LP_CMD_PAGER_SERVICE   0x10A4u
#define LP_PS_TPROC(i)   (8u + (i) * 3u)
#define LP_PS_TVS(i)     (9u + (i) * 3u)
#define LP_PS_TNOTIF(i)  (10u + (i) * 3u)
#define LP_PS_VMO(j)     (16u + (j))
#define LP_PS_MAX_TARGETS 2u
#define LP_PS_MAX_VMOS    2u
#define LP_PS_OP_PING       1u
#define LP_PS_OP_REPORT     2u
#define LP_PS_OP_MAP_RESUME 3u
#define LP_PS_OP_KILL       4u
#define LP_PS_OP_SHUTDOWN   5u
#define LP_PS_ERR_BADOP    0x60u
#define LP_PS_ERR_INFO     0x61u
#define LP_PS_ERR_CR2      0x62u
#define LP_PS_ERR_NOFAULT  0x63u

static inline long lp_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long lp_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long lp_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long lp_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long r10 __asm__("r10") = a3;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

/* Little-endian field reads from the SYS_PROCESS_FAULT_INFO record. */
static inline uint32_t lp_rd32(const uint8_t *b, uint32_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}
static inline uint64_t lp_rd64(const uint8_t *b, uint32_t off) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8u; i++) v |= (uint64_t)b[off + i] << (i * 8);
    return v;
}

/* ── Fase 27 pager-service helpers ──────────────────────────────────────── */

/* Resolve target[tidx]'s fault and (for MAP_RESUME) install vmo[vidx]'s page,
 * then seq-resolve.  Returns 0 or a negative error / -LP_PS_ERR_* marker. */
static long lp_ps_serve(uint32_t op, uint32_t tidx, uint32_t vidx,
                        uint32_t flags, uint64_t offset, uint64_t expect_cr2) {
    if (tidx >= LP_PS_MAX_TARGETS) return -(long)LP_PS_ERR_BADOP;
    if (op == LP_PS_OP_MAP_RESUME && vidx >= LP_PS_MAX_VMOS) return -(long)LP_PS_ERR_BADOP;

    long tproc  = (long)LP_PS_TPROC(tidx);
    long tvs    = (long)LP_PS_TVS(tidx);
    long tnotif = (long)LP_PS_TNOTIF(tidx);
    long vmo    = (long)LP_PS_VMO(vidx);

    uint64_t bits = 0;
    long r = lp_sys3(SYS_NOTIFY_WAIT_TIMEOUT, tnotif, (long)(uintptr_t)&bits, 2000000000L);
    if (r != 0) return -(long)LP_PS_ERR_NOFAULT;

    uint8_t fb[FAULT_MSG_LEN];
    r = lp_sys2(SYS_PROCESS_FAULT_INFO, tproc, (long)(uintptr_t)fb);
    if (r != 0) return r;
    uint32_t vector  = lp_rd32(fb, FAULT_OFF_VECTOR);
    uint32_t task_id = lp_rd32(fb, FAULT_OFF_TASK_ID);
    uint32_t seq     = lp_rd32(fb, FAULT_OFF_SEQ);
    uint64_t cr2     = lp_rd64(fb, FAULT_OFF_CR2);
    if (vector != 14u || seq == 0u || task_id == 0u) return -(long)LP_PS_ERR_INFO;
    if (expect_cr2 != 0 && cr2 != expect_cr2)         return -(long)LP_PS_ERR_CR2;

    if (op == LP_PS_OP_MAP_RESUME) {
        uint64_t va  = cr2 & ~0xFFFULL;
        uint64_t ofs = (offset & ~0xFFFULL) | (uint64_t)(flags & 0x3u);
        r = lp_sys4(SYS_VMO_MAP_PAGE, vmo, tvs, (long)va, (long)ofs);
        if (r != 0) return r;
    }
    uint64_t action = ((uint64_t)seq << 32) | ((op == LP_PS_OP_KILL) ? 3u : 2u);
    return lp_sys3(SYS_EXCEPTION_RESUME, tproc, (long)task_id, (long)action);
}

/* Manifest oracle: bitmask of slots 0..17 that resolve, plus high-authority
 * slots a minimal pager must never hold (bit24 spawn=6, bit26 untyped=55,
 * bit27 vspace-self=56). */
static uint32_t lp_ps_report(void) {
    uint32_t mask = 0u;
    for (uint32_t s = 0; s < 18u; s++)
        if (lp_sys1(SYS_CSPACE_RESOLVE, (long)s) >= 0) mask |= (1u << s);
    if (lp_sys1(SYS_CSPACE_RESOLVE, 6)  >= 0) mask |= (1u << 24);
    if (lp_sys1(SYS_CSPACE_RESOLVE, 55) >= 0) mask |= (1u << 26);
    if (lp_sys1(SYS_CSPACE_RESOLVE, 56) >= 0) mask |= (1u << 27);
    return mask;
}

void lp_main(handle_id_t bootstrap_ch_h);
void lp_main(handle_id_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;   /* RBX = 0 under the CPtr-mint bootstrap model */

    struct IrisMsg msg;
    uint8_t *p = (uint8_t *)&msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) p[i] = 0;

    /* Block until the parent sends/calls — or until the parent kills us. */
    (void)lp_sys2(SYS_EP_RECV, (long)LP_CPTR_CMD_EP, (long)&msg);

    /* Fase 27: persistent PAGER SERVICE mode.  Loop on the control endpoint
     * serving fault-resolution requests inside the minted manifest, replying
     * to each EP_CALL.  Exit on SHUTDOWN or when the control endpoint closes
     * (supervisor dropped the master). */
    if (msg.label == (uint64_t)LP_CMD_PAGER_SERVICE) {
        for (;;) {
            for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) p[i] = 0;
            long rr = lp_sys2(SYS_EP_RECV, (long)LP_CPTR_CMD_EP, (long)&msg);
            if (rr != 0) { lp_sys1(SYS_EXIT, 0); for (;;) {} }

            handle_id_t reply_h = (handle_id_t)msg.attached_handle;
            uint32_t op    = (uint32_t)(msg.words[0] & 0xFFu);
            uint32_t tidx  = (uint32_t)((msg.words[0] >> 8) & 0xFFu);
            uint32_t vidx  = (uint32_t)((msg.words[0] >> 16) & 0xFFu);
            uint32_t flags = (uint32_t)((msg.words[0] >> 24) & 0x3u);
            uint64_t offset = msg.words[1];
            uint64_t expect = msg.words[2];

            long result = 0;
            int shutdown = 0;
            if (op == LP_PS_OP_PING)            result = 0;
            else if (op == LP_PS_OP_REPORT)     result = (long)lp_ps_report();
            else if (op == LP_PS_OP_MAP_RESUME ||
                     op == LP_PS_OP_KILL)       result = lp_ps_serve(op, tidx, vidx, flags, offset, expect);
            else if (op == LP_PS_OP_SHUTDOWN) { result = 0; shutdown = 1; }
            else                                result = -(long)LP_PS_ERR_BADOP;

            if (reply_h != HANDLE_INVALID) {
                struct IrisMsg reply;
                for (uint32_t i = 0; i < (uint32_t)sizeof(reply); i++) ((uint8_t *)&reply)[i] = 0;
                reply.label      = IRIS_EP_REPLY_OK;
                reply.words[0]   = (uint64_t)result;
                reply.word_count = 1u;
                (void)lp_sys2(SYS_REPLY, (long)reply_h, (long)&reply);
                lp_sys1(SYS_HANDLE_CLOSE, (long)reply_h);
            }
            if (shutdown) { lp_sys1(SYS_EXIT, 0); for (;;) {} }
        }
    }

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

    /* Fase 22: report which well-known CPtr slots resolve, as an exit bitmask.
     * Bits 0..15 = slots 0..15 (covers the core service slots + spawn cap 6);
     * bit 16 = slot 25 (proc cap), bit 17 = slot 55 (untyped), bit 18 = slot 56
     * (vspace) — the high-authority slots a minimal service must NEVER hold. */
    if (msg.label == (uint64_t)LP_CMD_REPORT_SLOTS) {
        uint32_t mask = 0u;
        for (uint32_t s = 0; s < 16u; s++) {
            if (lp_sys1(SYS_CSPACE_RESOLVE, (long)s) >= 0) mask |= (1u << s);
        }
        if (lp_sys1(SYS_CSPACE_RESOLVE, 25) >= 0) mask |= (1u << 16);
        if (lp_sys1(SYS_CSPACE_RESOLVE, 55) >= 0) mask |= (1u << 17);
        if (lp_sys1(SYS_CSPACE_RESOLVE, 56) >= 0) mask |= (1u << 18);
        lp_sys1(SYS_EXIT, (long)mask);
        for (;;) {}
    }

    /* Fase 23: compromised-driver escalation battery — every attempt must be
     * denied by the kernel; the exit bitmask marks any that leaked through. */
    if (msg.label == (uint64_t)LP_CMD_DEV_PROBE) {
        uint32_t breach = 0u;
        if (lp_sys3(SYS_CAP_CREATE_IOPORT, 6, 0x2F8, 8) >= 0) breach |= (1u << 0);
        if (lp_sys2(SYS_IOPORT_IN, 10, (long)msg.words[0]) >= 0) breach |= (1u << 1);
        if (lp_sys3(SYS_IOPORT_OUT, 10, (long)msg.words[0], 0) >= 0) breach |= (1u << 2);
        if (lp_sys3(SYS_CAP_CREATE_IRQCAP, 6, 9, 0) >= 0) breach |= (1u << 3);
        if (lp_sys1(SYS_IRQ_ACK, 11) >= 0) breach |= (1u << 4);
        lp_sys1(SYS_EXIT, (long)breach);
        for (;;) {}
    }

    /* Fase 25: external user pager — serve fault(s) of the authorized target
     * using ONLY the minted manifest (slots 12-15).  Every step is an explicit
     * capability invocation; nothing here works unless the supervisor granted
     * the exact authority (see header block for the wire contract). */
    if (msg.label == (uint64_t)LP_CMD_PAGER_SERVE) {
        uint32_t sub    = (uint32_t)(msg.words[0] & 0xFFu);
        uint32_t count  = (uint32_t)((msg.words[0] >> 8) & 0xFFu);
        uint64_t mflags = msg.words[1];
        uint64_t va_ovr = msg.words[2];
        uint64_t expect = msg.words[3];
        long err = 0;
        if (count == 0u) count = 1u;
        for (uint32_t n = 0; n < count && err == 0; n++) {
            uint64_t bits = 0;
            long r = lp_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)LP_PGR_SLOT_NOTIF,
                             (long)(uintptr_t)&bits, 2000000000L);
            if (r != 0) { err = r; break; }
            uint8_t fb[FAULT_MSG_LEN];
            r = lp_sys2(SYS_PROCESS_FAULT_INFO, (long)LP_PGR_SLOT_TPROC,
                        (long)(uintptr_t)fb);
            if (r != 0) { err = r; break; }
            uint32_t vector  = lp_rd32(fb, FAULT_OFF_VECTOR);
            uint32_t task_id = lp_rd32(fb, FAULT_OFF_TASK_ID);
            uint32_t seq     = lp_rd32(fb, FAULT_OFF_SEQ);
            uint64_t cr2     = lp_rd64(fb, FAULT_OFF_CR2);
            if (vector != 14u || seq == 0u || task_id == 0u) {
                err = -(long)LP_PGR_ERR_INFO; break;
            }
            if (expect != 0 && cr2 != expect) { err = -(long)LP_PGR_ERR_CR2; break; }
            if (sub == 1u) {
                /* Fase 25: install a raw frame (slot 14) at the fault page. */
                uint64_t va = va_ovr ? va_ovr : (cr2 & ~0xFFFULL);
                r = lp_sys4(SYS_FRAME_MAP, (long)LP_PGR_SLOT_FRAME,
                            (long)LP_PGR_SLOT_TVS, (long)va, (long)mflags);
                if (r != 0) { err = r; break; }
            } else if (sub == 4u) {
                /* Fase 26: install a VMO page (slot 14 = VMO) at the fault page,
                 * sourcing byte offset words[2] within the VMO. */
                uint64_t va  = cr2 & ~0xFFFULL;
                uint64_t ofs = (va_ovr & ~0xFFFULL) | (mflags & 0x3ULL);
                r = lp_sys4(SYS_VMO_MAP_PAGE, (long)LP_PGR_SLOT_FRAME,
                            (long)LP_PGR_SLOT_TVS, (long)va, (long)ofs);
                if (r != 0) { err = r; break; }
            }
            uint64_t action = ((uint64_t)seq << 32) | ((sub == 3u) ? 3u : 2u);
            r = lp_sys3(SYS_EXCEPTION_RESUME, (long)LP_PGR_SLOT_TPROC,
                        (long)task_id, (long)action);
            if (r != 0) { err = r; break; }
        }
        lp_sys1(SYS_EXIT, (long)(LP_EXIT_PGR_BASE | ((uint32_t)-err & 0xFFu)));
        for (;;) {}
    }

    /* Fase 25: unauthorized-resolution battery — every attempt against the
     * victim (whose caps in slots 8/9 are deliberately under-privileged, or
     * whose task simply is not the pager's target) must be denied by the
     * kernel.  Exit bitmask marks any that leaked through; contained = 0. */
    if (msg.label == (uint64_t)LP_CMD_PAGER_XPROBE) {
        uint32_t breach = 0u;
        uint32_t vtid = (uint32_t)msg.words[0];
        uint64_t va   = msg.words[1];
        uint64_t vseq = msg.words[2];
        /* resume / kill the victim through a no-MANAGE victim proc cap */
        if (lp_sys3(SYS_EXCEPTION_RESUME, (long)LP_PGR_SLOT_XPROC, (long)vtid,
                    (long)((vseq << 32) | 2u)) >= 0) breach |= (1u << 0);
        if (lp_sys3(SYS_EXCEPTION_RESUME, (long)LP_PGR_SLOT_XPROC, (long)vtid,
                    (long)((vseq << 32) | 3u)) >= 0) breach |= (1u << 1);
        /* map / unmap in the victim VSpace through a no-WRITE vspace cap */
        if (lp_sys4(SYS_FRAME_MAP, (long)LP_PGR_SLOT_FRAME, (long)LP_PGR_SLOT_XVS,
                    (long)va, 0) >= 0) breach |= (1u << 2);
        if (lp_sys3(SYS_FRAME_UNMAP, (long)LP_PGR_SLOT_FRAME, (long)LP_PGR_SLOT_XVS,
                    (long)va) >= 0) breach |= (1u << 3);
        /* resolve the victim's task through the pager's OWN (full) target cap:
         * the task does not belong to that process — exact-match must refuse */
        if (lp_sys3(SYS_EXCEPTION_RESUME, (long)LP_PGR_SLOT_TPROC, (long)vtid,
                    (long)((vseq << 32) | 2u)) >= 0) breach |= (1u << 4);
        /* the victim's fault must not appear through the unrelated target cap */
        {
            uint8_t fb[FAULT_MSG_LEN];
            if (lp_sys2(SYS_PROCESS_FAULT_INFO, (long)LP_PGR_SLOT_TPROC,
                        (long)(uintptr_t)fb) == 0) breach |= (1u << 5);
        }
        /* device/spawn forgery — a pager holds neither */
        if (lp_sys3(SYS_CAP_CREATE_IOPORT, 6, 0x2F8, 8) >= 0) breach |= (1u << 6);
        if (lp_sys3(SYS_CAP_CREATE_IRQCAP, 6, 9, 0) >= 0) breach |= (1u << 7);
        lp_sys1(SYS_EXIT, (long)breach);
        for (;;) {}
    }

    /* Fase 20: perform a faulting access so a supervisor's fault endpoint fires.
     * The instruction faults; the kernel suspends this task in BLOCKED_FAULT and
     * signals the parent's handler.  If the parent resumes without fixing the
     * condition the same fault recurs; if it fixes it (e.g. remaps writable) the
     * access completes and we exit with the marker; normally the parent kills us. */
    if (msg.label == (uint64_t)LP_CMD_FAULT_READ) {
        uint64_t va = msg.words[0] ? msg.words[0] : (uint64_t)(uintptr_t)&lp_main;
        volatile uint32_t *p2 = (volatile uint32_t *)(uintptr_t)va;
        uint32_t v = *p2;                      /* faults on an unmapped VA */
        lp_sys1(SYS_EXIT, (long)(LP_EXIT_MARKER ^ (v & 0xFFu)));
        for (;;) {}
    }
    if (msg.label == (uint64_t)LP_CMD_FAULT_READ_SEQ) {
        uint64_t base  = msg.words[0];
        uint32_t count = (uint32_t)(msg.words[1] & 0xFu);
        uint64_t order = msg.words[2];
        if (count == 0u || count > 8u) count = 1u;
        uint32_t acc = 0u;
        for (uint32_t step = 0; step < count; step++) {
            uint32_t page = (uint32_t)((order >> (4u * step)) & 0xFu);
            volatile uint32_t *p2 =
                (volatile uint32_t *)(uintptr_t)(base + (uint64_t)page * 0x1000ULL);
            uint32_t v = *p2;                  /* #PF here on each unmapped page */
            acc ^= (v & 0xFFu);
        }
        lp_sys1(SYS_EXIT, (long)(LP_EXIT_MARKER ^ acc));
        for (;;) {}
    }
    if (msg.label == (uint64_t)LP_CMD_FAULT_READ_OFFS) {
        uint64_t base  = msg.words[0];
        uint32_t count = (uint32_t)msg.words[1];
        if (count == 0u || count > 2u) count = 1u;
        uint32_t acc = 0u;
        for (uint32_t k = 0; k < count; k++) {
            uint64_t off = (k == 0u) ? msg.words[2] : msg.words[3];
            volatile uint32_t *p2 = (volatile uint32_t *)(uintptr_t)(base + off);
            uint32_t v = *p2;                  /* #PF once per distinct page */
            acc ^= (v & 0xFFu);
        }
        lp_sys1(SYS_EXIT, (long)(LP_EXIT_MARKER ^ acc));
        for (;;) {}
    }
    if (msg.label == (uint64_t)LP_CMD_FAULT_WRITE) {
        uint64_t va = msg.words[0] ? msg.words[0] : (uint64_t)(uintptr_t)&lp_main;
        volatile uint32_t *p2 = (volatile uint32_t *)(uintptr_t)va;
        *p2 = 0xFA017E57u;                     /* faults on unmapped / read-only VA */
        lp_sys1(SYS_EXIT, (long)LP_EXIT_MARKER);
        for (;;) {}
    }
    if (msg.label == (uint64_t)LP_CMD_FAULT_EXEC) {
        volatile uint8_t code_on_stack[16];
        for (uint32_t i = 0; i < 16u; i++) code_on_stack[i] = 0xC3u;  /* ret */
        void (*fn)(void) = (void (*)(void))(uintptr_t)code_on_stack;   /* NX page */
        fn();                                  /* faults on instruction fetch (NX) */
        lp_sys1(SYS_EXIT, (long)LP_EXIT_MARKER);
        for (;;) {}
    }

    /* Reached only when the recv returned: exit with the marker.  Any reply cap
     * delivered by an EP_CALL is dropped unanswered here on purpose. */
    lp_sys1(SYS_EXIT, (long)LP_EXIT_MARKER);
    for (;;) {}
}
