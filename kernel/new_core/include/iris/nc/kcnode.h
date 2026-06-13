#ifndef IRIS_NC_KCNODE_H
#define IRIS_NC_KCNODE_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <stdint.h>

/* Maximum and default slot counts.  slot_count MUST be a power-of-2 for
 * CSpace traversal (ctzll-based radix extraction).  KCNODE_MAX_SLOTS raised
 * from 256 to 4096 (= 2^12) to support wide CSpace fanout. */
#define KCNODE_MAX_SLOTS     4096u
#define KCNODE_DEFAULT_SLOTS  256u   /* used by UNTYPED_RETYPE when no count given */

struct KCSlot {
    struct KObject *object;
    iris_rights_t   rights;
    uint64_t        badge;   /* Fase 9: per-cap sender identity.  0 =
                              * unbadged.  Set only at mint time by the
                              * minting authority; copy/move/swap preserve
                              * it; it never crosses to other caps. */
};

/*
 * Variable-capacity capability node.
 *
 * Memory layout (one contiguous allocation):
 *   [struct KCNode header][struct KCSlot slots[slot_count]]
 *
 * The slots pointer always points to (this + 1): the KCSlot array immediately
 * follows the header.  Use KCNODE_ALLOC_SIZE(n) to compute the required bytes.
 *
 * slot_count MUST be a power-of-2 ≥ 1.  Enforced by kcnode_alloc and
 * sys_cnode_create.  This constraint enables O(1) CSpace traversal via
 * ctzll(slot_count) bits per level.
 */
struct KCNode {
    struct KObject   base;       /* must be first */
    irq_spinlock_t   lock;
    uint32_t         slot_count;
    struct KCSlot   *slots;      /* inline array immediately after header */
};

/* Total allocation size for a KCNode with n slots */
#define KCNODE_ALLOC_SIZE(n) \
    ((uint32_t)(sizeof(struct KCNode) + (uint32_t)((n) * sizeof(struct KCSlot))))

struct KCNode *kcnode_alloc(uint32_t num_slots);
struct KCNode *kcnode_alloc_at(void *mem, uint32_t num_slots); /* untyped-backed */
void           kcnode_close(struct KCNode *cn);
iris_error_t   kcnode_mint(struct KCNode *cn, uint32_t slot_idx,
                            struct KObject *obj, iris_rights_t rights);
/* Exclusive mint (Fase 8): fails with IRIS_ERR_ALREADY_EXISTS if the slot
 * is occupied instead of silently replacing the cap.  Used by
 * SYS_PROC_CSPACE_MINT so a spawner cannot clobber a child's slots. */
iris_error_t   kcnode_mint_excl(struct KCNode *cn, uint32_t slot_idx,
                                 struct KObject *obj, iris_rights_t rights);
/* Overwrite mint preserving an explicit badge (Fase 9; MOVE path). */
iris_error_t   kcnode_mint_badged(struct KCNode *cn, uint32_t slot_idx,
                                   struct KObject *obj, iris_rights_t rights,
                                   uint64_t badge);
/* Badged exclusive mint (Fase 9): like kcnode_mint_excl but also records
 * the per-cap badge in the slot. */
iris_error_t   kcnode_mint_excl_badged(struct KCNode *cn, uint32_t slot_idx,
                                        struct KObject *obj,
                                        iris_rights_t rights, uint64_t badge);
iris_error_t   kcnode_fetch(struct KCNode *cn, uint32_t slot_idx,
                             struct KObject **out_obj, iris_rights_t *out_rights);
/* Badge-aware fetch (Fase 9): also returns the slot badge. */
iris_error_t   kcnode_fetch_badged(struct KCNode *cn, uint32_t slot_idx,
                                    struct KObject **out_obj,
                                    iris_rights_t *out_rights,
                                    uint64_t *out_badge);
iris_error_t   kcnode_delete(struct KCNode *cn, uint32_t slot_idx);
iris_error_t   kcnode_swap(struct KCNode *cn, uint32_t slot_a, uint32_t slot_b);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KCNODE_H */
