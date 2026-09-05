#ifndef IRIS_COMMON_IPC_BUFFER_H
#define IRIS_COMMON_IPC_BUFFER_H

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/endpoint_proto.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>

/* The services define their own arity wrappers; these are the two shapes this
 * header needs, spelled here so it depends on nothing but iris_syscall4. */
static inline long iris_ipcbuf_sys1(long nr, long a0) {
    return iris_syscall4(nr, a0, 0, 0, 0);
}
static inline long iris_ipcbuf_sys3(long nr, long a0, long a1, long a2) {
    return iris_syscall4(nr, a0, a1, a2, 0);
}

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
static inline void *iris_ipc_buffer_init(uint32_t frame_slot,
                                         uint32_t pt_slot,
                                         uint64_t vaddr)
{
    if (!frame_slot || !pt_slot || (vaddr & 0xFFFu)) return 0;

    /* One page, retyped from memory this service owns. */
    if (iris_syscall4(SYS_UNTYPED_RETYPE2, (long)IRIS_CPTR_OWN_UNTYPED,
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
        long r = iris_syscall4(SYS_FRAME_MAP, (long)frame_slot,
                               (long)IRIS_CPTR_OWN_VSPACE, (long)vaddr, 1);
        if (r == 0) break;
        if (r != (long)IRIS_ERR_MISSING_TABLE || level == 3) return 0;
        /* One level, retyped and installed at whichever depth is missing. */
        (void)iris_ipcbuf_sys3(SYS_CNODE_DELETE, 0, (long)pt_slot, 0);
        if (iris_syscall4(SYS_UNTYPED_RETYPE2, (long)IRIS_CPTR_OWN_UNTYPED,
                          (long)((uint64_t)IRIS_KOBJ_PAGE_TABLE | (1ULL << 32)),
                          (long)((uint64_t)pt_slot << 32), 4096) != 0)
            return 0;
        if (iris_ipcbuf_sys3(SYS_VSPACE_MAP_TABLE, (long)pt_slot,
                             (long)IRIS_CPTR_OWN_VSPACE, (long)vaddr) != 0)
            return 0;
    }

    if (iris_ipcbuf_sys3(SYS_TCB_SET_IPC_BUFFER, (long)IRIS_CPTR_OWN_TCB,
                         (long)frame_slot, (long)vaddr) != 0)
        return 0;

    return (void *)(uintptr_t)vaddr;
}

#endif /* IRIS_COMMON_IPC_BUFFER_H */
