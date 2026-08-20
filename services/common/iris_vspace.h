#ifndef IRIS_COMMON_VSPACE_H
#define IRIS_COMMON_VSPACE_H

/*
 * iris_vspace.h — supplying your own paging levels (Stage 6-pure, Step 2).
 *
 * The kernel stopped creating page tables.  A map into an address space whose
 * holder has a budget answers IRIS_ERR_MISSING_TABLE when the walk for that
 * address is incomplete, and the holder is expected to retype a paging level
 * and install it.  That is seL4's contract — seL4_FailedLookup on the map,
 * seL4_X86_PageTable_Map to fix it — and this is the client side of it.
 *
 * WHY THE DEPTH IS ASKED FOR, NOT COMPUTED.  How deep a walk already is
 * depends on what the address space has mapped before: two addresses a
 * gigabyte apart can share a PDPT, and an address space that has mapped
 * nothing needs three levels.  A caller that computed the depth up front would
 * be re-deriving state the kernel already knows and would get it wrong the
 * moment anything else mapped nearby.  Asking, being told exactly what is
 * missing, and fixing that one thing is both simpler and correct under
 * concurrency.
 *
 * WHICH ADDRESSES to ask about is the caller's own arithmetic, and is not the
 * same question.  A page table covers 2 MiB, so a range wider than that needs
 * one per region it touches — a fact about the range, knowable without looking
 * at the address space at all.  iris_vspace_ensure_range walks the regions and
 * asks about each; iris_vspace_ensure answers one.
 *
 * THE TABLE CAPABILITY IS NOT KEPT.  Installing hands the VSpace a reference
 * of its own, so the slot used to carry the capability can be reused for the
 * next level immediately — and IS, here, so ensuring a deep walk costs one
 * slot rather than three.  The tables come back when the address space is
 * destroyed, not when this slot is overwritten.  A holder that wants to unmap
 * a level later must keep its own capability instead of reusing the slot.
 */

#include <iris/syscall.h>
#include <iris/nc/error.h>
#include <stdint.h>

/*
 * Clear the slot a RETYPE2 destination names.
 *
 * `dest` is the destination packing every publishing syscall already uses —
 * CNode CPtr in the low 32 bits (0 = the caller's own root), slot index in the
 * high 32 — which is exactly the (cnode, slot) pair SYS_CNODE_DELETE takes.
 *
 * Deriving the pair from `dest` rather than decoding the leaf CPtr is not a
 * tidy-up.  The decode it replaces was `slot_c & 0xFF` / `slot_c >> 8`, which
 * hard-codes a 256-slot root CNode — and since Stage 6-pure Step 5 the spawner
 * CHOOSES its child's CSpace width, so a child with a 64-slot root resolves
 * CPtrs on a 6-bit radix and that decode names a different CNode and a
 * different slot.  Not a failure: a successful delete of somebody else's
 * capability.  `dest` carries the two numbers already split, so there is no
 * radix to assume.
 */
static inline void iris_vspace_slot_clear(long dest) {
    (void)iris_syscall2(SYS_CNODE_DELETE,
                        (long)((uint64_t)dest & 0xFFFFFFFFu),
                        (long)((uint64_t)dest >> 32));
}

/*
 * Install paging levels until the walk for `vaddr` is complete.
 *
 *   vspace_c:  KOBJ_VSPACE (RIGHT_WRITE) for the address space being filled —
 *              the CALLER's own, or a child's from SYS_PROCESS_VSPACE.
 *   untyped_c: the budget the levels are retyped from (RIGHT_WRITE).
 *   dest:      a destination slot for the table capability, in the RETYPE2
 *              packing (cnode | slot<<32).  Reused for every level.
 *   slot_c:    the CPtr that `dest` names, so the slot can be READ between
 *              levels (SYS_CAP_IDENTIFY and SYS_VSPACE_MAP_TABLE take a CPtr;
 *              clearing it goes through `dest`).
 *
 * Returns 0 when the walk is complete — including when it already was — or a
 * negative iris_error_t.  Bounded at 3: x86-64 has exactly three levels below
 * the PML4, so a loop that has not finished by then is not going to.
 */
