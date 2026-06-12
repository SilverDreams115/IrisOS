#include <iris/nc/kframe.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kvmo.h>
#include <iris/kslab.h>
#include <iris/paging.h>
#include <iris/panic.h>
#include <stddef.h>

static void kframe_obj_close(struct KObject *obj) {
    (void)obj;
    /* No tasks to wake — KFrame has no blocked waiters. */
}

static void kframe_obj_destroy(struct KObject *obj) {
    struct KFrame   *f      = (struct KFrame *)obj;
    struct KUntyped *parent = f->alloc_parent;
    struct KVmo     *vmo    = f->vmo_owner;

    IRIS_ASSERT(
        atomic_load_explicit(&f->mapped_count, memory_order_relaxed) == 0,
        "kframe: destroy with active mappings — caller must unmap before release");

    kslab_free(f, (uint32_t)sizeof(struct KFrame));

    if (parent) {
        atomic_fetch_sub_explicit(&parent->child_count, 1u, memory_order_relaxed);
        kobject_release(&parent->base);
    }
    if (vmo) {
        /* Release the VMO retain held since kframe_alloc_vmo_page.
         * If this was the last retain, kvmo_destroy runs and frees the physical page.
         * Ordering is safe: mapped_count was 0 before kslab_free, so no PTE
         * can reference this physical address at this point. */
        kobject_release(&vmo->base);
    }
}

static const struct KObjectOps kframe_ops = {
    .close   = kframe_obj_close,
    .destroy = kframe_obj_destroy,
};

struct KFrame *kframe_alloc(uint64_t paddr, uint64_t size,
                             struct KUntyped *alloc_parent) {
    if (!size || (size & 0xFFFULL)) return 0;

    struct KFrame *f = kslab_alloc((uint32_t)sizeof(struct KFrame));
    if (!f) return 0;

    kobject_init(&f->base, KOBJ_FRAME, &kframe_ops);
    f->paddr        = paddr;
    f->size         = size;
    f->alloc_parent = alloc_parent;
    f->vmo_owner    = NULL;
    atomic_store_explicit(&f->mapped_count, 0u, memory_order_relaxed);

    if (alloc_parent) {
        kobject_retain(&alloc_parent->base);
        atomic_fetch_add_explicit(&alloc_parent->child_count, 1u, memory_order_relaxed);
    }
    return f;
}

struct KFrame *kframe_alloc_vmo_page(uint64_t paddr, struct KVmo *vmo) {
    if (!vmo) return NULL;
    struct KFrame *f = kframe_alloc(paddr, 4096u, NULL);
    if (!f) return NULL;
    kobject_retain(&vmo->base);
    f->vmo_owner = vmo;
    return f;
}

iris_error_t kframe_map_page(struct KFrame *f, struct KVSpace *vs,
                              uint64_t user_va, uint64_t map_flags)
{
    struct KFrameMapping *m;
    uint64_t page_flags;
    int r, writable, executable;

    if (!f || !vs) return IRIS_ERR_INVALID_ARG;
    if (!kframe_va_valid(user_va)) return IRIS_ERR_INVALID_ARG;
    if (map_flags & ~3ULL) return IRIS_ERR_INVALID_ARG;

    writable   = (int)(map_flags & 1u);
    executable = (int)((map_flags >> 1) & 1u);
    if (writable && executable) return IRIS_ERR_INVALID_ARG;

    /* Allocate the mapping record before acquiring the lock so that the
     * slab allocator path (which may sleep in future SMP builds) does not
     * hold vs->lock.  On failure the caller sees NO_MEMORY before any PTE
     * or list state is touched. */
    m = kslab_alloc((uint32_t)sizeof(*m));
    if (!m) return IRIS_ERR_NO_MEMORY;

    spinlock_lock(&vs->lock);

    if (!vs->valid || !vs->cr3) {
        spinlock_unlock(&vs->lock);
        kslab_free(m, (uint32_t)sizeof(*m));
        return IRIS_ERR_BAD_HANDLE;
    }

    if (paging_virt_to_phys_in(vs->cr3, user_va) != 0) {
        spinlock_unlock(&vs->lock);
        kslab_free(m, (uint32_t)sizeof(*m));
        return IRIS_ERR_BUSY;
    }

    page_flags = PAGE_PRESENT | PAGE_USER;
    if (!executable) page_flags |= PAGE_NX;
    if (writable)    page_flags |= PAGE_WRITABLE;

    r = paging_map_checked_in(vs->cr3, user_va, f->paddr, page_flags);
    if (r != 0) {
        spinlock_unlock(&vs->lock);
        kslab_free(m, (uint32_t)sizeof(*m));
        return IRIS_ERR_NO_MEMORY;
    }

    /* Retain the frame for the lifetime of this mapping record. */
    kobject_retain(&f->base);
    m->frame   = f;
    m->user_va = user_va;
    m->next    = vs->mappings;
    vs->mappings = m;
    vs->mapping_count++;

    spinlock_unlock(&vs->lock);

    atomic_fetch_add_explicit(&f->mapped_count, 1u, memory_order_relaxed);
    return IRIS_OK;
}

struct KFrame *bootstrap_kframe_map(struct KVSpace *vs,
                                     uint64_t       paddr,
                                     uint64_t       user_va,
                                     uint64_t       map_flags)
{
    struct KFrame *f = kframe_alloc(paddr, 4096u, NULL);
    if (!f) return NULL;
    iris_error_t r = kframe_map_page(f, vs, user_va, map_flags);
    if (r != IRIS_OK) {
        kobject_release(&f->base);
        return NULL;
    }
    return f;
}

iris_error_t kframe_unmap_page(struct KFrame *f, struct KVSpace *vs,
                                uint64_t user_va)
{
    struct KFrameMapping **link;
    struct KFrameMapping  *m;

    if (!f || !vs) return IRIS_ERR_INVALID_ARG;
    if (!kframe_va_valid(user_va)) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&vs->lock);

    if (!vs->valid || !vs->cr3) {
        spinlock_unlock(&vs->lock);
        return IRIS_ERR_BAD_HANDLE;
    }

    /* Search list by frame pointer AND VA.  If VA matches but frame differs,
     * the VA is occupied by a different frame — report INVALID_ARG. */
    m    = NULL;
    link = &vs->mappings;
    while (*link) {
        if ((*link)->user_va == user_va) {
            if ((*link)->frame != f) {
                spinlock_unlock(&vs->lock);
                return IRIS_ERR_INVALID_ARG;
            }
            m     = *link;
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

    paging_unmap_in(vs->cr3, user_va);
    spinlock_unlock(&vs->lock);

    kslab_free(m, (uint32_t)sizeof(*m));
    /* Decrement mapped_count BEFORE kobject_release so that kframe_obj_destroy
     * always observes mapped_count == 0 when the mapping retain is the last one. */
    atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
    kobject_release(&f->base);
    return IRIS_OK;
}
