#include <iris/nc/kvspace.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kpagetable.h>
#include <iris/paging.h>
#include <iris/kslab.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kprocess.h>   /* KPROCESS_BOOTSTRAP_FRAME_MAX — boot arena bound */
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

/*
 * Everything a dying VSpace owes back, in the one order that works.
 *
 * Shared by both destructors because it is the same debt either way — only
 * where the HEADER came from differs, and that is all each destructor adds.
 *
 * The order is not arbitrary.  Sweep the mappings first (they hold frame
 * refs and PTEs), then return the mapping-record nodes, then the page
 * children — the PML4 and every table carved under it.  The pool's own retain
 * goes LAST and is left to the caller, because a header block that lives
 * inside the pool's region is what tells the caller who the parent was:
 * releasing the pool first could destroy the region the block is standing in.
 */
static void kvspace_settle(struct KVSpace *vs, struct KUntyped *pool) {
    uint64_t              cr3 = vs->cr3;
    struct KFrameMapping *m   = vs->mappings;
    vs->mappings = 0;

    /* kvspace_invalidate() should have swept this already; the loop is a
     * safety net for unusual teardown paths (e.g. direct CNode slot deletion
     * without prior process teardown). */
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

    /* Stage 6-pure Etapa 1: tables the HOLDER retyped go back as capabilities.
     * Releasing our reference is the whole of it — the region returns to its
     * Untyped when the last capability to the table goes, exactly like any
     * other retyped object, and the holder can then RESET.  The PTEs that
     * referenced them are torn down by paging_destroy_user_space_from. */
    {
        struct KPageTable *t = vs->tables;
        vs->tables = 0;
        while (t) {
            struct KPageTable *next = t->next;
            t->next      = 0;
            t->mapped_vs = 0;
            t->level     = KPT_LEVEL_UNMAPPED;
            kobject_release(&t->base);
            t = next;
        }
    }

    /* Stage 6 Etapa 2: tables the KERNEL charged are not individually freed —
     * the pages stay where they are, because a bump allocator does not rewind.
     * What goes back is the pool's child_count, which is what lets its holder
     * RESET and reuse the whole region: the same reclamation path every
     * retyped object has. */
    while (pool && vs->pt_count) {
        kuntyped_release_page_child(pool);
        vs->pt_count--;
    }
    vs->pt_pool = 0;
    atomic_fetch_sub_explicit(&kvspace_live, 1u, memory_order_relaxed);
}

/* One destructor for both births: kobject_storage_free returns the header to
 * wherever it came from — the kernel slab for the root task's address space,
 * an Untyped child block for every other. */
static void kvspace_obj_destroy(struct KObject *obj) {
    struct KVSpace  *vs   = (struct KVSpace *)obj;
    struct KUntyped *pool = vs->pt_pool;

    kvspace_settle(vs, pool);
    kobject_storage_free(obj, (uint32_t)sizeof(struct KVSpace), 0);
    if (pool) kobject_release(&pool->base);
}

static const struct KObjectOps kvspace_ops = {
    .close   = kvspace_obj_close,
    .destroy = kvspace_obj_destroy,
};

