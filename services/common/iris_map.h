/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_COMMON_MAP_H
#define IRIS_COMMON_MAP_H

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/error.h>

/*
 * iris_map_frame — map a frame, supplying the paging levels the window needs.
 *
 * The kernel creates no page tables: a map whose walk is
 * incomplete answers MISSING_TABLE and the holder retypes a KOBJ_PAGE_TABLE
 * and installs it (seL4's `seL4_X86_PageTable_Map`).  For a single page that
 * is a short loop over the levels; for a LARGE frame it is not, and the reason
 * is worth stating because it is not obvious.
 *
 * A map covers the whole frame, so the walk that fails may be
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
        long r = iris_invoke((long)frame_c, INV_FRAME_MAP,
                             (long)vspace_c, (long)vaddr, (long)flags);
        if (r == 0) return 0;
        if (r != (long)IRIS_ERR_MISSING_TABLE) return r;

        /*
         * Why this reports the RETYPE's error and not `r`.
         *
         * It used to `return r` — `IRIS_ERR_MISSING_TABLE` — whenever the
         * page table could not be carved, which says "a level of the walk is
         * missing" to a caller whose actual problem was that the budget was
         * spent or the Untyped was busy.  Three different failures arrived as
         * one number, and the one it chose was the least informative of them:
         * the caller then believed the walk was incomplete and had no reason
         * to look at its own memory.
         *
         * A spawn that died this way reported `PROC_STEP_INTERP` with
         * err -16, which reads as "the interpreter could not be found" and is
         * not what happened at all.
         */
        int installed = 0;
        long why = r;
        for (uint64_t off = 0; off < (size ? size : 4096u);
             off += 0x200000ULL) {
            long rt, mp, del;
            /*
             * The slot is freed before it is reused, and the answer is READ.
             *
             * This discarded its result, which is the same mistake the
             * paragraph above is about, one line lower: if the delete fails
             * the slot stays occupied, the retype below then answers
             * ALREADY_EXISTS, and the caller is told its page-table
             * allocation failed for a reason that is not the reason.
             * Deleting an empty slot is OK by contract, so a non-zero here is
             * a real refusal and belongs to the caller.
             */
            del = iris_invoke1(0, INV_CNODE_DELETE, (long)pt_slot);
            if (del != 0) return del;
            rt = iris_invoke((long)untyped_c, INV_UNTYPED_RETYPE,
                             (long)((uint64_t)IRIS_KOBJ_PAGE_TABLE |
                                    (1ULL << 32)),
                             (long)((uint64_t)pt_slot << 32), 4096);
            if (rt != 0) return rt;         /* the budget, not the walk */
            mp = iris_invoke2((long)pt_slot, INV_PAGE_TABLE_MAP,
                              (long)vspace_c, (long)(vaddr + off));
            if (mp == 0) { installed = 1; break; }
            why = mp;                       /* the last real refusal */
        }
        if (!installed) return why;
    }
    return (long)IRIS_ERR_MISSING_TABLE;
}

#endif /* IRIS_COMMON_MAP_H */
