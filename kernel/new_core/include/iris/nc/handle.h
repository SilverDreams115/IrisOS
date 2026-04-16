#ifndef IRIS_NC_HANDLE_H
#define IRIS_NC_HANDLE_H

#include <stdint.h>
#include <iris/nc/kobject.h>  /* forward-declares struct KObject for both kernel and user */
#include <iris/nc/rights.h>

/*
 * handle_id_t: token opaco visible al proceso.
 *   bits [9:0]  = slot index (0..1023)
 *   bits [31:10] = generation counter
 *
 * HANDLE_INVALID = 0 siempre.
 * Generation 0 PROHIBIDA para handles válidos.
 */
typedef uint32_t handle_id_t;
#define HANDLE_INVALID    ((handle_id_t)0)
#define HANDLE_SLOT_BITS  10u
#define HANDLE_SLOT_MASK  ((1u << HANDLE_SLOT_BITS) - 1u)
#define HANDLE_GEN_SHIFT  HANDLE_SLOT_BITS
#define HANDLE_GEN_MAX    ((1u << (32u - HANDLE_SLOT_BITS)) - 1u)

static inline uint32_t handle_id_slot(handle_id_t id) {
    return (uint32_t)(id & HANDLE_SLOT_MASK);
}
static inline uint32_t handle_id_gen(handle_id_t id) {
    return (uint32_t)(id >> HANDLE_GEN_SHIFT);
}
static inline handle_id_t handle_id_make(uint32_t slot, uint32_t gen) {
    /* generation 0 is forbidden — wrap skips to 1 */
    if (gen == 0u) gen = 1u;
    return (handle_id_t)((gen << HANDLE_GEN_SHIFT) | (slot & HANDLE_SLOT_MASK));
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

void handle_entry_init(struct HandleEntry *e, struct KObject *obj,
                       iris_rights_t rights, uint32_t gen);
void handle_entry_reset(struct HandleEntry *e);
#endif /* __KERNEL__ */

#endif /* IRIS_NC_HANDLE_H */
