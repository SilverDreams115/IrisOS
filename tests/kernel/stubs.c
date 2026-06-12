/* stubs.c — host-side implementations of kernel allocator and globals
 * used by the unit-test build.  Never linked into the real kernel. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ── kpage stub (legacy; still used by kpage.c itself if compiled) ───────── */
#define STUB_HDR 16u

int iris_smap_enabled = 0;
int iris_pcid_enabled = 0;

/* ── Failure injection state (Fase 6.4) — declared early, used by kslab/paging stubs ── */
static int g_kslab_fail_countdown = -1; /* -1=never; 0=fail next; N=fail after N more successes */
static int g_paging_force_fail    =  0; /* non-zero = fail next paging_map_checked_in */

void *kpage_alloc(uint32_t size) {
    if (!size) return NULL;
    uint8_t *p = (uint8_t *)calloc(1, (size_t)(size + STUB_HDR));
    if (!p) return NULL;
    *((uint32_t *)p) = size;
    return p + STUB_HDR;
}

void kpage_free(void *ptr, uint32_t size) {
    if (!ptr) return;
    uint8_t *base = (uint8_t *)ptr - STUB_HDR;
    uint32_t stored = *((uint32_t *)base);
    if (stored != size) {
        fprintf(stderr, "[KPAGE STUB] size mismatch: stored=%u freed=%u\n",
                stored, size);
        abort();
    }
    free(base);
}

/* ── kslab stub — used by all kobject alloc/free paths in unit tests ─────── */
void *kslab_alloc(uint32_t size) {
    if (!size) return NULL;
    /* Failure injection: simulate slab OOM at a controlled call site. */
    if (g_kslab_fail_countdown == 0) { return NULL; }
    if (g_kslab_fail_countdown  > 0) { g_kslab_fail_countdown--; }
    uint8_t *p = (uint8_t *)calloc(1, (size_t)(size + STUB_HDR));
    if (!p) return NULL;
    *((uint32_t *)p) = size;
    return p + STUB_HDR;
}

void kslab_free(void *ptr, uint32_t size) {
    if (!ptr) return;
    uint8_t *base = (uint8_t *)ptr - STUB_HDR;
    uint32_t stored = *((uint32_t *)base);
    if (stored != size) {
        fprintf(stderr, "[KSLAB STUB] size mismatch: stored=%u freed=%u\n",
                stored, size);
        abort();
    }
    free(base);
}

/* ── task / scheduler stubs ─────────────────────────────────────────────── */
#include <iris/task.h>
void         task_wakeup(struct task *t) { (void)t; }
struct task *task_current(void)          { return NULL; }
void         task_yield(void)            { }

/* ── kprocess quota stubs (needed when compiling kchannel.c) ─────────────── */
#include <iris/nc/kprocess.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kobject.h>
iris_error_t kprocess_quota_acquire_channel(struct KProcess *p)    { (void)p; return IRIS_OK; }
void         kprocess_quota_release_channel(struct KProcess *p)    { (void)p; }
iris_error_t kprocess_quota_acquire_notification(struct KProcess *p){ (void)p; return IRIS_OK; }
void         kprocess_quota_release_notification(struct KProcess *p){ (void)p; }
iris_error_t kprocess_quota_acquire_vmo(struct KProcess *p)        { (void)p; return IRIS_OK; }
void         kprocess_quota_release_vmo(struct KProcess *p)        { (void)p; }
iris_error_t kprocess_quota_acquire_page(struct KProcess *p)       { (void)p; return IRIS_OK; }
void         kprocess_quota_release_page(struct KProcess *p)       { (void)p; }

/* ── bootstrap frame stubs (Fase 6.2) ───────────────────────────────────── */
iris_error_t kprocess_register_bootstrap_frame(struct KProcess *p, struct KFrame *f) {
    if (!p || !f) return IRIS_ERR_INVALID_ARG;
    if (p->bootstrap_frame_count >= 32u) return IRIS_ERR_NO_MEMORY;
    p->bootstrap_frames[p->bootstrap_frame_count++] = f;
    return IRIS_OK;
}
void kprocess_release_bootstrap_frames(struct KProcess *p) {
    if (!p) return;
    for (uint32_t i = 0; i < p->bootstrap_frame_count; i++) {
        if (p->bootstrap_frames[i]) {
            kobject_release(&p->bootstrap_frames[i]->base);
            p->bootstrap_frames[i] = 0;
        }
    }
    p->bootstrap_frame_count = 0;
}

/* ── Minimal test VMO stub ─────────────────────────────────────────────────
 * Creates a bare-bones KVmo (sparse=0, owned=0, no pages[], no PMM) suitable
 * for unit tests that exercise kframe_alloc_vmo_page / kframe_obj_destroy
 * without pulling full kvmo.c (which needs pmm_alloc_pages / PHYS_TO_VIRT).
 * Destroy calls kslab_free only — no PMM.
 * ─────────────────────────────────────────────────────────────────────────── */
#include <iris/nc/kvmo.h>

