/*
 * test_syscall_retype.c — the OBJECT-CREATION authority, under host unit test.
 *
 * SYS_UNTYPED_RETYPE2 is the only way a kernel object comes into existence
 * since Phase S1, so its argument validation is the narrowest place in the
 * system: everything the kernel will ever hold passes through this switch
 * first.  A malformed size accepted here is a kernel object built to the wrong
 * shape; a slot count accepted here that is not a power of two is a CSpace
 * whose radix walk indexes off the end of its own array.
 *
 * Every case below is checked BEFORE the source Untyped is resolved — the
 * handler validates the type and computes the payload "before touching state",
 * which is why these tests need no Untyped and no memory.  That ordering is
 * itself the property worth pinning: a retype that fails must fail without
 * having consumed anything, or a caller learns it was refused by discovering
 * its budget shrank.
 *
 * The device-capability creators are here for the same reason.  They are the
 * two syscalls that turn boot authority into hardware access, and their
 * refusals are the boundary between "holds the IRQ control capability" and
 * "can route any interrupt line".
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/cspace.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/rights.h>
#include <iris/task.h>
#include <iris/syscall.h>
#include <iris/kpage.h>
#include <string.h>

void test_set_current_task(struct task *t);

uint64_t sys_untyped_retype2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_untyped_retype(uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t sys_cap_create_irqcap(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t sys_cap_create_ioport(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

static long rt_err(uint64_t r) { return (long)(int64_t)r; }

/* arg1 packs type | count<<32; arg2 packs dest_cnode | dest_slot<<32. */
static uint64_t rt_type(uint32_t type, uint32_t count) {
    return (uint64_t)type | ((uint64_t)count << 32);
}
static uint64_t rt_dest(uint32_t cnode, uint32_t slot) {
    return (uint64_t)cnode | ((uint64_t)slot << 32);
}

static struct task *rt_caller(void) {
    struct task *t = (struct task *)kpage_alloc((uint32_t)sizeof(struct task));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    struct KCNode *root = kcnode_alloc(256);
    if (!root) return NULL;
    kobject_active_retain(&root->base);
    t->cspace_root = root;
    test_set_current_task(t);
    return t;
}

