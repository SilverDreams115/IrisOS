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
iris_error_t kprocess_quota_acquire_channel(struct KProcess *p)    { (void)p; return IRIS_OK; }
void         kprocess_quota_release_channel(struct KProcess *p)    { (void)p; }
iris_error_t kprocess_quota_acquire_notification(struct KProcess *p){ (void)p; return IRIS_OK; }
void         kprocess_quota_release_notification(struct KProcess *p){ (void)p; }
iris_error_t kprocess_quota_acquire_vmo(struct KProcess *p)        { (void)p; return IRIS_OK; }
void         kprocess_quota_release_vmo(struct KProcess *p)        { (void)p; }
iris_error_t kprocess_quota_acquire_page(struct KProcess *p)       { (void)p; return IRIS_OK; }
void         kprocess_quota_release_page(struct KProcess *p)       { (void)p; }