static inline long iris_vspace_ensure(long vspace_c, long untyped_c,
                                      long dest, long slot_c, uint64_t vaddr) {
    for (int level = 0; level < 3; level++) {
        /*
         * Retype only when the slot does not already hold a spare level.
         *
         * A call that finds the walk complete leaves its unused table sitting
         * here, and the next call reuses it.  Without that, every ensure over
         * an already-complete walk — which is most of them, because callers
         * pre-pass whole ranges — would burn a page of the budget to discover
         * that there was nothing to do.
         */
        if (iris_syscall1(SYS_CAP_IDENTIFY, slot_c)
                != (long)IRIS_HANDLE_TYPE_PAGE_TABLE) {
            iris_vspace_slot_clear(dest);
            long rr = iris_syscall4(SYS_UNTYPED_RETYPE2, untyped_c,
                                    (long)((uint64_t)IRIS_KOBJ_PAGE_TABLE | (1ULL << 32)),
                                    dest, 4096);
            if (rr != 0) return rr;
        }

        long r = iris_syscall3(SYS_VSPACE_MAP_TABLE, slot_c, vspace_c, (long)vaddr);
        if (r == (long)IRIS_ERR_ALREADY_EXISTS) return 0;  /* walk complete */
        if (r != 0) return r;
        /* Installed: the VSpace holds its own reference, so this capability is
         * spent.  Drop it so the next level starts from a fresh table. */
        iris_vspace_slot_clear(dest);
    }
    return 0;
}

/* A page table covers 2 MiB of virtual address space, so a range needs one per
 * 2 MiB region it touches — counted from the region the range STARTS in, not
 * from its first byte. */
#define IRIS_VSPACE_PT_SPAN 0x200000ULL

/*
 * Fill the walk for EVERY 2 MiB region a [vaddr, vaddr+bytes) range touches.
 *
 * The single-address form is not enough for the syscalls this header wraps.
 * SYS_VMO_MAP and SYS_VMO_MAP_INTO map a whole VMO in one call, and a VMO
 * larger than what one page table covers — a framebuffer, a service image —
 * crosses into a region whose PT nobody supplied.  The kernel reports
 * MISSING_TABLE, the caller ensures the FIRST address, retries, and fails at
 * exactly the same boundary: a fixup that cannot converge, and (because the
 * map rolls back) a silent one.  fb painted nothing above 2 MiB of
 * framebuffer and vfs dropped every initrd export that big.
 */
static inline long iris_vspace_ensure_range(long vspace_c, long untyped_c,
                                            long dest, long slot_c,
                                            uint64_t vaddr, uint64_t bytes) {
    uint64_t first = vaddr & ~(IRIS_VSPACE_PT_SPAN - 1ULL);
    uint64_t last  = (vaddr + (bytes ? bytes - 1ULL : 0ULL))
                     & ~(IRIS_VSPACE_PT_SPAN - 1ULL);
    for (uint64_t va = first;; va += IRIS_VSPACE_PT_SPAN) {
        long r = iris_vspace_ensure(vspace_c, untyped_c, dest, slot_c, va);
        if (r != 0) return r;
        if (va >= last) return 0;
    }
}

/*
 * How many bytes a mapping syscall will cover, so the fixup knows how much of
 * the walk it owes.
 *
 * The whole-VMO maps say it themselves: SYS_VMO_SIZE reads the size off the
 * capability being mapped, which is the same number the kernel loops over.
 * The page-granular ones cover exactly one page by construction.  Guessing one
 * page for the whole-VMO forms is what made the fixup unable to converge.
 */
static inline uint64_t iris_vspace_map_span(long nr, long vmo_c) {
    if (nr == SYS_VMO_MAP || nr == SYS_VMO_MAP_INTO) {
        long sz = iris_syscall1(SYS_VMO_SIZE, vmo_c);
        if (sz > 0) return (uint64_t)sz;
    }
    return 4096ULL;
}

