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

/* ── Failure injection state (Phase 6.4) — declared early, used by kslab/paging stubs ── */
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
void         task_yield(void)            { }

/*
 * The current task is SETTABLE now, because the syscall layer is compiled into
 * this suite and every syscall begins by asking who is calling.
 *
 * NULL stays the default, and that matters: the object-layer suites were
 * written against a kernel with no current task, and the CSpace walk's root
 * guard is deliberately inert in that case.  A test that wants to exercise a
 * syscall installs a task for the duration and takes it back down, so no suite
 * can leak a caller into another.
 */
static struct task *g_test_current;
struct task *task_current(void)          { return g_test_current; }
void         test_set_current_task(struct task *t) { g_test_current = t; }

/*
 * The restart request, observable so a test can assert a syscall PARKED.
 *
 * In the kernel this sets a flag the dispatcher acts on; here there is no
 * dispatcher, so it records the fact and the test reads it.  That is the
 * distinction the host suite could not otherwise make: a handler that returns
 * 0 having parked and one that returns 0 having finished look identical from
 * the outside, and for a preemptible operation those are opposite answers.
 */
static uint32_t g_test_restarts;
void syscall_request_restart(struct task *t) {
    if (t) t->sc_restart = 1u;
    g_test_restarts++;
}
uint32_t test_restart_count(void) { return g_test_restarts; }

/* ── kprocess quota stubs (needed when compiling kchannel.c) ─────────────── */
#include <iris/nc/kprocess.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kobject.h>

/* Bootstrap-frame stubs RETIRED (Stage 7-proc): the frames belong to the
 * ADDRESS SPACE they are mapped in, and kvspace.c — which this build already
 * compiles — provides kvspace_register_bootstrap_frame for real.  A stub here
 * would shadow the thing under test. */

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

/* ── Failure injection API (Phase 6.4) ─────────────────────────────────────
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

/* ── Stateful paging stubs for Phase 5.1 KFrame mapping tests ─────────────
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
void paging_stub_reset_tables(void);   /* defined with the table stubs below */
typedef struct { uint64_t cr3; uint64_t virt; uint64_t phys; } stub_pmap_t;
static stub_pmap_t stub_pmap[STUB_PMAP_MAX];
static int stub_pmap_n = 0;

