#include <iris/nc/kvspace.h>
#include <iris/nc/kframe.h>
#include <iris/paging.h>
#include <iris/kslab.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>

/* Fase 19 — live KVSpace object count (additive diagnostics, SYS_SCHED_INFO
 * ext4 tier).  Lets VM tests prove a child's VSpace is destroyed on process
 * death (V16) and that the count returns to baseline after churn. */
static _Atomic uint32_t kvspace_live;

static void kvspace_release_nodes(struct KVSpace *vs);

uint32_t kvspace_live_count(void) {
    return atomic_load_explicit(&kvspace_live, memory_order_relaxed);
}

static void kvspace_obj_close(struct KObject *obj) {
    (void)obj;
}

/* Called when refcount reaches 0.  kvspace_invalidate() should have swept the
 * mapping list already; this loop is a safety net for unusual teardown paths
 * (e.g. direct CNode slot deletion without prior process teardown). */
static void kvspace_obj_destroy(struct KObject *obj) {
    struct KVSpace       *vs  = (struct KVSpace *)obj;
    uint64_t              cr3 = vs->cr3;
    struct KFrameMapping *m   = vs->mappings;
    vs->mappings = 0;

    while (m) {
        struct KFrameMapping *next = m->next;
        struct KFrame        *f   = m->frame;
        if (cr3) paging_unmap_in(cr3, m->user_va);
        kvspace_node_free(vs, m);
        atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
        kframe_stat_cleanup();
        kobject_release(&f->base);
        m = next;
    }
    /* Stage 6 Etapa 2: drop the page-table budget.  The tables carved from it
     * are not individually freed — they return to the Untyped when it is
     * RESET or destroyed, which is the lifetime every retyped object has. */
    kvspace_release_nodes(vs);
    if (vs->pt_pool) {
        /* Return every page table this address space carved: the pages stay
         * where they are (a bump allocator does not rewind), but the pool's
         * child_count drops to zero so its holder can RESET and reuse the
         * whole region — the same reclamation path every retyped object has. */
        while (vs->pt_count) {
            kuntyped_release_page_child(vs->pt_pool);
            vs->pt_count--;
        }
        kobject_release(&vs->pt_pool->base);
        vs->pt_pool = 0;
    }
    atomic_fetch_sub_explicit(&kvspace_live, 1u, memory_order_relaxed);
    kslab_free(vs, (uint32_t)sizeof(struct KVSpace));
}

static const struct KObjectOps kvspace_ops = {
    .close   = kvspace_obj_close,
    .destroy = kvspace_obj_destroy,
};

/*
 * Stage 6 Etapa 3 — the Untyped-born VSpace.
 *
 * Header, PML4 and every page table below it now come out of one budget the
 * creator named.  The teardown order matters and is not arbitrary: return the
 * page children first (PML4 + tables), then the header block — which drops the
 * retain the block itself holds — and only then the VSpace's own pool retain.
 * Releasing the pool first could destroy the Untyped whose region the header
 * block lives in, and the block is what tells us who the parent was.
 */
static void kvspace_obj_destroy_ut(struct KObject *obj) {
    struct KVSpace       *vs   = (struct KVSpace *)obj;
    struct KUntyped      *pool = vs->pt_pool;
    uint64_t              cr3  = vs->cr3;
    struct KFrameMapping *m    = vs->mappings;
    vs->mappings = 0;

    while (m) {
        struct KFrameMapping *next = m->next;
        struct KFrame        *f    = m->frame;
        if (cr3) paging_unmap_in(cr3, m->user_va);
        kvspace_node_free(vs, m);
        atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
        kframe_stat_cleanup();
        kobject_release(&f->base);
        m = next;
    }

    kvspace_release_nodes(vs);
    while (vs->pt_count && pool) {
        kuntyped_release_page_child(pool);
        vs->pt_count--;
    }
    atomic_fetch_sub_explicit(&kvspace_live, 1u, memory_order_relaxed);
    vs->pt_pool = 0;
    kuntyped_release_child(vs, sizeof(struct KVSpace));
    if (pool) kobject_release(&pool->base);
}

