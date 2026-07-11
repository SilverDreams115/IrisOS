#include <iris/nc/kuntyped.h>
#include <iris/nc/kobject.h>
#include <iris/kslab.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kuntyped_live;

/* Fase 18 — live KUntyped object count (additive diagnostics).  Exposed via
 * the SYS_SCHED_INFO ext3 tier so authority tests can prove untyped objects
 * (including RETYPE sub-untypeds) are destroyed, not leaked, after churn. */
uint32_t kuntyped_live_count(void) {
    return atomic_load_explicit(&kuntyped_live, memory_order_relaxed);
}

static void kuntyped_obj_close(struct KObject *obj) {
    (void)obj;
    /* No tasks to wake.  Physical region is NOT freed on close — seL4 semantics:
     * untyped memory is reclaimable only after all children are destroyed and
     * SYS_UNTYPED_RESET is called. */
}

static void kuntyped_obj_destroy(struct KObject *obj) {
    struct KUntyped *u      = (struct KUntyped *)obj;
    struct KUntyped *parent = u->alloc_parent;

    atomic_fetch_sub_explicit(&kuntyped_live, 1u, memory_order_relaxed);
    kslab_free(u, (uint32_t)sizeof(struct KUntyped));

    /* Ph80: if this KUntyped was itself a RETYPE child, notify the parent. */
    if (parent) {
        atomic_fetch_sub_explicit(&parent->child_count, 1u, memory_order_relaxed);
        kobject_release(&parent->base);
    }
}

static const struct KObjectOps kuntyped_ops = {
    .close   = kuntyped_obj_close,
    .destroy = kuntyped_obj_destroy,
};

struct KUntyped *kuntyped_create(uint64_t phys_base, uint64_t size, int is_device) {
    if (!size) return 0;

    struct KUntyped *u = kslab_alloc((uint32_t)sizeof(struct KUntyped));
    if (!u) return 0;

    kobject_init(&u->base, KOBJ_UNTYPED, &kuntyped_ops);
    irq_spinlock_init(&u->lock);
    u->phys_base   = phys_base;
    u->total_size  = size;
    u->used        = 0;
    u->is_device   = is_device;
    u->alloc_parent = 0;
    atomic_store_explicit(&u->child_count, 0u, memory_order_relaxed);

    atomic_fetch_add_explicit(&kuntyped_live, 1u, memory_order_relaxed);
    return u;
}

void kuntyped_destroy_ref(struct KUntyped *u) {
    if (!u) return;
    kobject_release(&u->base);
}

/* Advance bump pointer by aligned(bytes); return byte offset or (uint64_t)-1. */
static uint64_t kuntyped_bump(struct KUntyped *u, uint64_t bytes) {
    uint64_t aligned = (bytes + KUNTYPED_ALIGN - 1u) & ~(uint64_t)(KUNTYPED_ALIGN - 1u);
    uint64_t flags   = irq_spinlock_lock(&u->lock);
    if (u->used + aligned > u->total_size) {
        irq_spinlock_unlock(&u->lock, flags);
        return (uint64_t)-1;
    }
    uint64_t offset = u->used;
    u->used += aligned;
    irq_spinlock_unlock(&u->lock, flags);
    return offset;
}

void *kuntyped_bump_alloc(struct KUntyped *u, uint64_t bytes) {
    if (!u || !bytes) return 0;

    uint64_t offset  = kuntyped_bump(u, bytes);
    if (offset == (uint64_t)-1) return 0;

    uint64_t aligned = (bytes + KUNTYPED_ALIGN - 1u) & ~(uint64_t)(KUNTYPED_ALIGN - 1u);
    uint8_t *virt    = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(u->phys_base + offset);

    if (!u->is_device) {
        for (uint64_t i = 0; i < aligned; i++) virt[i] = 0;
    }
    return (void *)virt;
}

uint64_t kuntyped_bump_alloc_phys(struct KUntyped *u, uint64_t bytes) {
    if (!u || !bytes) return 0;
    uint64_t offset = kuntyped_bump(u, bytes);
    if (offset == (uint64_t)-1) return 0;
    /* No zero-fill: sub-untyped zeroes lazily per child allocation. */
    return u->phys_base + offset;
}

/*
 * Ph79: kuntyped_alloc_child
 *
 * Layout in the untyped region for each typed child:
 *   [0 .. KUNTYPED_ALIGN):  parent pointer (first 8 bytes) + padding
 *   [KUNTYPED_ALIGN .. KUNTYPED_ALIGN + align_up(obj_bytes)):  the object
 *
 * The entire block is zero-filled by kuntyped_bump_alloc before we write
 * the parent pointer.
 */
void *kuntyped_alloc_child(struct KUntyped *u, uint64_t obj_bytes) {
    if (!u || !obj_bytes) return 0;
    uint64_t total = KUNTYPED_ALIGN + obj_bytes;
    uint8_t *block = (uint8_t *)kuntyped_bump_alloc(u, total);
    if (!block) return 0;

    /* Store parent back-pointer in first 8 bytes of the header. */
    *(struct KUntyped **)block = u;
    kobject_retain(&u->base);
    atomic_fetch_add_explicit(&u->child_count, 1u, memory_order_relaxed);

    return block + KUNTYPED_ALIGN;
}

void kuntyped_release_child(void *obj_ptr, uint64_t obj_bytes) {
    if (!obj_ptr) return;
    uint8_t *block  = (uint8_t *)obj_ptr - KUNTYPED_ALIGN;
    struct KUntyped *parent = *(struct KUntyped **)block;

    /* Zero the entire block (header + object area). */
    uint64_t aligned_obj = (obj_bytes + KUNTYPED_ALIGN - 1u) & ~(uint64_t)(KUNTYPED_ALIGN - 1u);
    uint64_t total       = KUNTYPED_ALIGN + aligned_obj;
    for (uint64_t i = 0; i < total; i++) block[i] = 0;

    if (parent) {
        atomic_fetch_sub_explicit(&parent->child_count, 1u, memory_order_relaxed);
        kobject_release(&parent->base);
    }
}

uint64_t kuntyped_available(struct KUntyped *u) {
    if (!u) return 0;
    uint64_t flags = irq_spinlock_lock(&u->lock);
    uint64_t avail = u->total_size - u->used;
    irq_spinlock_unlock(&u->lock, flags);
    return avail;
}

uint64_t kuntyped_bump_alloc_phys_page(struct KUntyped *u, uint64_t size) {
    /* size must be non-zero and PAGE_SIZE-aligned. */
    if (!u || !size || (size & 0xFFFULL)) return 0;

    uint64_t irqfl = irq_spinlock_lock(&u->lock);
    /* Round current bump pointer up to the next PAGE_SIZE boundary. */
    uint64_t aligned_start = (u->used + 0xFFFULL) & ~0xFFFULL;
    if (aligned_start + size > u->total_size) {
        irq_spinlock_unlock(&u->lock, irqfl);
        return 0;
    }
    u->used = aligned_start + size;
    uint64_t phys = u->phys_base + aligned_start;
    irq_spinlock_unlock(&u->lock, irqfl);
    return phys;
}