struct KVSpace *kvspace_alloc_at(void *mem, uint64_t cr3) {
    if (!mem) return 0;
    struct KVSpace *vs = (struct KVSpace *)mem;   /* block arrives zeroed */
    kobject_init_in_untyped(&vs->base, KOBJ_VSPACE, &kvspace_ops,
                            (uint32_t)sizeof(struct KVSpace));
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
 * does: the root task's.  It keeps the kernel slab out of the mapping path
 * entirely rather than moving that use from one file to another, and
 * exhaustion fails the map rather than silently spending kernel memory.
 *
 * SIZE.  Not "a handful of pages": the root task maps far more than its own
 * image, because svc_loader maps every image it loads into the loader's OWN
 * address space to parse it — the raw ELF at SL_ELF_VADDR plus each PT_LOAD
 * segment, all live at once — before mapping the segments into the child.  So
 * the peak is its bootstrap frames (bounded by KPROCESS_BOOTSTRAP_FRAME_MAX)
 * plus the pages of the largest image it will ever load, twice over.  The
 * bound below is that sum with room for an image an order of magnitude larger
 * than today's init, because the failure mode is a boot that dies with
 * IRIS_ERR_NO_MEMORY from a map three call levels down.
 *
 * The array is 24 bytes a node, so being generous here is cheaper than being
 * wrong.  Only the root task draws from it; every other address space carries
 * a budget (Stage 6 Etapa 2) and cannot reach this code.
 */
#define KVSPACE_BOOT_NODES 512u
_Static_assert(KVSPACE_BOOT_NODES > KPROCESS_BOOTSTRAP_FRAME_MAX,
               "boot arena must outlast the root task's own bootstrap frames");
static struct KFrameMapping kvspace_boot_nodes[KVSPACE_BOOT_NODES];
static uint32_t             kvspace_boot_next;
static struct KFrameMapping *kvspace_boot_free;
/* The arena is global state reached without any VSpace lock (kframe_map_page
 * allocates before it takes vs->lock), so it carries its own. */
static irq_spinlock_t       kvspace_boot_lock;

/* Does this node live in the bootstrap arena?
 *
 * Asked of the NODE, never of the VSpace.  Where a node came from is fixed the
 * moment it is handed out, but vs->pt_pool is a field that can be set after
 * the fact — keying the free path off it meant a node carved from the arena
 * before a pool was attached would be returned to the pool instead, and
 * kuntyped_release_child would then read a parent pointer out of whatever sat
 * before it in this array and release that.  A pointer's range cannot drift. */
static int kvspace_node_is_boot(const struct KFrameMapping *m) {
    return m >= &kvspace_boot_nodes[0] &&
           m <  &kvspace_boot_nodes[KVSPACE_BOOT_NODES];
}

static struct KFrameMapping *kvspace_boot_node_alloc(void) {
    uint64_t f = irq_spinlock_lock(&kvspace_boot_lock);
    struct KFrameMapping *m = kvspace_boot_free;
    if (m) {
        kvspace_boot_free = m->next;
    } else if (kvspace_boot_next < KVSPACE_BOOT_NODES) {
        m = &kvspace_boot_nodes[kvspace_boot_next++];
    }
    irq_spinlock_unlock(&kvspace_boot_lock, f);
    if (m) { m->next = 0; m->frame = 0; m->user_va = 0; }
    return m;
}

static void kvspace_boot_node_free(struct KFrameMapping *m) {
    uint64_t f = irq_spinlock_lock(&kvspace_boot_lock);
    m->frame = 0;
    m->next  = kvspace_boot_free;
    kvspace_boot_free = m;
    irq_spinlock_unlock(&kvspace_boot_lock, f);
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
    if (kvspace_node_is_boot(m)) { kvspace_boot_node_free(m); return; }
    spinlock_lock(&vs->lock);
    m->frame = 0;
    m->next  = vs->free_nodes;
    vs->free_nodes = m;
    spinlock_unlock(&vs->lock);
}

/* Return every carved node to the Untyped.  Called from the destructor, after
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

iris_error_t kvspace_map_table(struct KVSpace *vs, struct KPageTable *pt,
                               uint64_t vaddr) {
    if (!vs || !pt) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&vs->lock);
    if (!vs->valid || !vs->cr3) {
        spinlock_unlock(&vs->lock);
        return IRIS_ERR_BAD_HANDLE;
    }
    /* A table is installed at most once: the same region appearing twice in a
     * walk would make one unmap strand the other. */
    if (pt->mapped_vs) {
        spinlock_unlock(&vs->lock);
        return IRIS_ERR_ALREADY_EXISTS;
    }

    /* Intermediate levels are always present+writable+user: the leaf PTE is
     * what carries the real permissions, and a level that refused USER would
     * make every mapping under it unreachable from ring 3. */
    int level = paging_install_table_in(vs->cr3, vaddr, pt->paddr,
                                        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    if (level < 0) {
        spinlock_unlock(&vs->lock);
        return IRIS_ERR_INVALID_ARG;      /* huge-page leaf, or bad address */
    }
    if (level == 0) {
        spinlock_unlock(&vs->lock);
        return IRIS_ERR_ALREADY_EXISTS;   /* the walk was already complete */
    }

    kobject_retain(&pt->base);            /* the VSpace holds it while installed */
    pt->mapped_vs = vs;
    pt->mapped_va = vaddr;
    pt->level     = (uint32_t)level;
    pt->next      = vs->tables;
    vs->tables    = pt;
    spinlock_unlock(&vs->lock);
    return IRIS_OK;
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