/*
 * Map, and supply what the kernel says is missing.
 *
 * On IRIS_ERR_MISSING_TABLE the levels for `vaddr` are filled and the map is
 * retried exactly once, because iris_vspace_ensure returns only when the walk
 * is complete — a second MISSING_TABLE would mean something else unmapped a
 * level underneath us, which is a real error and not something to spin on.
 */
static inline long iris_vspace_map(long nr, long a0, long a1, long a2, long a3,
                                   long vspace_c, long untyped_c,
                                   long dest, long slot_c, uint64_t vaddr) {
    long r = iris_syscall4(nr, a0, a1, a2, a3);
    if (r != (long)IRIS_ERR_MISSING_TABLE) return r;
    long e = iris_vspace_ensure_range(vspace_c, untyped_c, dest, slot_c, vaddr,
                                      iris_vspace_map_span(nr, a0));
    if (e != 0) return e;
    return iris_syscall4(nr, a0, a1, a2, a3);
}

/*
 * The same thing, for a service that would rather fix this once at its syscall
 * wrapper than at every map site.
 *
 * Each mapping syscall says where its target address space and virtual address
 * are, so the decode is a fact about the ABI rather than a guess: SYS_VMO_MAP
 * maps into the CALLER's own address space, SYS_FRAME_MAP and
 * SYS_VMO_MAP_PAGE name theirs, and SYS_VMO_MAP_INTO names a PROCESS whose
 * address space is a capability of its own.  Anything else that returned
 * MISSING_TABLE would be a kernel bug, so it is passed through unchanged.
 *
 *   self_vs:   the caller's own KOBJ_VSPACE (SYS_VSPACE_SELF), or 0 if it has
 *              not got one — SYS_VMO_MAP then cannot be fixed up.
 *   untyped_c: the budget levels are retyped from.  For a child of svc_loader
 *              this is IRIS_CPTR_OWN_VSPACE_POOL, the region its own address
 *              space was built from.
 *   pt_dest/pt_slot:  scratch for the level capability (RETYPE2 dest packing,
 *              and the CPtr it names).
 *   vs_dest/vs_slot:  scratch for a child's VSpace; 0 disables SYS_VMO_MAP_INTO
 *              fixup, which most services never issue.
 */
static inline long iris_vspace_fixup(long nr, long a0, long a1, long a2, long a3,
                                     long self_vs, long untyped_c,
                                     long pt_dest, long pt_slot,
                                     long vs_dest, long vs_slot) {
    long vs, va;
    if (nr == SYS_VMO_MAP) {
        if (!self_vs) return (long)IRIS_ERR_MISSING_TABLE;
        vs = self_vs; va = a1;
    } else if (nr == SYS_FRAME_MAP || nr == SYS_VMO_MAP_PAGE) {
        vs = a1; va = a2;
    } else if (nr == SYS_VMO_MAP_INTO) {
        if (!vs_slot) return (long)IRIS_ERR_MISSING_TABLE;
        iris_vspace_slot_clear(vs_dest);
        if (iris_syscall2(SYS_PROCESS_VSPACE, a1, vs_dest) != 0)
            return (long)IRIS_ERR_MISSING_TABLE;
        vs = vs_slot; va = a2;
    } else {
        return (long)IRIS_ERR_MISSING_TABLE;
    }

    long e = iris_vspace_ensure_range(vs, untyped_c, pt_dest, pt_slot,
                                      (uint64_t)va,
                                      iris_vspace_map_span(nr, a0));

    /* Drop the child's VSpace capability again.  Holding it would keep that
     * address space alive past the death of the process it belongs to, which
     * in turn holds every page table installed in it — and therefore holds
     * child entries on a budget its owner is entitled to RESET once the child
     * is gone.  A scratch slot is scratch. */
    if (nr == SYS_VMO_MAP_INTO && vs_slot)
        iris_vspace_slot_clear(vs_dest);
    if (e != 0) return e;
    return iris_syscall4(nr, a0, a1, a2, a3);
}

#endif /* IRIS_COMMON_VSPACE_H */
