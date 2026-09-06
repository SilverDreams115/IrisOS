#ifndef IRIS_NC_KASIDPOOL_H
#define IRIS_NC_KASIDPOOL_H

/*
 * kasidpool.h — address-space identifiers as a CAPABILITY (ledger A-21).
 *
 * An address space needs a hardware tag: on x86-64 a PCID, on ARM an ASID.
 * IRIS allocated one automatically out of a kernel-global bitmap the moment a
 * KVSpace was retyped, which made two things true that a capability system
 * should not allow.  Nobody had to be GRANTED the right to create an address
 * space — the tag arrived with the object.  And the number of address spaces
 * the system could hold was a constant inside the kernel, invisible from ring
 * 3 and answerable to nobody.
 *
 * seL4's arrangement, which this is:
 *
 *   - `ASIDControl` is one capability, handed to the root task in BootInfo.
 *     It authorises carving POOLS, and nothing else.
 *   - an `ASIDPool` is a real object, retyped from an Untyped somebody holds,
 *     that owns a contiguous RANGE of the identifier space.
 *   - `ASIDPool_Assign` gives one identifier from that pool to one address
 *     space.  An address space with no identifier is a page and a header: it
 *     cannot be bound to a thread and nothing can run in it.
 *
 * So "how many address spaces may exist" stops being a kernel constant and
 * becomes "how many pools were carved, and by whom" — a question with an
 * answer in the capability graph.
 */

#include <iris/syscall.h>
#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>

/* x86-64 PCIDs are 12 bits.  0 is the kernel's and 4095 is reserved, so the
 * assignable space is 1..4094 and a pool is a slice of it. */
/* Identifier 1 is the root task's, stamped by boot before any pool exists —
 * the same bootstrap exception its CNode and TCB are. */
#define KASID_BOOTSTRAP    1u
#define KASID_FIRST        2u
#define KASID_LAST      4094u
#define KASID_POOL_SIZE  IRIS_ASID_POOL_SIZE  /* identifiers per pool */
#define KASID_POOL_WORDS   2u   /* KASID_POOL_SIZE / 64 */

struct KAsidPool {
    struct KObject base;
    spinlock_t     lock;
    uint16_t       first;                        /* first identifier owned */
    uint16_t       count;                        /* how many, from `first` */
    uint64_t       used[KASID_POOL_WORDS];       /* bit i = first+i assigned */
};

/* Carve the next unowned range out of the identifier space.  Returns
 * IRIS_ERR_NO_MEMORY when the space is exhausted — which is the honest answer
 * and is now REACHABLE, where the kernel-global bitmap simply refused to make
 * an address space with no way to say why. */
iris_error_t kasidpool_carve_range(uint16_t *out_first, uint16_t *out_count);

/* Build a pool over a range, in memory the caller carved from an Untyped. */
struct KAsidPool *kasidpool_alloc_at(void *mem, uint16_t first, uint16_t count);

/* Take one identifier from the pool.  0 means the pool is full. */
uint16_t kasidpool_take(struct KAsidPool *p);

/* Give one back.  Silently ignores an identifier this pool does not own. */
void     kasidpool_put(struct KAsidPool *p, uint16_t id);

uint32_t kasidpool_live_count(void);

#endif /* IRIS_NC_KASIDPOOL_H */
