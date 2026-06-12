#include <iris/nc/kvspace.h>
#include <iris/nc/kframe.h>
#include <iris/paging.h>
#include <iris/kslab.h>
#include <stdatomic.h>

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
        kslab_free(m, (uint32_t)sizeof(*m));
        atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
        kobject_release(&f->base);
        m = next;
    }
    kslab_free(vs, (uint32_t)sizeof(struct KVSpace));
}

static const struct KObjectOps kvspace_ops = {
    .close   = kvspace_obj_close,
    .destroy = kvspace_obj_destroy,
};

struct KVSpace *kvspace_alloc(uint64_t cr3) {
    struct KVSpace *vs = kslab_alloc((uint32_t)sizeof(struct KVSpace));
    if (!vs) return 0;
    kobject_init(&vs->base, KOBJ_VSPACE, &kvspace_ops);
    spinlock_init(&vs->lock);
    vs->cr3           = cr3;
    vs->valid         = 1;
    vs->mapping_count = 0;
    vs->mappings      = 0;
    return vs;
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
        kslab_free(m, (uint32_t)sizeof(*m));
        atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
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

    kslab_free(m, (uint32_t)sizeof(*m));
    atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
    kobject_release(&f->base);
    return IRIS_OK;
}

void kvspace_free(struct KVSpace *vs) {
    if (!vs) return;
    kobject_release(&vs->base);
}
