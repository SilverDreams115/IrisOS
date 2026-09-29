/* SPDX-License-Identifier: Apache-2.0 */
/*
 * prog_mem.c — the invocations behind `brk`, `mmap` and `mprotect`.
 *
 * Everything here is two calls on objects the program already holds: retype a
 * frame out of `IRIS_CPTR_OWN_UNTYPED`, map it into `IRIS_CPTR_OWN_VSPACE`.
 * There is no memory server in the path, no kernel policy to consult, and
 * nothing to ask permission of — the permission is the budget.
 *
 * Which means the failure mode is the good one.  A budget that is spent makes
 * the retype answer `IRIS_ERR_NO_MEMORY`, and that reaches the program as a
 * return value, on the calling thread, with everything else still running.  No
 * signal, no arbitration between processes, and nothing else on the machine
 * even notices — because there was never a shared pool for this program to
 * take from in the first place.
 *
 * The arithmetic is in `prog_mem_plan.c` and is unit-tested; this file is the
 * part that can only be tested by running it.
 */
#include "prog_mem.h"
#include "iris_map.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/error.h>

/*
 * One frame, retyped and mapped.  Returns 1, or 0 with nothing left behind.
 *
 * "Nothing left behind" is the part worth being careful about: a retype that
 * succeeds and a map that fails would leave a frame charged to the budget and
 * reachable from a slot the caller is about to forget, which is a leak that
 * only a `Untyped_Reset` would ever find.  So the slot is deleted on the way
 * out of every failure below.
 */
static int pm_add(struct prog_mem *m, uint64_t vaddr, uint64_t bytes,
                  uint32_t prot, int heap) {
    uint32_t slot;

    if (!prog_mem_take_slot(m, &slot)) return 0;
    if (iris_invoke((long)m->untyped_c, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)slot << 32), (long)bytes) != 0)
        return 0;                       /* the budget said no: nothing exists */

    if (iris_map_frame(slot, m->vspace_c, m->untyped_c, m->pt_slot,
                       vaddr, bytes, (uint64_t)prot) != 0) {
        (void)iris_invoke1(0, INV_CNODE_DELETE, (long)slot);
        return 0;
    }
    if (!prog_mem_record(m, vaddr, bytes, slot, prot, heap)) {
        (void)iris_invoke2((long)slot, INV_FRAME_UNMAP,
                           (long)m->vspace_c, (long)vaddr);
        (void)iris_invoke1(0, INV_CNODE_DELETE, (long)slot);
        return 0;
    }
    return 1;
}

/* ...and its inverse, by record index. */
static void pm_drop(struct prog_mem *m, int idx) {
    struct prog_map e = m->maps[idx];
    (void)iris_invoke2((long)e.slot, INV_FRAME_UNMAP,
                       (long)m->vspace_c, (long)e.vaddr);
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)e.slot);
    prog_mem_forget(m, idx);
}

uint64_t prog_brk(struct prog_mem *m, int64_t delta) {
    uint64_t old;

    if (!m || m->brk_base == 0u) return (uint64_t)-1;
    old = m->brk_cur;

    if (delta > 0) {
        uint64_t want = old + (uint64_t)delta;
        if (want < old || want > m->brk_end) return (uint64_t)-1;

        /* Map ahead of the break until the frames cover it.  More than one
         * pass only when a single chunk could not be carved; the loop is
         * bounded by the region because every pass advances `heap_top`. */
        while (m->heap_top < want) {
            uint64_t need  = want - m->heap_top;
            uint64_t chunk = prog_mem_chunk(m, need);
            uint64_t va;
            if (chunk == 0u) return (uint64_t)-1;
            if (!prog_mem_reserve(m, chunk, /*heap=*/1, &va))
                return (uint64_t)-1;
            if (!pm_add(m, va, chunk, PROG_PROT_RW, /*heap=*/1)) {
                /* The reservation advanced `heap_top` and the memory did not
                 * arrive: put it back, so the next attempt asks for the same
                 * address rather than skipping a hole nothing owns. */
                m->heap_top = va;
                return (uint64_t)-1;
            }
        }
        m->brk_cur = want;
        return old;
    }

    if (delta < 0) {
        uint64_t want;
        uint64_t drop = (uint64_t)(-delta);
        if (drop > old - m->brk_base) return (uint64_t)-1;
        want = old - drop;

        /*
         * Release only the frames that end up ENTIRELY above the new break.
         *
         * A break is allowed to leave the rest mapped, and every real one does
         * — the memory below a partially-used frame is still the program's, and
         * freeing half a frame is not something a frame can do.  The scan
         * restarts because `prog_mem_forget` moves the last entry into the
         * hole, so the index it just looked at may now hold a different
         * record.
         */
        for (;;) {
            uint32_t i;
            int hit = -1;
            for (i = 0; i < m->nmaps; i++) {
                if (!m->maps[i].heap) continue;
                if (m->maps[i].vaddr < want) continue;
                hit = (int)i;
                break;
            }
            if (hit < 0) break;
            if (m->maps[hit].vaddr < m->heap_top)
                m->heap_top = m->maps[hit].vaddr;
            pm_drop(m, hit);
        }
        m->brk_cur = want;
        return old;
    }

    return old;
}

