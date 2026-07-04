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

    /* Reached only when the recv returned: exit with the marker.  Any reply cap
     * delivered by an EP_CALL is dropped unanswered here on purpose. */
    lp_sys1(SYS_EXIT, (long)LP_EXIT_MARKER);
    for (;;) {}
}
