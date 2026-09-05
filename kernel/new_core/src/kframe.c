#include <iris/nc/kframe.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kvmo.h>
#include <iris/kslab.h>
#include <iris/paging.h>
#include <iris/panic.h>
#include <stdatomic.h>
#include <stddef.h>

/* Phase 18 — live KFrame object count (additive diagnostics).  Incremented on
 * every kframe_alloc, decremented in kframe_obj_destroy, so authority tests can
 * prove a retyped/mapped frame is destroyed exactly once after unmap+release. */
static _Atomic uint32_t kframe_live;

uint32_t kframe_live_count(void) {
    return atomic_load_explicit(&kframe_live, memory_order_relaxed);
}

/* Phase 19 — mapping instrumentation (additive, exposed via SYS_SCHED_INFO ext4).
 *   kframe_live_mappings — KFrameMapping nodes currently installed across every
 *     VSpace.  Bumped on a successful map, dropped on every removal path
 *     (explicit unmap, VSpace invalidate, VSpace destroy) so it returns to
 *     baseline after teardown — the observable behind V13/V15/V16.
 *   kframe_map_ok / kframe_unmap_ok — monotonic success counters (explicit
 *     map / explicit unmap), for progress and V10/V12 sanity.
 * None of this changes mapping behaviour. */
static _Atomic uint32_t kframe_live_mappings;
static _Atomic uint32_t kframe_map_ok;
static _Atomic uint32_t kframe_unmap_ok;

uint32_t kframe_live_mapping_count(void) {
    return atomic_load_explicit(&kframe_live_mappings, memory_order_relaxed);
}
uint32_t kframe_map_success_count(void) {
    return atomic_load_explicit(&kframe_map_ok, memory_order_relaxed);
}
uint32_t kframe_unmap_success_count(void) {
    return atomic_load_explicit(&kframe_unmap_ok, memory_order_relaxed);
}

/* A mapping was installed (kframe_map_page success). */
void kframe_stat_map(void) {
    atomic_fetch_add_explicit(&kframe_map_ok, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&kframe_live_mappings, 1u, memory_order_relaxed);
}
/* An explicit unmap removed a mapping (kframe_unmap_page / kvspace_unmap_page). */
void kframe_stat_unmap(void) {
    atomic_fetch_add_explicit(&kframe_unmap_ok, 1u, memory_order_relaxed);
    atomic_fetch_sub_explicit(&kframe_live_mappings, 1u, memory_order_relaxed);
}
/* A bulk cleanup removed a mapping (VSpace invalidate / destroy). */
void kframe_stat_cleanup(void) {
    atomic_fetch_sub_explicit(&kframe_live_mappings, 1u, memory_order_relaxed);
}

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

    atomic_fetch_sub_explicit(&kframe_live, 1u, memory_order_relaxed);
    /* Read vmo/parent BEFORE this: it returns the storage, which for an
     * Untyped-born frame means zeroing the block this struct lives in. */
    kobject_storage_free(obj, (uint32_t)sizeof(struct KFrame), 0);

    /* Only a slab frame carries alloc_parent; a block-backed one's accounting
     * rides on the block kobject_storage_free just handed back. */
    if (parent) {
        atomic_fetch_sub_explicit(&parent->child_count, 1u, memory_order_relaxed);
        kobject_release(&parent->base);
    }
    if (vmo) {
        /* Release the VMO retain held since kframe_alloc_vmo_page.
         * If this was the last retain, kvmo_destroy runs and frees the physical page.
         * Ordering is safe: mapped_count was 0 before the storage went back, so
         * no PTE can reference this physical address at this point. */
        kobject_release(&vmo->base);
    }
}

static const struct KObjectOps kframe_ops = {
    .close   = kframe_obj_close,
    .destroy = kframe_obj_destroy,
};

/*
 * Stage 6 Step 1 — the Untyped-born frame.
 *
 * The frame's PAGE was always carved from the Untyped; its header was a kslab
 * allocation, so a caller who paid for a page also spent kernel memory that
 * nothing accounted and no capability authorised.  The header is now a child
 * block of the same Untyped, carved from the TOP so that it does not push the
 * page carve onto the next page boundary.
 *
 * It is never inside the frame's own page: that page is mapped into ring 3,
 * and kernel bookkeeping placed there would be readable and writable by the
 * process the frame was handed to.  The two carves come from opposite ends of
 * the region precisely so they cannot meet.
 */
struct KFrame *kframe_alloc_at(void *mem, uint64_t paddr, uint64_t size) {
    if (!mem || !size || (size & 0xFFFULL)) return 0;

    struct KFrame *f = (struct KFrame *)mem;   /* block arrives zeroed */
    kobject_init_in_untyped(&f->base, KOBJ_FRAME, &kframe_ops,
                            (uint32_t)sizeof(struct KFrame));
    f->paddr        = paddr;
    f->size         = size;
    f->alloc_parent = NULL;   /* the header block holds the child accounting */
    f->vmo_owner    = NULL;
    atomic_store_explicit(&f->mapped_count, 0u, memory_order_relaxed);
    atomic_fetch_add_explicit(&kframe_live, 1u, memory_order_relaxed);
    return f;
}

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
    atomic_fetch_add_explicit(&kframe_live, 1u, memory_order_relaxed);

    if (alloc_parent) {
        kobject_retain(&alloc_parent->base);
        atomic_fetch_add_explicit(&alloc_parent->child_count, 1u, memory_order_relaxed);
    }
    return f;
}