uint64_t prog_mmap(struct prog_mem *m, uint64_t bytes, uint32_t prot) {
    uint64_t n = prog_mem_pages(bytes), va;
    if (!m || n == 0u) return 0;
    if (!prog_mem_reserve(m, n, /*heap=*/0, &va)) return 0;
    if (!pm_add(m, va, n, prot, /*heap=*/0)) {
        /* Hand the addresses back, exactly as the break path does. */
        m->mmap_cur = va;
        return 0;
    }
    return va;
}

int prog_munmap(struct prog_mem *m, uint64_t vaddr) {
    int idx;
    if (!m) return (int)IRIS_ERR_INVALID_ARG;
    idx = prog_mem_find(m, vaddr, 1u);
    if (idx < 0 || m->maps[idx].vaddr != vaddr)
        return (int)IRIS_ERR_NOT_FOUND;
    if (m->maps[idx].heap) return (int)IRIS_ERR_INVALID_ARG;  /* that is brk's */
    pm_drop(m, idx);
    return 0;
}

int prog_mprotect(struct prog_mem *m, uint64_t vaddr, uint64_t bytes,
                  uint32_t prot) {
    int idx;
    struct prog_map *e;

    if (!m) return (int)IRIS_ERR_INVALID_ARG;
    idx = prog_mem_find(m, vaddr, bytes);
    if (idx < 0) return (int)IRIS_ERR_NOT_FOUND;
    e = &m->maps[idx];
    /*
     * The whole mapping or nothing.
     *
     * A frame is mapped by ONE invocation covering all of it, so changing the
     * protection of part of one would mean splitting the frame, and a frame
     * does not split.  A caller that needs a sub-range — which is exactly what
     * `PT_GNU_RELRO` is — has to have mapped it as its own frame, and that is
     * a decision for whoever laid the segments out, not something this call
     * can fix after the fact.  Refused rather than silently widened, because
     * silently widening is how a writable page becomes read-only under code
     * that was still using it.
     */
    if (vaddr != e->vaddr || bytes != e->bytes)
        return (int)IRIS_ERR_NOT_SUPPORTED;
    if (prot == e->prot) return 0;

    if (iris_invoke2((long)e->slot, INV_FRAME_UNMAP,
                     (long)m->vspace_c, (long)e->vaddr) != 0)
        return (int)IRIS_ERR_INVALID_ARG;
    if (iris_map_frame(e->slot, m->vspace_c, m->untyped_c, m->pt_slot,
                       e->vaddr, e->bytes, (uint64_t)prot) != 0) {
        /*
         * The remap failed and the pages are GONE.
         *
         * Put the old protection back rather than returning with the mapping
         * missing: a caller that got an error and still has its memory can
         * carry on, and one whose memory silently vanished faults somewhere
         * else entirely.  If even that fails the record is dropped, because a
         * record of a mapping that is not there is worse than no record.
         */
        if (iris_map_frame(e->slot, m->vspace_c, m->untyped_c, m->pt_slot,
                           e->vaddr, e->bytes, (uint64_t)e->prot) != 0)
            prog_mem_forget(m, idx);
        return (int)IRIS_ERR_INVALID_ARG;
    }
    e->prot = prot;
    return 0;
}
