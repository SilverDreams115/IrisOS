#ifndef IRIS_COMMON_MAP_H
#define IRIS_COMMON_MAP_H

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/error.h>

/*
 * iris_map_frame — map a frame, supplying the paging levels the window needs.
 *
 * Since Stage 6-pure the kernel creates no page tables: a map whose walk is
 * incomplete answers MISSING_TABLE and the holder retypes a KOBJ_PAGE_TABLE
 * and installs it (seL4's `seL4_X86_PageTable_Map`).  For a single page that
 * is a short loop over the levels; for a LARGE frame it is not, and the reason
 * is worth stating because it is not obvious.
 *
 * A map covers the whole frame (ledger D-10), so the walk that fails may be
 * for any page in the range, not the first — and the map does not say which,
 * because a partial map unwinds and reports one error.  Retrying with a table
 * installed at the BASE address would therefore loop forever on a range whose
 * second 2 MiB is the one missing a level.  So each retry walks the range in
 * 2 MiB steps and installs a level at the first boundary that accepts one; a
 * boundary that already has its levels refuses, which is the answer that moves
 * the search along.
 *
 * `pt_slot` is scratch and is reused for every level; the installed table
 * belongs to the address space, not to the slot.  Everything comes out of
 * `untyped_c`, so page and paging are charged to the same memory and revoking
 * it reclaims both.
 *
 * Returns 0, or the error the map last gave.
 */
static inline long iris_map_frame(uint64_t frame_c, uint64_t vspace_c,
                                  uint64_t untyped_c, uint32_t pt_slot,
                                  uint64_t vaddr, uint64_t size,
                                  uint64_t flags)
{
    /* Bounded by the levels a range can need: three per 2 MiB step, plus the
     * shared upper levels.  A loop that cannot terminate is worse than a map
     * that fails. */
    for (int attempt = 0; attempt < 96; attempt++) {
        long r = iris_syscall4((long)SYS_FRAME_MAP, (long)frame_c,
                               (long)vspace_c, (long)vaddr, (long)flags);
        if (r == 0) return 0;
        if (r != (long)IRIS_ERR_MISSING_TABLE) return r;

        int installed = 0;
        for (uint64_t off = 0; off < (size ? size : 4096u);
             off += 0x200000ULL) {
            (void)iris_syscall4((long)SYS_CNODE_DELETE, 0,
                                (long)pt_slot, 0, 0);
            if (iris_syscall4((long)SYS_UNTYPED_RETYPE2, (long)untyped_c,
                              (long)((uint64_t)IRIS_KOBJ_PAGE_TABLE |
                                     (1ULL << 32)),
                              (long)((uint64_t)pt_slot << 32), 4096) != 0)
                return r;
            if (iris_syscall4((long)SYS_VSPACE_MAP_TABLE, (long)pt_slot,
                              (long)vspace_c, (long)(vaddr + off), 0) == 0) {
                installed = 1;
                break;
            }
        }
        if (!installed) return r;
    }
    return (long)IRIS_ERR_MISSING_TABLE;
}

#endif /* IRIS_COMMON_MAP_H */
