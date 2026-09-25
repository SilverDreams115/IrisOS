/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_NC_CPTR_H
#define IRIS_NC_CPTR_H

#include <stdint.h>

/*
 * cptr.h — what a capability argument IS, and the boundary that says so.
 *
 * This file was `nc/handle.h` until Stage 10-abi, and everything in it was
 * named after a namespace that stopped existing in Stage 4.  The rename is the
 * point of that stage rather than tidiness: a 1.0 ABI that ships a type called
 * `iris_cptr_t` is telling every future caller that handles are a thing, and
 * the first question anybody asks about a name is answered wrongly before they
 * read a line of documentation.
 *
 * ── One namespace ──────────────────────────────────────────────────────────
 *
 * A syscall argument that names authority is a CPtr or it is
 * IRIS_ERR_INVALID_ARG.  There is no second table, no generation counter and
 * no token: Stage 4 deleted the handle table and Stage 7-proc deleted the last
 * structure that held one.
 *
 * ── ...and the boundary that keeps it one ──────────────────────────────────
 *
 * Handles used to be `slot | gen << 10` — every value from 1024 upward — so
 * the two namespaces were told apart by MAGNITUDE.  That is why the boundary
 * survives the namespace: values at or above IRIS_CPTR_LIMIT are exactly the
 * bit patterns a caller written against the old ABI would send, and the kernel
 * must REFUSE them rather than resolve them.  Resolving one would silently
 * hand that caller a slot it did not mean to name.
 *
 * `cspace_value_is_cptr()` is that refusal and this constant is what it tests.
 * A CPtr owns the whole low 31 bits and nothing above them.
 */

/*
 * Repeated identical typedefs are legal in C11 and both headers that need this
 * one declare it, so neither has to include the other: `nc/cspace.h` is the
 * kernel's CSpace header and has no business in a ring-3 build, and this file
 * is what ring 3 includes.
 */
typedef uint64_t iris_cptr_t;

/* The null capability.  Slot 0 is the null slot of every CNode by kernel
 * convention and is never populated; `cspace_resolve_cap` rejects it before
 * any traversal. */
#define IRIS_CPTR_NULL  ((iris_cptr_t)0u)

/* One past the largest value that can be a CPtr. */
#define IRIS_CPTR_LIMIT (1u << 31)

/*
 * A value shaped like the thing that is no longer accepted.
 *
 * Tests need to construct one on purpose — the boundary is only proven by
 * something crossing it — and they should not open-code the old packing, both
 * because it would outlive this file and because "1 << 31 | something" at a
 * call site reads as a magic number rather than as the ABI's past.
 */
static inline iris_cptr_t iris_cptr_beyond_limit(uint32_t slot, uint32_t gen) {
    if (gen == 0u) gen = 1u;                       /* generation 0 never existed */
    return (iris_cptr_t)(IRIS_CPTR_LIMIT |
                         ((gen & ((1u << 21) - 1u)) << 10) |
                         (slot & ((1u << 10) - 1u)));
}

#endif /* IRIS_NC_CPTR_H */