void paging_stub_reset(void) {
    stub_pmap_n = 0;
    g_paging_force_fail = 0;
    memset(stub_pmap, 0, sizeof(stub_pmap));
    paging_stub_reset_tables();
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


#define STUB_PT_MAX 256
typedef struct { uint64_t cr3; uint64_t key; int level; uint64_t phys; } stub_pt_t;
static stub_pt_t stub_pt[STUB_PT_MAX];
static int       stub_pt_n = 0;

static uint64_t stub_pt_key(uint64_t virt, int level) {
    /* The bits above the level being filled: a PDPT is shared by every VA with
     * the same PML4 index, a PD by every VA with the same PML4+PDPT, and so on. */
    if (level == 3) return virt >> 39;
    if (level == 2) return virt >> 30;
    return virt >> 21;
}

static int stub_pt_has(uint64_t cr3, uint64_t virt, int level) {
    uint64_t k = stub_pt_key(virt, level);
    for (int i = 0; i < stub_pt_n; i++)
        if (stub_pt[i].cr3 == cr3 && stub_pt[i].level == level && stub_pt[i].key == k)
            return 1;
    return 0;
}

/*
 * Level modelling is OPT-IN.
 *
 * Most host suites are about mapping records, frame lifecycle and VSpace
 * teardown, and predate paging levels being objects at all; to them an address
 * space is a place mappings go.  Modelling an empty walk for every one of them
 * would replace what they test with a paging fixture nobody wrote.
 *
 * A suite that IS about the walk turns this on and then owes every level,
 * exactly as a real holder does.
 */
static int stub_pt_strict = 0;
void paging_stub_strict_levels(int on) { stub_pt_strict = on; stub_pt_n = 0; }

/* Stage 6-pure Step 4: no MMU on the host, so a PML4 needs no contents —
 * what the tests observe is that the object exists and behaves, not what the
 * hardware would read out of the page. */
void paging_init_user_pml4(uint64_t pml4_page_phys) { (void)pml4_page_phys; }

int paging_missing_level_in(uint64_t cr3, uint64_t virt) {
    if (!cr3) return -1;
    if (!stub_pt_strict) return 0;          /* the walk is whatever it needs to be */
    if (!stub_pt_has(cr3, virt, 3)) return 3;
    if (!stub_pt_has(cr3, virt, 2)) return 2;
    if (!stub_pt_has(cr3, virt, 1)) return 1;
    return 0;
}

int paging_install_table_in(uint64_t cr3, uint64_t virt, uint64_t table_phys,
                            uint64_t flags) {
    (void)flags;
    if (!cr3 || !table_phys || (table_phys & 0xFFFULL)) return -1;
    if (!stub_pt_strict) return 0;          /* nothing was ever missing */
    int level = paging_missing_level_in(cr3, virt);
    if (level <= 0) return level;
    if (stub_pt_n >= STUB_PT_MAX) return -1;
    stub_pt[stub_pt_n].cr3   = cr3;
    stub_pt[stub_pt_n].key   = stub_pt_key(virt, level);
    stub_pt[stub_pt_n].level = level;
    stub_pt[stub_pt_n].phys  = table_phys;
    stub_pt_n++;
    return level;
}

/* Stage 6-pure: the level leaves the walk again.  Identity-checked like the
 * real one, so a test can prove a stale (va, level) record detaches nothing. */
int paging_detach_table_in(uint64_t cr3, uint64_t virt, int level,
                           uint64_t table_phys) {
    if (!cr3 || !table_phys || level < 1 || level > 3) return -1;
    if (!stub_pt_strict) return -1;      /* no walk is modelled: nothing to take out */
    uint64_t k = stub_pt_key(virt, level);
    for (int i = 0; i < stub_pt_n; i++) {
        if (stub_pt[i].cr3 != cr3 || stub_pt[i].level != level) continue;
        if (stub_pt[i].key != k || stub_pt[i].phys != table_phys) continue;
        stub_pt[i] = stub_pt[--stub_pt_n];
        return 0;
    }
    return -1;
}

void paging_stub_reset_tables(void) { stub_pt_n = 0; }

int paging_map_strict_in(uint64_t cr3, uint64_t virt, uint64_t phys,
                         uint64_t flags) {
    int level = paging_missing_level_in(cr3, virt);
    if (level != 0) return level;
    return paging_map_checked_in(cr3, virt, phys, flags);
}

void paging_destroy_user_space_from(uint64_t cr3, int pml4_pooled) {
    (void)cr3; (void)pml4_pooled;   /* no page tables on the host */
}

/*
 * Stage 6-pure Step 1: the host models the WALK, not the hardware.
 *
 * What the tests need to observe is the level arithmetic — that a fresh
 * address space is missing a PDPT, that installing one leaves it missing a PD,
 * and that a completed walk reports nothing missing.  One record per installed
 * level, keyed by the index bits that level actually consumes, reproduces that
 * exactly without a page-table walk.
 */

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

/*
 * Stage 7-mem: a notification's close hook unbinds any interrupt routed to it,
 * so the host build needs the routing symbol.  The IRQ table is kernel-only
 * (PIC masks, an interrupt-time signal path), and nothing the host suite
 * exercises registers a route — so the honest stub is a no-op that records
 * nothing rather than a fake table that could be mistaken for coverage.
 */
struct KNotification;
void irq_routing_unregister_notification(struct KNotification *n) { (void)n; }

/* ── stubs for the syscall layers now compiled into this suite ────────────
 *
 * These are the boundary between the syscall handlers under test and the
 * subsystems they report on: instrumentation gauges, the user-copy path, and
 * the TCB allocator.  Every one of them is on a SUCCESS path — the tests here
 * assert refusals, which return before reaching any of these — so a stub that
 * returns nothing is not hiding a behaviour, it is declining to link a
 * subsystem the refusal cases never touch.
 */
#include <iris/usercopy.h>
int copy_to_user_checked(uint64_t dst, const void *src, uint32_t len) {
    (void)dst; (void)src; (void)len; return 0;   /* no user address space here */
}
int user_range_writable(uint64_t ptr, uint32_t len) {
    (void)ptr; (void)len; return 0;              /* no user address space here */
}
uint32_t syscall_restart_count(void)         { return g_test_restarts; }
uint32_t kprocess_quota_failed_count(void)   { return 0; }
uint32_t kprocess_quota_rollback_count(void) { return 0; }
uint32_t kslab_fail_count(void)   { return 0; }
uint32_t kslab_total_bytes(void)  { return 0; }
uint32_t kslab_used_bytes(void)   { return 0; }
void     task_registry_stats(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    if (a) *a = 0;
    if (b) *b = 0;
    if (c) *c = 0;
    if (d) *d = 0;
}
struct task *ktcb_alloc_at(void *mem) { (void)mem; return 0; }
void ktcb_stats(uint32_t *live, uint32_t *hwm, uint32_t *retyped, uint32_t *destroyed) {
    if (live)      *live      = 0;
    if (hwm)       *hwm       = 0;
    if (retyped)   *retyped   = 0;
    if (destroyed) *destroyed = 0;
}

/*
 * Scheduler-side operations the TCB syscalls delegate to once their argument
 * and authority checks have PASSED.  The suite asserts refusals, so nothing
 * here is reached by a passing test; a stub is the honest way to say "this
 * subsystem is not what is under test" rather than linking the scheduler.
 */
iris_error_t ktcb_configure(struct task *t, struct KCNode *cs, struct KVSpace *vs) {
    (void)t; (void)cs; (void)vs; return IRIS_ERR_NOT_SUPPORTED;
}
iris_error_t ktcb_write_regs(struct task *t, uint64_t entry, uint64_t rsp,
                             uint64_t arg) {
    (void)t; (void)entry; (void)rsp; (void)arg; return IRIS_ERR_NOT_SUPPORTED;
}
void task_suspend(struct task *t)        { (void)t; }
void task_exit_current(void)             { }
void task_kill_external(struct task *t)  { (void)t; }
