/* SPDX-License-Identifier: Apache-2.0 */
/*
 * prog_mem_plan.c — where a program's next mapping goes.
 *
 * The arithmetic half of `prog_mem.h`, separated so it can be compiled for the
 * host and asserted on.  Every function here is pure: it reads and writes the
 * caller's `struct prog_mem` and invokes nothing.
 *
 * It is separated because this is the half that can be WRONG QUIETLY.  A
 * mis-ordered cursor hands out an address inside the stack guard, an
 * off-by-one in the slot pool hands out a slot another mapping is using, and a
 * `find` that accepts a partial overlap lets `mprotect` change the protection
 * of a page its caller did not name.  None of those fault where the mistake
 * is.
 */
#include "prog_mem.h"
#include <iris/program_abi.h>

void prog_mem_setup(struct prog_mem *m, uint64_t private_base,
                    uint64_t untyped_c, uint64_t vspace_c,
                    uint32_t slot_lo, uint32_t slot_hi, uint32_t pt_slot) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;

    m->untyped_c = untyped_c;
    m->vspace_c  = vspace_c;
    m->slot_next = slot_lo;
    m->slot_end  = slot_hi;
    m->pt_slot   = pt_slot;

    m->brk_base  = private_base + IRIS_PROG_HEAP_OFF;
    m->brk_cur   = m->brk_base;
    m->brk_end   = private_base + IRIS_PROG_HEAP_END_OFF;
    m->heap_top  = m->brk_base;

    m->mmap_cur  = private_base + IRIS_PROG_MMAP_OFF;
    /*
     * The `mmap` region's ceiling is the STACK, and the guard page below it is
     * left out of the region rather than merely respected.  A cursor that
     * could hand out the guard page would silently turn a stack overflow — the
     * one thing that page exists to catch — into a write into somebody's
     * mapping.
     */
    m->mmap_end  = private_base + IRIS_PROG_STACK_OFF - 4096ULL;
}

uint64_t prog_mem_pages(uint64_t bytes) {
    if (bytes > (uint64_t)-1 - 4095ULL) return 0;   /* saturate, never wrap */
    return (bytes + 4095ULL) & ~4095ULL;
}

int prog_mem_reserve(struct prog_mem *m, uint64_t bytes, int heap,
                     uint64_t *out_va) {
    uint64_t *cur = heap ? &m->heap_top : &m->mmap_cur;
    uint64_t  end = heap ?  m->brk_end  :  m->mmap_end;
    uint64_t  n   = prog_mem_pages(bytes);

    if (!m || !out_va || n == 0u) return 0;
    /* Subtraction, so a size the caller chose cannot wrap the test into
     * passing — the same rule the kernel's own bump allocator states. */
    if (*cur > end || n > end - *cur) return 0;

    *out_va = *cur;
    *cur   += n;
    return 1;
}

#define PROG_MEM_CHUNK_MIN (64u << 10)

uint64_t prog_mem_chunk(const struct prog_mem *m, uint64_t need) {
    uint64_t have, want;
    if (!m) return 0;
    need = prog_mem_pages(need);
    if (need == 0u) return 0;

    have = m->heap_top - m->brk_base;          /* what the heap already costs */
    want = (have > (uint64_t)PROG_MEM_CHUNK_MIN) ? have
                                                 : (uint64_t)PROG_MEM_CHUNK_MIN;
    if (want < need) want = need;
    /* Never past the region: a chunk that would not fit is trimmed to what
     * does, and the reserve below is what refuses if even that is nothing. */
    if (m->heap_top <= m->brk_end && want > m->brk_end - m->heap_top)
        want = m->brk_end - m->heap_top;
    if (want < need) return 0;
    return want;
}

int prog_mem_take_slot(struct prog_mem *m, uint32_t *out_slot) {
    if (!m || !out_slot) return 0;
    if (m->slot_next > m->slot_end) return 0;
    *out_slot = m->slot_next++;
    return 1;
}

int prog_mem_find(const struct prog_mem *m, uint64_t vaddr, uint64_t bytes) {
    if (!m || bytes == 0u) return -1;
    if (vaddr > (uint64_t)-1 - bytes) return -1;
    for (uint32_t i = 0; i < m->nmaps; i++) {
        const struct prog_map *e = &m->maps[i];
        if (vaddr < e->vaddr) continue;
        if (vaddr - e->vaddr > e->bytes) continue;
        if (bytes > e->bytes - (vaddr - e->vaddr)) continue;
        return (int)i;
    }
    return -1;
}

int prog_mem_record(struct prog_mem *m, uint64_t vaddr, uint64_t bytes,
                    uint32_t slot, uint32_t prot, int heap) {
    if (!m || m->nmaps >= PROG_MEM_MAPS) return 0;
    struct prog_map *e = &m->maps[m->nmaps++];
    e->vaddr = vaddr;
    e->bytes = bytes;
    e->slot  = slot;
    e->prot  = prot;
    e->heap  = heap ? 1u : 0u;
    return 1;
}

void prog_mem_forget(struct prog_mem *m, int index) {
    if (!m || index < 0 || (uint32_t)index >= m->nmaps) return;
    /* The LAST entry moves into the hole.  Order carries no meaning here —
     * `find` scans — and a memmove down would be a second thing to get wrong
     * for no gain. */
    m->maps[index] = m->maps[m->nmaps - 1u];
    m->nmaps--;
}