static void kvmo_stub_destroy(struct KObject *o) {
    kslab_free(o, (uint32_t)sizeof(struct KVmo));
}
static const struct KObjectOps kvmo_stub_ops_val = {
    .destroy = kvmo_stub_destroy,
};

struct KVmo *kvmo_make_stub(void) {
    struct KVmo *v = (struct KVmo *)kslab_alloc((uint32_t)sizeof(struct KVmo));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    kobject_init(&v->base, KOBJ_VMO, &kvmo_stub_ops_val);
    return v;
}

/* ── iris_panic stub — calls abort() so lifecycle assertions are loud ────── */
#include <iris/panic.h>
__attribute__((noreturn)) void iris_panic(const char *msg) {
    fprintf(stderr, "[PANIC] %s\n", msg);
    abort();
}

/* ── Failure injection API (Fase 6.4) ─────────────────────────────────────
 *
 * Test-only hooks to simulate allocation failures at specific points.
 * State variables are declared at the top of this file (g_kslab_fail_countdown,
 * g_paging_force_fail) so they are visible to both kslab_alloc and the paging
 * stubs below.
 *
 *   kslab_fail_after(n)      — make the (n+1)-th kslab_alloc call return NULL.
 *                              n=0 fails the very next call; n=1 lets one succeed.
 *   kslab_clear_fail()       — disable kslab failure injection.
 *
 *   paging_force_fail_next() — make the next paging_map_checked_in return -1
 *                              (simulates page-table allocation failure after the
 *                              KFrameMapping node has already been slab-alloc'd).
 *   paging_clear_force_fail()— disable paging failure injection (also called by
 *                              paging_stub_reset for full test isolation).
 *
 * Rules:
 *   - Each test MUST restore clean state by calling the clear functions or
 *     paging_stub_reset() on both success and error paths.
 *   - These globals are not thread-safe; unit tests are single-threaded.
 * ────────────────────────────────────────────────────────────────────────── */
void kslab_fail_after(int n)       { g_kslab_fail_countdown = n; }
void kslab_clear_fail(void)        { g_kslab_fail_countdown = -1; }
void paging_force_fail_next(void)  { g_paging_force_fail = 1; }
void paging_clear_force_fail(void) { g_paging_force_fail = 0; }

/* ── Stateful paging stubs for Fase 5.1 KFrame mapping tests ─────────────
 *
 * Previously these stubs were trivial no-ops (always succeed, always return 0).
 * That made any test using paging_virt_to_phys_in a false positive: duplicate
 * map checks, PTE presence checks, and unmap verification all silently passed
 * regardless of what the code actually did.
 *
 * This replacement maintains a small flat array of (cr3, virt) → phys records
 * that mirrors what the real page-table walk does for 4 KiB pages:
 *   - paging_map_checked_in  registers a mapping; fails if (cr3,virt) already exists.
 *   - paging_virt_to_phys_in looks up the mapping; returns 0 if absent.
 *   - paging_unmap_in        removes the entry (swap-with-last idiom).
 *   - paging_stub_reset      clears all entries and paging failure state.
 *
 * 256 slots is far more than any single unit test needs.
 * ────────────────────────────────────────────────────────────────────────── */
#include <iris/paging.h>

#define STUB_PMAP_MAX 256
typedef struct { uint64_t cr3; uint64_t virt; uint64_t phys; } stub_pmap_t;
static stub_pmap_t stub_pmap[STUB_PMAP_MAX];
static int stub_pmap_n = 0;

void paging_stub_reset(void) {
    stub_pmap_n = 0;
    g_paging_force_fail = 0;
    memset(stub_pmap, 0, sizeof(stub_pmap));
}

int paging_map_checked_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)flags;
    if (!cr3) return -1;
    /* Failure injection: simulate page-table allocation failure. */
    if (g_paging_force_fail) { g_paging_force_fail = 0; return -1; }
    for (int i = 0; i < stub_pmap_n; i++) {
        if (stub_pmap[i].cr3 == cr3 && stub_pmap[i].virt == virt)
            return -1; /* duplicate — mirrors IRIS_ERR_BUSY from real kernel */
    }
    if (stub_pmap_n >= STUB_PMAP_MAX) return -1; /* OOM */
    stub_pmap[stub_pmap_n].cr3  = cr3;
    stub_pmap[stub_pmap_n].virt = virt;
    stub_pmap[stub_pmap_n].phys = phys;
    stub_pmap_n++;
    return 0;
}

uint64_t paging_virt_to_phys_in(uint64_t cr3, uint64_t virt) {
    for (int i = 0; i < stub_pmap_n; i++) {
        if (stub_pmap[i].cr3 == cr3 && stub_pmap[i].virt == virt)
            return stub_pmap[i].phys;
    }
    return 0;
}

void paging_unmap_in(uint64_t cr3, uint64_t virt) {
    for (int i = 0; i < stub_pmap_n; i++) {
        if (stub_pmap[i].cr3 == cr3 && stub_pmap[i].virt == virt) {
            stub_pmap[i] = stub_pmap[--stub_pmap_n]; /* swap with last */
            return;
        }
    }
}