void test_syscall_retype(void) {
    TEST_SUITE("object-creation authority (retype + device caps)");

    /* ── RT-1: no caller, or no CSpace, before anything else ─────────────*/
    {
        test_set_current_task(NULL);
        ASSERT_EQ(rt_err(sys_untyped_retype2(1, rt_type(KOBJ_ENDPOINT, 1),
                                             rt_dest(0, 4), 0)),
                  (long)IRIS_ERR_INVALID_ARG);
    }

    /* ── RT-2: an unknown object type is NOT_SUPPORTED, not INVALID_ARG ──
     * The distinction is the ABI's: a type the kernel has never had and a type
     * it retired are the same answer to a caller, and both differ from "you
     * named a legal type badly".  A retype that collapsed them would make a
     * forward-compatible caller unable to tell "this kernel is older than I
     * thought" from "I built the argument wrong". */
    {
        struct task *t = rt_caller();
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(rt_err(sys_untyped_retype2(1, rt_type(0x7FFFu, 1),
                                             rt_dest(0, 4), 0)),
                  (long)IRIS_ERR_NOT_SUPPORTED);
        /* KOBJ_PROCESS is a RESERVED enumerator since Stage 7-proc: no live
         * capability carries it and nothing may create one. */
        ASSERT_EQ(rt_err(sys_untyped_retype2(1, rt_type(KOBJ_PROCESS, 1),
                                             rt_dest(0, 4), 0)),
                  (long)IRIS_ERR_NOT_SUPPORTED);
        test_set_current_task(NULL);
    }

    /* ── RT-3: a CNode's slot count must be a power of two, in range ──────
     * Not a style rule.  CSpace traversal extracts radix bits with ctz(count)
     * and indexes with `cptr & (count - 1)`; a non-power-of-two makes that mask
     * wrong, so a CPtr would index past the end of the slot array.  The check
     * is what keeps the walk's arithmetic honest. */
    {
        struct task *t = rt_caller();
        ASSERT_NOT_NULL(t);
        const uint64_t ty = rt_type(KOBJ_CNODE, 1);
        const uint64_t de = rt_dest(0, 4);
        ASSERT_EQ(rt_err(sys_untyped_retype2(1, ty, de, 3)),      /* not 2^n */
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(rt_err(sys_untyped_retype2(1, ty, de, 100)),    /* not 2^n */
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(rt_err(sys_untyped_retype2(1, ty, de,
                                             KCNODE_MAX_SLOTS * 2u)),
                  (long)IRIS_ERR_INVALID_ARG);
        test_set_current_task(NULL);
    }

    /* ── RT-4: a batch is bounded ────────────────────────────────────────
     * The whole batch is carved, initialised and published under the untyped
     * lock, so its size is an IRQ-off window.  An unbounded count would let a
     * ring-3 caller choose how long interrupts stay disabled. */
    {
        struct task *t = rt_caller();
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(rt_err(sys_untyped_retype2(1,
                             rt_type(KOBJ_ENDPOINT, KUNTYPED_RETYPE_MAX_COUNT + 1u),
                             rt_dest(0, 4), 0)),
                  (long)IRIS_ERR_INVALID_ARG);
        test_set_current_task(NULL);
    }

    /* ── RT-5: physical-region types take exactly one, page-aligned ──────
     * A Frame, a page table, a VSpace and a sub-Untyped are REGIONS.  A batch
     * of them has no meaning (each needs its own base), and a size that is not
     * a whole number of pages describes a region the paging hardware cannot
     * address. */
    {
        struct task *t = rt_caller();
        ASSERT_NOT_NULL(t);
        const uint32_t phys[] = { KOBJ_UNTYPED, KOBJ_FRAME,
                                  KOBJ_PAGE_TABLE, KOBJ_VSPACE };
        for (unsigned i = 0; i < 4; i++) {
            /* count != 1 */
            ASSERT_EQ(rt_err(sys_untyped_retype2(1, rt_type(phys[i], 2),
                                                 rt_dest(0, 4), 4096)),
                      (long)IRIS_ERR_INVALID_ARG);
            /* below a page */
            ASSERT_EQ(rt_err(sys_untyped_retype2(1, rt_type(phys[i], 1),
                                                 rt_dest(0, 4), 2048)),
                      (long)IRIS_ERR_INVALID_ARG);
            /* not page-aligned */
            ASSERT_EQ(rt_err(sys_untyped_retype2(1, rt_type(phys[i], 1),
                                                 rt_dest(0, 4), 4097)),
                      (long)IRIS_ERR_INVALID_ARG);
        }
        test_set_current_task(NULL);
    }

    /* ── RT-6: the legacy handle-publishing retype is gone for good ──────
     * SYS_UNTYPED_RETYPE (87) predates capabilities living in CSpace.  It is
     * retired rather than fixed, and a caller that still finds it must get a
     * refusal rather than a second object-creation path. */
    {
        struct task *t = rt_caller();
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(rt_err(sys_untyped_retype(1, 2, 3)),
                  (long)IRIS_ERR_NOT_SUPPORTED);
        test_set_current_task(NULL);
    }

    /* ── RT-7: device capabilities check their argument before authority ──
     * These two syscalls are the boundary between holding a boot control
     * capability and reaching hardware.  An out-of-range IRQ or a destination
     * of slot 0 is refused before the authorising capability is even resolved,
     * so a malformed call cannot be used to probe which authority the caller
     * holds by timing or by error code. */
    {
        struct task *t = rt_caller();
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(rt_err(sys_cap_create_irqcap(1, 16, 4, 0)),   /* IRQ > 15 */
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(rt_err(sys_cap_create_irqcap(1, 1, 0, 0)),    /* slot 0   */
                  (long)IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(rt_err(sys_cap_create_irqcap(1, 1, 1024, 0)), /* slot OOR */
                  (long)IRIS_ERR_INVALID_ARG);
        test_set_current_task(NULL);
    }

    /* ── RT-8: and they refuse a caller with no authority at all ──────────
     * The authorising capability is resolved from the caller's own CSpace.  An
     * empty root holds nothing, so both must refuse — never fall back to a
     * default, an ambient permission, or the caller's own identity. */
    {
        struct task *t = rt_caller();
        ASSERT_NOT_NULL(t);
        ASSERT_EQ(rt_err(sys_cap_create_irqcap(9, 1, 4, 0)) < 0, 1);
        ASSERT_EQ(rt_err(sys_cap_create_ioport(9, 0x3F8, 8, 4)) < 0, 1);
        test_set_current_task(NULL);
    }

    test_set_current_task(NULL);
}
