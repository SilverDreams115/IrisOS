/*
 * it_t356.c — how fast is it, and a number to regress against (Stage 10).
 *
 * ── Why this exists at all ─────────────────────────────────────────────────
 *
 * Stage 10 lists "performance" among the things a general-purpose platform
 * needs.  There was no benchmark in this tree and therefore no number: nobody
 * could say whether a change made the system slower, only whether it still
 * worked.  A microkernel's cost is dominated by the operations everything else
 * is built out of, so those are what this measures.
 *
 * ── What it measures, and why these three ──────────────────────────────────
 *
 *   an INVOCATION that does nothing but resolve a capability and refuse
 *     — the floor of every operation in the system, and the thing the ABI
 *       freeze made the only door
 *   an IPC ROUND TRIP through an endpoint to a server and back
 *     — what every service call costs, and the number seL4 is judged on
 *   a DISK READ of one sector through the block service
 *     — a whole subsystem: two IPC round trips, a DMA transfer, a controller
 *
 * ── What the ceilings are, and what they are not ───────────────────────────
 *
 * They are ORDER-OF-MAGNITUDE guards, deliberately generous.  This runs under
 * TCG on a machine whose speed nobody controls, sometimes with four emulated
 * processors competing for one host thread, so a tight bound would fail for
 * reasons that have nothing to do with IRIS.  What a generous bound still
 * catches is the thing worth catching: an operation that became ten times more
 * expensive because somebody added a lock, a copy, or a walk to it.
 *
 * The NUMBERS are printed whether or not they pass, because the number is the
 * point.  A ceiling tells you nothing got catastrophically worse; the log tells
 * you what it actually costs, and that is what somebody optimising reads.
 */

#include "it_priv.h"
#include <iris/blk_ep_proto.h>
#include <iris/pci_ep_proto.h>

#define T356_INVOKE_ITERS  2000u
#define T356_IPC_ITERS     500u
#define T356_DISK_ITERS    20u

/* Order-of-magnitude ceilings, in nanoseconds per operation.  See above. */
#define T356_INVOKE_MAX_NS  200000u
#define T356_IPC_MAX_NS     500000u
#define T356_DISK_MAX_NS  50000000u

static uint64_t t356_now(void) {
    uint64_t ns = 0;
    if (it_timer_uptime(&ns) != 0) return 0;
    return ns;
}

static void t356_report(const char *what, uint64_t total_ns, uint32_t iters) {
    it_serial_write("[IRIS][TEST] T356 ");
    it_serial_write(what);
    it_serial_write(" ");
    it_log_num((uint32_t)(iters ? (total_ns / iters) : 0));
    it_serial_write(" ns/op over ");
    it_log_num(iters);
    it_serial_write("\n");
}

void test_t356(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "performance";

    /*
     * The clock is the timer SERVICE's, because A-24 made time a service and
     * there is no syscall that answers it for an ordinary task.  A machine
     * whose timer did not start cannot be measured, and saying so is better
     * than reporting zeroes.
     */
    if (t356_now() == 0u) {
        it_serial_write("[IRIS][TEST] T356 no clock; not measured\n");
        it_fail("T356", "no clock to measure with");
        return;
    }

    /* ── 1. an invocation that resolves a capability and refuses ─────────
     *
     * `INV_CAP_IDENTIFY` on the null capability: it enters the dispatcher,
     * selects a method, fails to resolve, and returns.  That is the floor —
     * no object is touched, nothing is allocated, and every other operation in
     * the system pays at least this much. */
    {
        uint64_t t0 = t356_now();
        for (uint32_t i = 0; i < T356_INVOKE_ITERS; i++)
            (void)it_invoke0(0, INV_CAP_IDENTIFY);
        uint64_t dt = t356_now() - t0;
        t356_report("invoke", dt, T356_INVOKE_ITERS);
        if (dt / T356_INVOKE_ITERS > T356_INVOKE_MAX_NS) {
            ok = 0; why = "an invocation costs an order of magnitude too much";
        }
    }

    /* ── 2. an IPC round trip to a real server ───────────────────────────
     *
     * `PCI_OP_COUNT` on the bus service: a send, a context switch, a server
     * that answers from a variable, a reply, and a context switch back.  A
     * real server rather than a loopback, because a loopback measures the
     * endpoint and this measures what a service call costs. */
    if (ok) {
        struct iris_msg m;
        uint64_t t0 = t356_now();
        for (uint32_t i = 0; i < T356_IPC_ITERS; i++) {
            iris_msg_zero(&m);
            m.label = PCI_OP_COUNT;
            m.word_count = 0u;
            if (iris_msg_call((long)IRIS_CPTR_PCI_EP, &m) != 0) {
                ok = 0; why = "the bus service stopped answering"; break;
            }
        }
        if (ok) {
            uint64_t dt = t356_now() - t0;
            t356_report("ipc", dt, T356_IPC_ITERS);
            if (dt / T356_IPC_ITERS > T356_IPC_MAX_NS) {
                ok = 0; why = "an IPC round trip costs an order of magnitude too much";
            }
        }
    }

    /* ── 3. a whole subsystem: one sector off a real disk ────────────────*/
    if (ok) {
        struct iris_msg m;
        uint64_t t0 = t356_now();
        uint32_t done = 0;
        for (uint32_t i = 0; i < T356_DISK_ITERS; i++) {
            iris_msg_zero(&m);
            m.label = BLK_OP_READ;
            m.words[0] = 0; m.words[1] = 1; m.words[2] = 0;
            m.word_count = 3u;
            /* No receive slot: the frame is not wanted, only the transfer.
             * A read that hands back no capability still did the DMA. */
            if (iris_msg_call((long)IRIS_CPTR_BLK_EP_TEST, &m) != 0 ||
                m.label != BLK_REP_OK) break;
            done++;
        }
        if (done == 0u) { ok = 0; why = "the disk service stopped answering"; }
        else {
            uint64_t dt = t356_now() - t0;
            t356_report("disk-read-512", dt, done);
            if (dt / done > T356_DISK_MAX_NS) {
                ok = 0; why = "a disk read costs an order of magnitude too much";
            }
        }
    }

    it_quiesce_reaper();
    if (ok) it_pass("T356"); else it_fail("T356", why);
}
