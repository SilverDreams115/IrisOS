#ifndef IRIS_NC_HANDLE_H
#define IRIS_NC_HANDLE_H

#include <stdint.h>
#include <iris/nc/kobject.h>  /* forward-declares struct KObject for both kernel and user */
#include <iris/nc/rights.h>

/*
 * handle_id_t: process-visible opaque token.
 *   bit  [31]    = HANDLE_TAG — always 1 on a valid handle
 *   bits [30:10] = generation counter
 *   bits [9:0]   = slot index (0..1023)
 *
 * HANDLE_INVALID = 0 always.
 * Generation 0 is FORBIDDEN for valid handles.
 *
 * The tag bit is what unblocks the dual-namespace retirement.  Handles used to
 * be `slot | gen << 10`, i.e. every value from 1024 upward, which forced the
 * CPtr window to be everything BELOW 1024 — ten bits for the whole capability
 * address space of a process.  With a 256-slot root CNode that left two bits
 * for deeper CSpace levels, so multi-level CSpace was unusable and a process
 * that filled its root simply had nowhere left to put a capability.
 *
 * Moving handles to the top of the word costs one generation bit (still ~2M
 * generations per slot) and gives CPtrs the entire low 31 bits.  Nothing else
 * about handles changes: same slot count, same generation discipline, same
 * HANDLE_INVALID.  The two namespaces are still told apart by value, and that
 * discrimination still disappears when the handle namespace does — it just no
 * longer strangles the namespace that is replacing it.
 */
typedef uint32_t handle_id_t;
#define HANDLE_INVALID    ((handle_id_t)0)
#define HANDLE_TAG        (1u << 31)
#define HANDLE_SLOT_BITS  10u
#define HANDLE_SLOT_MASK  ((1u << HANDLE_SLOT_BITS) - 1u)
#define HANDLE_GEN_SHIFT  HANDLE_SLOT_BITS
#define HANDLE_GEN_MAX    ((1u << (31u - HANDLE_SLOT_BITS)) - 1u)

static inline uint32_t handle_id_slot(handle_id_t id) {
    return (uint32_t)(id & HANDLE_SLOT_MASK);
}
static inline uint32_t handle_id_gen(handle_id_t id) {
    return (uint32_t)((id & ~HANDLE_TAG) >> HANDLE_GEN_SHIFT);
}
static inline handle_id_t handle_id_make(uint32_t slot, uint32_t gen) {
    /* generation 0 is forbidden — wrap skips to 1 */
    if (gen == 0u) gen = 1u;
    return (handle_id_t)(HANDLE_TAG |
                         ((gen & HANDLE_GEN_MAX) << HANDLE_GEN_SHIFT) |
                         (slot & HANDLE_SLOT_MASK));
}

#ifdef __KERNEL__
/*
 * HandleEntry: kernel-internal handle slot.
 * Lives exclusively inside a HandleTable or in transit through a KChannel.
 * The handle_id_t token is NOT stored here — it is the table's address space.
 *
 * Invariants:
 *   - object != NULL for a slot that is in use
 *   - rights are immutable after initialisation
 *   - gen matches the generation counter of the HandleTable for this slot
 *   - Only HandleTable creates and destroys HandleEntry instances
 */
struct HandleEntry {
    struct KObject *object;
    iris_rights_t   rights;
    uint32_t        gen;
};
/* Fase 9 note: the per-cap badge for handle-namespace caps lives in a
 * parallel uint32_t array inside HandleTable (badge[slot]) — NOT here —
 * to keep sizeof(KProcess) inside the largest kslab class.  Handle-side
 * badges are 32-bit, matching the SYS_PROC_CSPACE_MINT arg3 packing. */

void handle_entry_init(struct HandleEntry *e, struct KObject *obj,
                       iris_rights_t rights, uint32_t gen);
void handle_entry_reset(struct HandleEntry *e);
#endif /* __KERNEL__ */

#endif /* IRIS_NC_HANDLE_H */