static const struct KObjectOps kvspace_ops_ut = {
    .close   = kvspace_obj_close,
    .destroy = kvspace_obj_destroy_ut,
};

struct KVSpace *kvspace_alloc_at(void *mem, uint64_t cr3) {
    if (!mem) return 0;
    struct KVSpace *vs = (struct KVSpace *)mem;   /* block arrives zeroed */
    kobject_init(&vs->base, KOBJ_VSPACE, &kvspace_ops_ut);
    spinlock_init(&vs->lock);
    vs->cr3           = cr3;
    vs->valid         = 1;
    vs->mapping_count = 0;
    vs->mappings      = 0;
    vs->pt_pool       = 0;
    vs->pt_count      = 0;
    atomic_fetch_add_explicit(&kvspace_live, 1u, memory_order_relaxed);
    return vs;
}

struct KVSpace *kvspace_alloc(uint64_t cr3) {
    struct KVSpace *vs = kslab_alloc((uint32_t)sizeof(struct KVSpace));
    if (!vs) return 0;
    kobject_init(&vs->base, KOBJ_VSPACE, &kvspace_ops);
    spinlock_init(&vs->lock);
    vs->cr3           = cr3;
    vs->valid         = 1;
    vs->mapping_count = 0;
    vs->mappings      = 0;
    vs->pt_pool       = 0;
    vs->pt_count      = 0;
    vs->free_nodes    = 0;
    vs->node_count    = 0;
    atomic_fetch_add_explicit(&kvspace_live, 1u, memory_order_relaxed);
    return vs;
}

/*
 * Stage 6 Etapa 6 — a mapping record comes from the address space's own budget.
 *
 * These are per-mapping kernel bookkeeping: small, numerous, and churning.
 * Carved from the VSpace's Untyped and recycled through a free list, so the
 * budget pays once per CONCURRENT mapping rather than once per map call — the
 * same bounded-by-what-is-alive rule the loader's child budgets follow.
 * A VSpace with no budget (the root task) keeps the kernel slab.
 */
/*
 * Bootstrap arena for the ONE address space that exists before any budget
 * does: the root task's.  It maps its text, its stack and its BootInfo — a
 * handful of pages — so a fixed, bounded array is the honest shape here, and
 * it keeps the kernel slab out of the mapping path entirely rather than moving
 * that use from one file to another.  Exhaustion fails the map; it cannot be
 * reached by anything but the boot path, because every other address space
 * carries a budget.
 */
#define KVSPACE_BOOT_NODES 64u
static struct KFrameMapping kvspace_boot_nodes[KVSPACE_BOOT_NODES];
static uint32_t             kvspace_boot_next;
static struct KFrameMapping *kvspace_boot_free;

static struct KFrameMapping *kvspace_boot_node_alloc(void) {
    if (kvspace_boot_free) {
        struct KFrameMapping *m = kvspace_boot_free;
        kvspace_boot_free = m->next;
        m->next = 0; m->frame = 0; m->user_va = 0;
        return m;
    }
    if (kvspace_boot_next >= KVSPACE_BOOT_NODES) return 0;
    return &kvspace_boot_nodes[kvspace_boot_next++];
}

static void kvspace_boot_node_free(struct KFrameMapping *m) {
    m->frame = 0;
    m->next  = kvspace_boot_free;
    kvspace_boot_free = m;
}

struct KFrameMapping *kvspace_node_alloc(struct KVSpace *vs) {
    if (!vs) return 0;
    if (!vs->pt_pool) return kvspace_boot_node_alloc();

    spinlock_lock(&vs->lock);
    struct KFrameMapping *m = vs->free_nodes;
    if (m) {
        vs->free_nodes = m->next;
        spinlock_unlock(&vs->lock);
        m->next = 0; m->frame = 0; m->user_va = 0;
        return m;
    }
    struct KUntyped *pool = vs->pt_pool;
    spinlock_unlock(&vs->lock);

