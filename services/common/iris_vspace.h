#ifndef IRIS_COMMON_VSPACE_H
#define IRIS_COMMON_VSPACE_H

/*
 * iris_vspace.h — supplying your own paging levels (Stage 6-pure, Etapa 2).
 *
 * The kernel stopped creating page tables.  A map into an address space whose
 * holder has a budget answers IRIS_ERR_MISSING_TABLE when the walk for that
 * address is incomplete, and the holder is expected to retype a paging level
 * and install it.  That is seL4's contract — seL4_FailedLookup on the map,
 * seL4_X86_PageTable_Map to fix it — and this is the client side of it.
 *
 * WHY A RETRY LOOP AND NOT A PRE-PASS.  How deep a walk already is depends on
 * what the address space has mapped before: two addresses a gigabyte apart can
 * share a PDPT, and an address space that has mapped nothing needs three
 * levels.  A caller that computed the depth up front would be re-deriving
 * state the kernel already knows and would get it wrong the moment anything
 * else mapped nearby.  Asking, being told exactly what is missing, and fixing
 * that one thing is both simpler and correct under concurrency.
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
 * Install paging levels until the walk for `vaddr` is complete.
 *
 *   vspace_c:  KOBJ_VSPACE (RIGHT_WRITE) for the address space being filled —
 *              the CALLER's own, or a child's from SYS_PROCESS_VSPACE.
 *   untyped_c: the budget the levels are retyped from (RIGHT_WRITE).
 *   dest:      a destination slot for the table capability, in the RETYPE2
 *              packing (cnode | slot<<32).  Reused for every level.
 *   slot_c:    the CPtr that `dest` names, so the slot can be cleared between
 *              levels (publication is exclusive).
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
            /* A CPtr below 256 is a slot of the caller's ROOT CNode; at or
             * above it, the low byte names a second-level CNode and the rest
             * the leaf.  SYS_CNODE_DELETE takes (cnode, slot), so the two
             * forms do not share an argument order. */
            if (slot_c >= 256)
                (void)iris_syscall2(SYS_CNODE_DELETE, (long)(slot_c & 0xFFu),
                                    (long)(slot_c >> 8));
            else
                (void)iris_syscall2(SYS_CNODE_DELETE, 0, slot_c);
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
        if (slot_c >= 256)
            (void)iris_syscall2(SYS_CNODE_DELETE, (long)(slot_c & 0xFFu),
                                (long)(slot_c >> 8));
        else
            (void)iris_syscall2(SYS_CNODE_DELETE, 0, slot_c);
    }
    return 0;
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
    long e = iris_vspace_ensure(vspace_c, untyped_c, dest, slot_c, vaddr);
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
        if (vs_slot >= 256)
            (void)iris_syscall2(SYS_CNODE_DELETE, (long)(vs_slot & 0xFFu),
                                (long)(vs_slot >> 8));
        else
            (void)iris_syscall2(SYS_CNODE_DELETE, 0, vs_slot);
        if (iris_syscall2(SYS_PROCESS_VSPACE, a1, vs_dest) != 0)
            return (long)IRIS_ERR_MISSING_TABLE;
        vs = vs_slot; va = a2;
    } else {
        return (long)IRIS_ERR_MISSING_TABLE;
    }

    long e = iris_vspace_ensure(vs, untyped_c, pt_dest, pt_slot, (uint64_t)va);

    /* Drop the child's VSpace capability again.  Holding it would keep that
     * address space alive past the death of the process it belongs to, which
     * in turn holds every page table installed in it — and therefore holds
     * child entries on a budget its owner is entitled to RESET once the child
     * is gone.  A scratch slot is scratch. */
    if (nr == SYS_VMO_MAP_INTO && vs_slot) {
        if (vs_slot >= 256)
            (void)iris_syscall2(SYS_CNODE_DELETE, (long)(vs_slot & 0xFFu),
                                (long)(vs_slot >> 8));
        else
            (void)iris_syscall2(SYS_CNODE_DELETE, 0, vs_slot);
    }
    if (e != 0) return e;
    return iris_syscall4(nr, a0, a1, a2, a3);
}

#endif /* IRIS_COMMON_VSPACE_H */