struct KFrame *kframe_alloc_vmo_page(uint64_t paddr, struct KVmo *vmo) {
    if (!vmo) return NULL;

    /* Stage 6 Step 6: the header for a VMO page comes out of the VMO's own
     * budget — the same one its page came from.  This is the frequent runtime
     * path (one per mapped page), so leaving it on the kernel slab would mean
     * a process could still grow kernel memory by mapping. */
    /* Always the VMO's pool: a VMO cannot exist without one since
     * `kvmo_alloc_in` stopped accepting NULL, and the slab fallback that used
     * to be here had no reachable caller left. */
    if (!vmo->pool) return NULL;
    void *hdr = kuntyped_alloc_child_top(vmo->pool, sizeof(struct KFrame));
    if (!hdr) return NULL;
    struct KFrame *f = kframe_alloc_at(hdr, paddr, 4096u);
    if (!f) { kuntyped_release_child(hdr, sizeof(struct KFrame)); return NULL; }
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
    m = kvspace_node_alloc(vs);
    if (!m) return IRIS_ERR_NO_MEMORY;

    spinlock_lock(&vs->lock);

    if (!vs->valid || !vs->cr3) {
        spinlock_unlock(&vs->lock);
        kvspace_node_free(vs, m);
        return IRIS_ERR_BAD_HANDLE;
    }

    /*
     * D-10: a map covers the WHOLE frame, and every page of it is checked
     * before any of it is installed.
     *
     * Checking as we go would leave a partial mapping behind when page seven
     * of sixteen turned out to be occupied — the caller would get an error and
     * an address space with six pages of somebody's frame in it.  Two passes
     * cost a walk and buy the property that a failed map changes nothing.
     */
    uint64_t pages = f->size >> 12;
    if (pages == 0) pages = 1;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = user_va + (i << 12);
        if (!kframe_va_valid(va) ||
            paging_virt_to_phys_in(vs->cr3, va) != 0) {
            spinlock_unlock(&vs->lock);
            kvspace_node_free(vs, m);
            return IRIS_ERR_BUSY;
        }
    }

    page_flags = PAGE_PRESENT | PAGE_USER;
    if (!executable) page_flags |= PAGE_NX;
    if (writable)    page_flags |= PAGE_WRITABLE;

    /*
     * Stage 6-pure Step 2: the kernel does not create paging levels.
     *
     * A VSpace whose holder was given a budget (every spawned process) is
     * mapped STRICTLY: a missing level is reported, not carved.  The holder
     * retypes a KOBJ_PAGE_TABLE and installs it with SYS_VSPACE_MAP_TABLE,
     * then retries — which is what makes the level an object it owns rather
     * than a side effect it paid for (ledger D-5).
     *
     * The exception is bounded to the root task's BOOTSTRAP maps — its text,
     * stack and BootInfo, mapped before it exists, with no userland to ask.
     * Those come from the PMM reserve.  Stage 6-pure Step 3 ends the
     * exception the moment the root task can speak for itself
     * (kvspace_end_bootstrap), so it too supplies its own levels from then on
     * and no address space is implicitly funded while anyone is running.
     */
    r = 0;
    uint64_t done = 0;
    for (; done < pages; done++) {
        uint64_t va = user_va + (done << 12);
        uint64_t pa = f->paddr + (done << 12);
        if (!vs->kernel_funded) {
            r = paging_map_strict_in(vs->cr3, va, pa, page_flags);
            if (r > 0) r = -1;            /* a level is missing; name it */
        } else {
            r = paging_map_checked_in(vs->cr3, va, pa, page_flags);
        }
        if (r != 0) break;
    }
    if (r != 0) {
        /* Unwind exactly what went in.  A holder that has to retype a page
         * table and retry must find the address space as it left it, or the
         * retry hits BUSY on its own leftovers. */
        for (uint64_t i = 0; i < done; i++)
            paging_unmap_in(vs->cr3, user_va + (i << 12));
        spinlock_unlock(&vs->lock);
        kvspace_node_free(vs, m);
        return (r > 0 || !vs->kernel_funded) ? IRIS_ERR_MISSING_TABLE
                                             : IRIS_ERR_NO_MEMORY;
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
    kframe_stat_map();
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

/*
 * Remove every PTE of a frame mapped at `base_va`.
 *
 * One mapping record covers a WHOLE frame (ledger D-10), so every teardown
 * path has to walk the frame's pages rather than the record's first one.  A
 * cleanup that removed only the first page would leave the rest of a large
 * frame mapped in an address space being reclaimed — the PTEs would outlive
 * the object that justified them, which is the one thing an unmap exists to
 * prevent.  Shared by the explicit unmap and the three bulk teardowns for
 * exactly that reason: four copies of this loop is three chances to fix it
 * in one place only.
 */
void kframe_unmap_all(uint64_t cr3, const struct KFrame *f, uint64_t base_va) {
    if (!cr3 || !f) return;
    uint64_t pages = f->size >> 12;
    if (pages == 0) pages = 1;
    for (uint64_t i = 0; i < pages; i++)
        paging_unmap_in(cr3, base_va + (i << 12));
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

    kframe_unmap_all(vs->cr3, f, user_va);
    spinlock_unlock(&vs->lock);

    kvspace_node_free(vs, m);
    /* Decrement mapped_count BEFORE kobject_release so that kframe_obj_destroy
     * always observes mapped_count == 0 when the mapping retain is the last one. */
    atomic_fetch_sub_explicit(&f->mapped_count, 1u, memory_order_relaxed);
    kframe_stat_unmap();
    kobject_release(&f->base);
    return IRIS_OK;
}