    m = kuntyped_alloc_child_top(pool, sizeof(struct KFrameMapping));
    if (!m) return 0;
    spinlock_lock(&vs->lock);
    vs->node_count++;
    spinlock_unlock(&vs->lock);
    return m;
}

void kvspace_node_free(struct KVSpace *vs, struct KFrameMapping *m) {
    if (!m) return;
    if (!vs || !vs->pt_pool) {
        kvspace_boot_node_free(m);
        return;
    }
    spinlock_lock(&vs->lock);
    m->frame = 0;
    m->next  = vs->free_nodes;
    vs->free_nodes = m;
    spinlock_unlock(&vs->lock);
}

/* Return every carved node to the Untyped.  Called from the destructors, after
 * the mapping list has been swept, so the free list holds them all. */
static void kvspace_release_nodes(struct KVSpace *vs) {
    struct KFrameMapping *m = vs->free_nodes;
    vs->free_nodes = 0;
    while (m) {
        struct KFrameMapping *next = m->next;
        kuntyped_release_child(m, sizeof(struct KFrameMapping));
        vs->node_count--;
        m = next;
    }
}

void kvspace_set_pt_pool(struct KVSpace *vs, struct KUntyped *pool) {
    if (!vs || !pool || vs->pt_pool) return;
    kobject_retain(&pool->base);
    vs->pt_pool = pool;
}

/* Zero cr3/valid, grab the entire mapping list, then process it outside the
 * lock.  Since valid=0 prevents new kframe_map_page calls from succeeding,
 * no new nodes can appear after the lock is released. */
void kvspace_invalidate(struct KVSpace *vs) {
    struct KFrameMapping *list;
    uint64_t              saved_cr3;

    if (!vs) return;
    spinlock_lock(&vs->lock);
    saved_cr3    = vs->cr3;
    vs->valid    = 0;
    vs->cr3      = 0;
    list         = vs->mappings;
    vs->mappings = 0;
    vs->mapping_count = 0;
    spinlock_unlock(&vs->lock);

    while (list) {
        struct KFrameMapping *m = list;
        struct KFrame        *f = m->frame;
        list = m->next;
        if (saved_cr3) paging_unmap_in(saved_cr3, m->user_va);
        kvspace_node_free(vs, m);
        atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
        kframe_stat_cleanup();
        kobject_release(&f->base);
    }
}

/* Find the node for user_va, remove it, unmap the PTE, and release the
 * frame retain.  Returns IRIS_ERR_NOT_FOUND if user_va is not mapped. */
iris_error_t kvspace_unmap_page(struct KVSpace *vs, uint64_t user_va) {
    struct KFrameMapping **link;
    struct KFrameMapping  *m;
    struct KFrame         *f;
    uint64_t               cr3;

    if (!vs) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&vs->lock);
    if (!vs->valid || !vs->cr3) {
        spinlock_unlock(&vs->lock);
        return IRIS_ERR_BAD_HANDLE;
    }
    cr3  = vs->cr3;
    m    = 0;
    link = &vs->mappings;
    while (*link) {
        if ((*link)->user_va == user_va) {
            m    = *link;
            *link = m->next;
            vs->mapping_count--;
            break;
        }
        link = &(*link)->next;
    }
    if (!m) {
        spinlock_unlock(&vs->lock);
        return IRIS_ERR_NOT_FOUND;
    }
    f = m->frame;
    paging_unmap_in(cr3, user_va);
    spinlock_unlock(&vs->lock);

    kvspace_node_free(vs, m);
    atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
    kframe_stat_unmap();
    kobject_release(&f->base);
    return IRIS_OK;
}

void kvspace_free(struct KVSpace *vs) {
    if (!vs) return;
    kobject_release(&vs->base);
}
