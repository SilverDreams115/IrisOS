#ifndef IRIS_COMMON_IPC_BUFFER_H
#define IRIS_COMMON_IPC_BUFFER_H

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/endpoint_proto.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>

/* Ledger A-31: this header used to carry two arity wrappers of its own, so it
 * could name syscall numbers without depending on a service's helpers.  It
 * names METHODS now, and `iris_invoke*` comes with the ABI. */

/*
 * The well-known address for a thread's IPC buffer.
 *
 * A fixed address rather than a chosen one because every service that has one
 * has exactly one, and a service that has to remember where it put it has a
 * variable where a constant would do.  It sits below USER_VMO_BASE
 * (BASE + 0x50000000) and far above where a PIE image and its heap land, so it
 * collides with nothing a service maps for its own reasons.
 */
#define IRIS_IPC_BUFFER_VA   (0x0000008000000000ULL + 0x40000000ULL)

/*
 * iris_ipc_buffer_init — give THIS thread an IPC buffer of its own.
 *
 * Ledger D-4.  A message's bulk payload used to be staged in 256 bytes living
 * inside the thread's TCB: a size the service did not choose, memory it did
 * not pay for, and a buffer it could not name with a capability.  Since
 * `SYS_TCB_SET_IPC_BUFFER` a thread registers a FRAME instead, and the kernel
 * transfers payloads between the two ends' frames through its own window —
 * one copy, and no user pointer for anything to invalidate between the check
 * and the copy.
 *
 * A service can only do this because it OWNS memory: `IRIS_CPTR_OWN_UNTYPED`
 * is the sub-untyped its address space was already charged to, now named in
 * its own CSpace.  A service holding no Untyped can create nothing at all,
 * which is the shape the memory server exists to remove.
 *
 * The address space and the thread are named through `IRIS_CPTR_OWN_VSPACE`
 * and `IRIS_CPTR_OWN_TCB` — the delegations the spawner minted — rather than
 * fabricated with SYS_VSPACE_SELF / SYS_TCB_SELF, which publish MDB
 * LEGACY_ROOTS: capabilities with no parent, which no revoke can reach.  A
 * service should not have to create an unparented capability to use its own
 * address space.
 *
 * The two remaining slots are the caller's scratch, from the per-service range
 * (22..29 are unassigned).  Only the FRAME capability has to keep existing,
 * and it does — the kernel holds its own reference from the moment it is
 * registered.  The page-table slot is reused across levels, which is why it is
 * deleted before each retype.
 *
 * Returns the buffer's address, or NULL.  A NULL is not fatal by itself: the
 * kernel staging path still works and is what a thread with no registered
 * buffer gets, so a caller may simply keep using its static buffer.  That
 * fallback is the whole reason D-4 is MIGRATING rather than CLOSED.
 */
static inline void *iris_ipc_buffer_init_from(uint64_t untyped_c,
                                              uint64_t vspace_c,
                                              uint64_t tcb_c,
                                              uint32_t frame_slot,
                                              uint32_t pt_slot,
                                              uint64_t vaddr)
{
    if (!frame_slot || !pt_slot || (vaddr & 0xFFFu)) return 0;
    if (!untyped_c || !vspace_c || !tcb_c) return 0;

    /* One page, retyped from memory this service owns. */
    if (iris_invoke((long)untyped_c, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)frame_slot << 32), 4096) != 0)
        return 0;

    /*
     * Map it — installing the paging levels the window needs, because nobody
     * else will.
     *
     * Since Stage 6-pure the kernel creates no page tables: a map whose walk is
     * incomplete answers MISSING_TABLE, and the holder retypes a level and
     * installs it (seL4's seL4_X86_PageTable_Map).  The IPC buffer lands in a
     * window a service has never touched, so on a fresh address space all
     * three levels under the PML4 are missing — hence a loop rather than a
     * single attempt, bounded by the number of levels there can be.
     *
     * The levels come out of the same Untyped as the frame, so the whole
     * buffer — page and paging — is charged to memory the service owns, and
     * revoking that Untyped reclaims all of it.
     */
    for (int level = 0; level < 4; level++) {
        long r = iris_invoke((long)frame_slot, INV_FRAME_MAP,
                             (long)vspace_c, (long)vaddr, 1);
        if (r == 0) break;
        if (r != (long)IRIS_ERR_MISSING_TABLE || level == 3) return 0;
        /* One level, retyped and installed at whichever depth is missing. */
        (void)iris_invoke1(0, INV_CNODE_DELETE, (long)pt_slot);
        if (iris_invoke((long)untyped_c, INV_UNTYPED_RETYPE,
                        (long)((uint64_t)IRIS_KOBJ_PAGE_TABLE | (1ULL << 32)),
                        (long)((uint64_t)pt_slot << 32), 4096) != 0)
            return 0;
        if (iris_invoke2((long)pt_slot, INV_PAGE_TABLE_MAP,
                         (long)vspace_c, (long)vaddr) != 0)
            return 0;
    }

    if (iris_invoke2((long)tcb_c, INV_TCB_SET_IPC_BUFFER,
                     (long)frame_slot, (long)vaddr) != 0)
        return 0;

    return (void *)(uintptr_t)vaddr;
}

/*
 * The common case: a service spawned by the loader, which was given a budget
 * and therefore also its own address space and thread (ledger D-6).
 *
 * The general form above exists because not every holder is one of those — the
 * test suite owns a named Untyped of its own and no slot-12 budget, and
 * hardcoding the well-known CPtrs made its registration fail silently, which
 * left it on a staging path the kernel was about to stop having.
 */
static inline void *iris_ipc_buffer_init(uint32_t frame_slot,
                                         uint32_t pt_slot,
                                         uint64_t vaddr)
{
    return iris_ipc_buffer_init_from(IRIS_CPTR_OWN_UNTYPED,
                                     IRIS_CPTR_OWN_VSPACE,
                                     IRIS_CPTR_OWN_TCB,
                                     frame_slot, pt_slot, vaddr);
}

#endif /* IRIS_COMMON_IPC_BUFFER_H */
