#ifndef IRIS_NC_HANDLE_H
#define IRIS_NC_HANDLE_H

#include <stdint.h>

/*
 * The reserved top of the capability-argument value space.
 *
 * There is exactly ONE authority namespace: a syscall argument is a CPtr or it
 * is IRIS_ERR_INVALID_ARG.  Stage 4 deleted the handle table, and Stage 7-proc
 * deleted the last structure that held one; `struct HandleEntry` and its
 * helpers went with the kernel object they lived in.
 *
 * What survives is the BOUNDARY, and it survives on purpose.  Handles used to
 * be `slot | gen << 10` — every value from 1024 upward — so the two namespaces
 * were told apart by magnitude.  Values at or above HANDLE_TAG are therefore
 * the bit patterns an old caller would send, and the kernel must reject them
 * rather than resolve them: cspace_value_is_cptr() is that rejection, and it
 * is the reason this file still exists.  A CPtr owns the whole low 31 bits.
 *
 * handle_id_t and the packing helpers remain because the userland ABI headers
 * still name the type in a few protocol structs.  Nothing in the kernel
 * produces a handle id; nothing consumes one.
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

#endif /* IRIS_NC_HANDLE_H */
