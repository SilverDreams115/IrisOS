/*
 * pager/main.c — the IRIS user pager service.
 *
 * Fase 27: a supervised userland service that resolves faults from raw VMO
 * grants (PGR_OP_MAP_RESUME).  Fase 28 Bloque B: a complete file-backed memory
 * subsystem layered on top — backing identity + generation, validated file
 * regions, a bounded RO page cache, private-writable pages, EOF/zero-fill, and
 * generation-safe revocation — all in userland, composed from the existing
 * kernel primitives (SYS_VMO_MAP_PAGE, SYS_PROCESS_VSPACE, fault generations,
 * seq-checked resume) plus the VFS.  No new syscall.
 *
 * Fase 28.1 — file grants + multi-target:
 *   - File bytes come EXCLUSIVELY through VFS file grants
 *     (VFS_EP_OP_GRANT_READ_AT over the badged SESSION cap in slot 4).  The
 *     pager never sends a pathname; the VFS validates every access against
 *     its own grant table, so even a compromised pager reads only the
 *     backings its supervisor explicitly granted.
 *   - A registered backing carries the VFS-ISSUED (backing_id, generation);
 *     the pager cross-checks them against GRANT_QUERY_IDENTITY before
 *     accepting.
 *   - ONE shared fault notification (slot 5) serves ALL targets: the
 *     supervisor registers each target's exception handler with signal bit
 *     (1 << tidx).  A pending-bits accumulator makes the wait a wait-any:
 *     bits consumed for other targets are kept, never lost, so 16 targets
 *     can fault interleaved without a notification per target.
 *
 * The pager holds ONLY its minted manifest (pager_proto.h): a control
 * endpoint, its badged VFS session cap, the shared fault notification,
 * per-target proc/vspace grants, and two VMO grants.  It has no global VFS
 * access, no untyped, no spawn, no device caps, no KDEBUG.
 */
#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/fault_proto.h>
#include <iris/vfs_ep_proto.h>
#include "pager_proto.h"

#define PAGE_SZ    4096u
#define PG_SCRATCH 0x8060000000ULL   /* pager-private scratch VA for page fills */

static inline long pg_sys1(long nr, long a0) {
    long ret; __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory"); return ret;
}
static inline long pg_sys2(long nr, long a0, long a1) {
    long ret; __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory"); return ret;
}
static inline long pg_sys3(long nr, long a0, long a1, long a2) {
    long ret; __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"); return ret;
}
static inline long pg_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret; register long r10 __asm__("r10") = a3;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "rcx", "r11", "memory"); return ret;
}

static inline uint32_t pg_rd32(const uint8_t *b, uint32_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off+1] << 8) |
           ((uint32_t)b[off+2] << 16) | ((uint32_t)b[off+3] << 24);
}
static inline uint64_t pg_rd64(const uint8_t *b, uint32_t off) {
    uint64_t v = 0; for (uint32_t i = 0; i < 8u; i++) v |= (uint64_t)b[off+i] << (i*8); return v;
}
static void pg_zero(void *d, uint32_t n) { uint8_t *p = d; for (uint32_t i = 0; i < n; i++) p[i] = 0; }
static void pg_msg_zero(struct IrisMsg *m) { pg_zero(m, (uint32_t)sizeof(*m)); }

/* ── file-backed state ──────────────────────────────────────────────────── */

struct pg_backing {
    uint8_t  used;
    uint32_t grant_idx;                          /* VFS file grant (session-scoped) */
    uint64_t backing_id, generation, file_size;  /* VFS-issued identity */
};
struct pg_region {
    uint8_t  used;
    uint32_t target_idx, backing_idx, prot, mode;
    uint64_t start_va, mem_len, file_off, file_len, backing_gen;
    uint32_t cache_refmask;   /* bit i = holds a ref on cache slot i */
    uint32_t priv_ownmask;    /* bit i = owns private pool page i */
};
struct pg_cache {
    uint8_t  valid;
    uint64_t backing_id, generation, page_off;   /* file byte offset (page-aligned) */
    uint32_t refcount;        /* regions currently referencing this slot */
};

static struct pg_backing g_back[PGR_MAX_BACKINGS];
static struct pg_region  g_reg[PGR_MAX_REGIONS];
static struct pg_cache   g_cache[PGR_CACHE_CAP];
static uint8_t           g_priv_used[PGR_PRIV_CAP];
static long              g_self_vs = -1;
static uint64_t          g_pending = 0;   /* accumulated fault bits (bit i = target i) */

/* IPC buffers (single-threaded service). */
static uint8_t g_ctrl_buf[IRIS_IPC_BUF_SIZE];
static uint8_t g_vfs_buf[IRIS_IPC_BUF_SIZE];

static struct pgr_diag g_diag;   /* counters (also the DIAG reply payload) */

/* ── VFS grant ops (the pager's ONLY file authority) ─────────────────────── */

/* GRANT_QUERY_IDENTITY: fetch the VFS-issued (backing_id, generation, rights)
 * for grant_idx.  0 on success, negative marker on denial. */
static long pg_grant_query(uint32_t grant_idx, uint64_t *bid, uint64_t *gen,
                           uint64_t *rights) {
    struct IrisMsg m;
    pg_msg_zero(&m);
    m.label      = VFS_EP_OP_GRANT_QUERY_IDENTITY;
    m.words[0]   = (uint64_t)grant_idx;
    m.word_count = 1u;
    long r = pg_sys2(SYS_EP_CALL, (long)PGR_SLOT_VFS_EP, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -(long)PGR_ERR_GRANT;
    *bid = m.words[1]; *gen = m.words[2]; *rights = m.words[3];
    return 0;
}

/* ── VFS read: fill dst[0..want) via a file grant at `file_off` ─────────────
 * Returns bytes actually read (>=0, < want at EOF), or a negative error.
 * No pathname is sent: the grant index over the badged session cap is the
 * entire request — the VFS decides from ITS table whether to serve it. */
static long pg_read_file(uint32_t grant_idx, uint64_t file_off,
                         uint8_t *dst, uint32_t want) {
    uint32_t got = 0;
    while (got < want) {
        struct IrisMsg m;
        pg_msg_zero(&m);
        m.label      = VFS_EP_OP_GRANT_READ_AT;
        m.words[0]   = (uint64_t)grant_idx;
        m.words[1]   = file_off + got;
        m.words[2]   = (want - got < VFS_EP_DATA_MAX) ? (want - got) : VFS_EP_DATA_MAX;
        m.word_count = 3u;
        m.buf_uptr   = (uint64_t)(uintptr_t)g_vfs_buf;
        m.buf_len    = 0u;
        long r = pg_sys2(SYS_EP_CALL, (long)PGR_SLOT_VFS_EP, (long)&m);
        if (r != 0) return r;
        if (m.label != IRIS_EP_REPLY_OK) {
            g_diag.grant_denied++;
            return -(long)PGR_ERR_GRANT;
        }
        uint32_t n = (uint32_t)m.words[1];
        if (n == 0u) break;                 /* EOF */
        if (n > (want - got)) n = want - got;
        for (uint32_t i = 0; i < n; i++) dst[got + i] = g_vfs_buf[i];
        got += n;
    }
    return (long)got;
}

/* ── page fill: fill VMO page (vmo grant slot, page_idx) with `fb` file bytes
 * from backing `bk` at `file_off`, zero the rest.  RW mapped into the pager's
 * own VSpace at PG_SCRATCH, written, then unmapped.  0 on success. */
static long pg_fill_page(long vmo_slot, uint32_t page_idx, const struct pg_backing *bk,
                         uint64_t file_off, uint32_t fb) {
    long r = pg_sys4(SYS_VMO_MAP_PAGE, vmo_slot, g_self_vs, (long)PG_SCRATCH,
                     (long)(((uint64_t)page_idx * PAGE_SZ) | 1u /*W*/));
    if (r != 0) { g_diag.page_fill_fail++; return r; }
    uint8_t *page = (uint8_t *)(uintptr_t)PG_SCRATCH;
    pg_zero(page, PAGE_SZ);                              /* zero-fill (F11/F12) */
    if (fb > PAGE_SZ) fb = PAGE_SZ;
    if (fb > 0u) {
        long got = pg_read_file(bk->grant_idx, file_off, page, fb);
        if (got < 0) { (void)pg_sys2(SYS_VMO_UNMAP, (long)PG_SCRATCH, (long)PAGE_SZ); g_diag.page_fill_fail++; return got; }
        if ((uint32_t)got < fb) {
            /* Unexpected short read mid-file: zero the shortfall (already zero)
             * but report it — no partial PTE is installed by the caller. */
            (void)pg_sys2(SYS_VMO_UNMAP, (long)PG_SCRATCH, (long)PAGE_SZ);
            g_diag.page_fill_fail++;
            return -(long)PGR_ERR_SHORT_READ;
        }
    }
    r = pg_sys2(SYS_VMO_UNMAP, (long)PG_SCRATCH, (long)PAGE_SZ);
    if (r != 0) { g_diag.page_fill_fail++; return r; }
    g_diag.page_fill++;
    return 0;
}

/* ── RO cache ───────────────────────────────────────────────────────────── */
static int pg_cache_find(uint64_t bid, uint64_t gen, uint64_t page_off) {
    for (uint32_t i = 0; i < PGR_CACHE_CAP; i++)
        if (g_cache[i].valid && g_cache[i].backing_id == bid &&
            g_cache[i].generation == gen && g_cache[i].page_off == page_off)
            return (int)i;
    return -1;
}
/* Pick an evictable slot (refcount 0): prefer an invalid one, else the lowest
 * index (deterministic FIFO-ish).  -1 if the cache is full of live slots. */
static int pg_cache_alloc(void) {
    for (uint32_t i = 0; i < PGR_CACHE_CAP; i++)
        if (!g_cache[i].valid) return (int)i;
    for (uint32_t i = 0; i < PGR_CACHE_CAP; i++)
        if (g_cache[i].refcount == 0u) { g_diag.cache_evict++; g_cache[i].valid = 0; return (int)i; }
    return -1;
}
static uint32_t pg_cache_entries(void) {
    uint32_t n = 0; for (uint32_t i = 0; i < PGR_CACHE_CAP; i++) if (g_cache[i].valid) n++; return n;
}

/* ── shared fault notification: wait-any with a pending-bits accumulator ────
 * Fase 28.1: every target signals the ONE notification at slot 5 with bit
 * (1 << tidx).  Waiting for a specific target consumes ONLY that target's
 * bit; bits that arrive for other targets are accumulated, never dropped, so
 * interleaved faults from many targets survive any service order.  0 on
 * success, NOFAULT marker on timeout. */
static long pg_wait_fault(uint32_t tidx) {
    uint64_t bit = 1ull << tidx;
    if (g_pending & bit) { g_pending &= ~bit; return 0; }
    for (uint32_t tries = 0; tries < 64u; tries++) {
        uint64_t bits = 0;
        g_diag.notif_waits++;
        if (pg_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)PGR_SLOT_FAULT_NOTIF,
                    (long)(uintptr_t)&bits, 2000000000L) != 0)
            return -(long)PGR_ERR_NOFAULT;
        g_diag.notif_wakeups++;
        g_pending |= bits;
        if (g_pending & bit) { g_pending &= ~bit; return 0; }
    }
    return -(long)PGR_ERR_NOFAULT;
}

/* ── region resolution ──────────────────────────────────────────────────── */
static long pg_resolve_region(uint32_t tidx) {
    if (tidx >= PGR_MAX_TARGETS) return -(long)PGR_ERR_BADOP;
    long tproc = (long)PGR_TSLOT_PROC(tidx);
    long tvs   = (long)PGR_TSLOT_VS(tidx);

    long wr = pg_wait_fault(tidx);
    if (wr != 0) return wr;

    uint8_t fb[FAULT_MSG_LEN];
    long r = pg_sys2(SYS_PROCESS_FAULT_INFO, tproc, (long)(uintptr_t)fb);
    if (r != 0) return r;
    uint32_t vector = pg_rd32(fb, FAULT_OFF_VECTOR);
    uint32_t task   = pg_rd32(fb, FAULT_OFF_TASK_ID);
    uint32_t seq    = pg_rd32(fb, FAULT_OFF_SEQ);
    uint32_t err    = pg_rd32(fb, FAULT_OFF_ERROR);
    uint64_t cr2    = pg_rd64(fb, FAULT_OFF_CR2);
    if (vector != 14u || seq == 0u || task == 0u) return -(long)PGR_ERR_INFO;
    uint64_t va = cr2 & ~0xFFFULL;
    int write_fault = (err & 0x02u) != 0;

    /* Find the region for (target, VA). */
    struct pg_region *rg = 0;
    for (uint32_t i = 0; i < PGR_MAX_REGIONS; i++) {
        if (g_reg[i].used && g_reg[i].target_idx == tidx &&
            va >= g_reg[i].start_va && va < g_reg[i].start_va + g_reg[i].mem_len) {
            rg = &g_reg[i]; break;
        }
    }
    if (!rg) return -(long)PGR_ERR_NO_REGION;                    /* F9 */

    struct pg_backing *bk = &g_back[rg->backing_idx];
    if (!bk->used || bk->generation != rg->backing_gen)
        { g_diag.generation_stale++; return -(long)PGR_ERR_STALE_GEN; }  /* F18 */

    /* Read-only region + write fault → denied (never silently made writable, F13). */
    if (write_fault && rg->mode == PGR_MODE_RO_SHARED) return -(long)PGR_ERR_ACCESS;

    uint64_t region_off = va - rg->start_va;                    /* page-aligned */
    uint64_t file_off   = rg->file_off + region_off;
    uint32_t fbytes;                                            /* file bytes in this page */
    if (region_off >= rg->file_len) fbytes = 0u;                /* BSS-like zero tail (F11) */
    else { uint64_t rem = rg->file_len - region_off; fbytes = (rem < PAGE_SZ) ? (uint32_t)rem : PAGE_SZ; }

    if (rg->mode == PGR_MODE_RO_SHARED) {
        int slot = pg_cache_find(bk->backing_id, bk->generation, file_off);
        if (slot >= 0) g_diag.cache_hit++;
        else {
            slot = pg_cache_alloc();
            if (slot < 0) return -(long)PGR_ERR_CACHE_FULL;
            long fr = pg_fill_page((long)PGR_VSLOT(PGR_VMO_CACHE), (uint32_t)slot, bk, file_off, fbytes);
            if (fr != 0) return fr;                             /* no PTE, no valid entry (F17/F38) */
            g_cache[slot].valid = 1;
            g_cache[slot].backing_id = bk->backing_id;
            g_cache[slot].generation = bk->generation;
            g_cache[slot].page_off = file_off;
            g_cache[slot].refcount = 0;
            g_diag.cache_miss++;
        }
        /* Map the cache page into the target at the fault VA: read-only, plus
         * MAP_EXEC (bit 1) for an RX code segment.  Never writable (shared cache
         * pages are immutable); W^X already guaranteed at registration. */
        uint64_t mflags = (rg->prot & PGR_PROT_X) ? 2u : 0u;
        long mr = pg_sys4(SYS_VMO_MAP_PAGE, (long)PGR_VSLOT(PGR_VMO_CACHE), tvs, (long)va,
                          (long)(((uint64_t)slot * PAGE_SZ) | mflags));
        if (mr != 0) return mr;
        /* Take a region ref on this cache slot (idempotent per region). */
        if (!(rg->cache_refmask & (1u << slot))) { rg->cache_refmask |= (1u << slot); g_cache[slot].refcount++; }
    } else if (rg->mode == PGR_MODE_PRIVATE_WRITABLE) {
        /* A fresh private page per fault (copy-at-fill, no writeback, F14). */
        int slot = -1;
        for (uint32_t i = 0; i < PGR_PRIV_CAP; i++) if (!g_priv_used[i]) { slot = (int)i; break; }
        if (slot < 0) return -(long)PGR_ERR_PRIVFULL;
        long fr = pg_fill_page((long)PGR_VSLOT(PGR_VMO_PRIVATE), (uint32_t)slot, bk, file_off, fbytes);
        if (fr != 0) return fr;
        long mr = pg_sys4(SYS_VMO_MAP_PAGE, (long)PGR_VSLOT(PGR_VMO_PRIVATE), tvs, (long)va,
                          (long)(((uint64_t)slot * PAGE_SZ) | 1u /*W*/));
        if (mr != 0) return mr;
        g_priv_used[slot] = 1;
        rg->priv_ownmask |= (1u << slot);
        g_diag.private_pages++;
    } else {
        return -(long)PGR_ERR_MODE;                             /* shared-writable (F16) */
    }

    /* seq-checked resume — the target continues. */
    return pg_sys3(SYS_EXCEPTION_RESUME, tproc, (long)task, (long)(((uint64_t)seq << 32) | 2u));
}

/* Drop a region's cache/private references (on unregister / target death). */
static void pg_region_release(struct pg_region *rg) {
    for (uint32_t i = 0; i < PGR_CACHE_CAP; i++)
        if (rg->cache_refmask & (1u << i)) { if (g_cache[i].refcount) g_cache[i].refcount--; }
    for (uint32_t i = 0; i < PGR_PRIV_CAP; i++)
        if (rg->priv_ownmask & (1u << i)) { g_priv_used[i] = 0; if (g_diag.private_pages) g_diag.private_pages--; }
    rg->cache_refmask = 0; rg->priv_ownmask = 0;
}

/* ── control handlers ───────────────────────────────────────────────────── */
static long pg_register_backing(const struct pgr_backing_req *rq) {
    if (rq->backing_idx >= PGR_MAX_BACKINGS) return -(long)PGR_ERR_BADOP;
    if (rq->grant_idx >= VFS_GRANTS_PER_SESSION) return -(long)PGR_ERR_RANGE;
    /* Fase 28.1: the supervisor-declared identity must MATCH the VFS-issued
     * one for this grant — the pager verifies before trusting, so a wrong or
     * dead grant can never be silently bound to a backing (A2/A3). */
    {
        uint64_t bid = 0, gen = 0, rights = 0;
        if (pg_grant_query(rq->grant_idx, &bid, &gen, &rights) != 0)
            return -(long)PGR_ERR_GRANT;
        if (bid != rq->backing_id || gen != rq->generation)
            return -(long)PGR_ERR_GRANT;
        if (!(rights & VFS_FILE_RIGHT_READ)) return -(long)PGR_ERR_GRANT;
    }
    struct pg_backing *b = &g_back[rq->backing_idx];
    if (!b->used) g_diag.backing_live++;
    b->used = 1;
    b->grant_idx  = rq->grant_idx;
    b->backing_id = rq->backing_id;
    b->generation = rq->generation;
    b->file_size  = rq->file_size;
    return 0;
}
static long pg_register_region(const struct pgr_region_req *rq) {
    if (rq->region_idx >= PGR_MAX_REGIONS || rq->target_idx >= PGR_MAX_TARGETS ||
        rq->backing_idx >= PGR_MAX_BACKINGS) return -(long)PGR_ERR_BADOP;
    if (rq->mode == PGR_MODE_SHARED_WRITABLE) return -(long)PGR_ERR_MODE;   /* F16, no state */
    if (rq->mode != PGR_MODE_RO_SHARED && rq->mode != PGR_MODE_PRIVATE_WRITABLE)
        return -(long)PGR_ERR_MODE;
    struct pg_backing *b = &g_back[rq->backing_idx];
    if (!b->used || b->generation != rq->backing_generation) return -(long)PGR_ERR_NOBACK; /* F4/F18 */
    /* Region validation (atomic — nothing written until all checks pass, F6-F8). */
    if (rq->start_va & 0xFFFULL) return -(long)PGR_ERR_RANGE;
    if (rq->memory_length == 0u || (rq->memory_length & 0xFFFULL)) return -(long)PGR_ERR_RANGE;
    if (rq->start_va < 0x0000008000000000ULL) return -(long)PGR_ERR_RANGE;
    uint64_t vend = rq->start_va + rq->memory_length;
    if (vend < rq->start_va || vend > 0x0000800000000000ULL) return -(long)PGR_ERR_RANGE;
    if (rq->file_offset & 0xFFFULL) return -(long)PGR_ERR_RANGE;     /* page-aligned file offset */
    uint64_t fend = rq->file_offset + rq->file_length;
    if (fend < rq->file_offset) return -(long)PGR_ERR_RANGE;         /* overflow */
    if (fend > b->file_size) return -(long)PGR_ERR_RANGE;            /* beyond backing */
    if (rq->file_length > rq->memory_length) return -(long)PGR_ERR_RANGE;
    if ((rq->prot & PGR_PROT_W) && rq->mode == PGR_MODE_RO_SHARED) return -(long)PGR_ERR_RANGE;
    /* W^X (ELF-segment groundwork, F30): no region is both writable and
     * executable — a data/BSS segment (RW) and a code segment (RX) are
     * distinct.  The kernel enforces W^X on the PTE too; rejecting here keeps
     * the pager's segment model honest and fails fast. */
    if ((rq->prot & PGR_PROT_W) && (rq->prot & PGR_PROT_X)) return -(long)PGR_ERR_RANGE;
    if (rq->prot & ~(PGR_PROT_R | PGR_PROT_W | PGR_PROT_X)) return -(long)PGR_ERR_RANGE;
    if (!(rq->prot & PGR_PROT_R)) return -(long)PGR_ERR_RANGE;  /* every segment is readable */
    /* No overlap with another region of the same target. */
    for (uint32_t i = 0; i < PGR_MAX_REGIONS; i++) {
        if (i == rq->region_idx || !g_reg[i].used || g_reg[i].target_idx != rq->target_idx) continue;
        uint64_t a0 = rq->start_va, a1 = vend;
        uint64_t b0 = g_reg[i].start_va, b1 = g_reg[i].start_va + g_reg[i].mem_len;
        if (a0 < b1 && b0 < a1) return -(long)PGR_ERR_RANGE;
    }
    struct pg_region *rg = &g_reg[rq->region_idx];
    if (rg->used) pg_region_release(rg);
    else g_diag.region_count++;
    rg->used = 1;
    rg->target_idx = rq->target_idx; rg->backing_idx = rq->backing_idx;
    rg->prot = rq->prot; rg->mode = rq->mode;
    rg->start_va = rq->start_va; rg->mem_len = rq->memory_length;
    rg->file_off = rq->file_offset; rg->file_len = rq->file_length;
    rg->backing_gen = rq->backing_generation;
    rg->cache_refmask = 0; rg->priv_ownmask = 0;
    return 0;
}
static long pg_unregister_region(uint32_t ridx) {
    if (ridx >= PGR_MAX_REGIONS) return -(long)PGR_ERR_BADOP;
    struct pg_region *rg = &g_reg[ridx];
    if (rg->used) { pg_region_release(rg); rg->used = 0; if (g_diag.region_count) g_diag.region_count--; }
    return 0;
}
/* Local revoke hygiene: bump the local generation copy so cached pages of the
 * old generation are unreachable for new faults; regions still bound to the
 * old generation now fail STALE_GEN; existing mappings survive (kernel VSpace
 * contract).  Fase 28.1: this is BOOKKEEPING, not the authority boundary —
 * the VFS enforces revocation on its own table (GRANT_REVOKE bumps the export
 * generation), so a pager that skips this step still cannot read the revoked
 * backing: every GRANT_READ_AT fails CLOSED at the VFS. */
static long pg_revoke_backing(uint32_t bidx) {
    if (bidx >= PGR_MAX_BACKINGS) return -(long)PGR_ERR_BADOP;
    struct pg_backing *b = &g_back[bidx];
    if (b->used) {
        b->generation++;                 /* stale-gen: old cache entries unreachable (F27/F18) */
        b->used = 0;                      /* revoked: no new region may bind it (F4) */
        if (g_diag.backing_live) g_diag.backing_live--;
        g_diag.grant_revoke++;
        /* Drop unreferenced cache entries of this backing so they don't linger. */
        for (uint32_t i = 0; i < PGR_CACHE_CAP; i++)
            if (g_cache[i].valid && g_cache[i].backing_id == b->backing_id && g_cache[i].refcount == 0u)
                g_cache[i].valid = 0;
    }
    return 0;
}

/* Fase 28.1: target death/replacement cleanup — release every region of the
 * target and clear its pending fault bit, so a dead target leaves no ghost
 * fault record or accumulator bit behind (A22/A23/A24). */
static long pg_target_reset(uint32_t tidx) {
    if (tidx >= PGR_MAX_TARGETS) return -(long)PGR_ERR_BADOP;
    for (uint32_t i = 0; i < PGR_MAX_REGIONS; i++) {
        if (!g_reg[i].used || g_reg[i].target_idx != tidx) continue;
        pg_region_release(&g_reg[i]);
        g_reg[i].used = 0;
        if (g_diag.region_count) g_diag.region_count--;
    }
    g_pending &= ~(1ull << tidx);
    g_diag.target_resets++;
    return 0;
}

/* Manifest presence oracle — see pager_proto.h for the bit layout. */
static uint32_t pg_report_slots(void) {
    uint32_t mask = 0u;
    for (uint32_t s = 0; s < 20u; s++)
        if (pg_sys1(SYS_CSPACE_RESOLVE, (long)s) >= 0) mask |= (1u << s);
    for (uint32_t i = 0; i < PGR_MAX_TARGETS; i++) {
        if (pg_sys1(SYS_CSPACE_RESOLVE, (long)PGR_TSLOT_PROC(i)) >= 0) mask |= (1u << 20);
        if (pg_sys1(SYS_CSPACE_RESOLVE, (long)PGR_TSLOT_VS(i))   >= 0) mask |= (1u << 21);
    }
    if (pg_sys1(SYS_CSPACE_RESOLVE, 6)  >= 0) mask |= (1u << 24);
    if (pg_sys1(SYS_CSPACE_RESOLVE, 55) >= 0) mask |= (1u << 26);
    if (pg_sys1(SYS_CSPACE_RESOLVE, 56) >= 0) mask |= (1u << 27);
    return mask;
}

/* Fase 27 raw-VMO resolution (still used by T201–T210/T215).  Fase 28.1: the
 * fault wait goes through the same shared-notification accumulator. */
static long pg_serve_raw(uint32_t op, uint32_t tidx, uint32_t vidx, uint32_t flags,
                         uint64_t offset, uint64_t expect_cr2) {
    if (tidx >= PGR_MAX_TARGETS) return -(long)PGR_ERR_BADOP;
    if (op == PGR_OP_MAP_RESUME && vidx >= PGR_MAX_VMOS) return -(long)PGR_ERR_BADOP;
    long tproc = (long)PGR_TSLOT_PROC(tidx), tvs = (long)PGR_TSLOT_VS(tidx);
    long wr = pg_wait_fault(tidx);
    if (wr != 0) return wr;
    uint8_t fb[FAULT_MSG_LEN];
    long r = pg_sys2(SYS_PROCESS_FAULT_INFO, tproc, (long)(uintptr_t)fb);
    if (r != 0) return r;
    uint32_t vector = pg_rd32(fb, FAULT_OFF_VECTOR), task = pg_rd32(fb, FAULT_OFF_TASK_ID), seq = pg_rd32(fb, FAULT_OFF_SEQ);
    uint64_t cr2 = pg_rd64(fb, FAULT_OFF_CR2);
    if (vector != 14u || seq == 0u || task == 0u) return -(long)PGR_ERR_INFO;
    if (expect_cr2 != 0 && cr2 != expect_cr2) return -(long)PGR_ERR_CR2;
    if (op == PGR_OP_MAP_RESUME) {
        uint64_t vva = cr2 & ~0xFFFULL, ofs = (offset & ~0xFFFULL) | (uint64_t)(flags & 0x3u);
        r = pg_sys4(SYS_VMO_MAP_PAGE, (long)PGR_VSLOT(vidx), tvs, (long)vva, (long)ofs);
        if (r != 0) return r;
    }
    return pg_sys3(SYS_EXCEPTION_RESUME, tproc, (long)task, (long)(((uint64_t)seq << 32) | ((op == PGR_OP_KILL) ? 3u : 2u)));
}

void pager_main(handle_id_t bootstrap_ch_h);
void pager_main(handle_id_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;
    g_self_vs = pg_sys1(SYS_VSPACE_SELF, 0);
    g_diag.cache_capacity = PGR_CACHE_CAP;

    struct IrisMsg msg;
    for (;;) {
        pg_msg_zero(&msg);
        msg.buf_uptr = (uint64_t)(uintptr_t)g_ctrl_buf;   /* receive bulk (region/backing reqs) */
        long rr = pg_sys3(SYS_EP_RECV, (long)PGR_SLOT_CTRL_EP, (long)&msg,
                          (long)PGR_SLOT_REPLY);
        if (rr != 0) { pg_sys1(SYS_EXIT, 0); for (;;) {} }

        handle_id_t reply_h = (handle_id_t)msg.attached_handle;
        uint32_t op    = PGR_OP(msg.words[0]);
        uint32_t tidx  = PGR_TIDX(msg.words[0]);
        uint32_t vidx  = PGR_VIDX(msg.words[0]);
        uint32_t flags = PGR_FLAGS(msg.words[0]);

        long result = 0;
        int  shutdown = 0;
        int  reply_diag = 0;
        switch (op) {
        case PGR_OP_PING:    result = 0; break;
        case PGR_OP_REPORT:  result = (long)pg_report_slots(); break;
        case PGR_OP_MAP_RESUME:
        case PGR_OP_KILL:    result = pg_serve_raw(op, tidx, vidx, flags, msg.words[1], msg.words[2]); break;
        case PGR_OP_MAP_REGION:      result = pg_resolve_region(tidx); break;
        case PGR_OP_REGISTER_BACKING:
            if (msg.buf_len >= sizeof(struct pgr_backing_req))
                result = pg_register_backing((const struct pgr_backing_req *)g_ctrl_buf);
            else result = -(long)PGR_ERR_RANGE;
            break;
        case PGR_OP_REGISTER_REGION:
            if (msg.buf_len >= sizeof(struct pgr_region_req))
                result = pg_register_region((const struct pgr_region_req *)g_ctrl_buf);
            else result = -(long)PGR_ERR_RANGE;
            break;
        case PGR_OP_UNREGISTER_REGION: result = pg_unregister_region((uint32_t)msg.words[1]); break;
        case PGR_OP_REVOKE_BACKING:    result = pg_revoke_backing((uint32_t)msg.words[1]); break;
        case PGR_OP_TARGET_RESET:      result = pg_target_reset((uint32_t)msg.words[1]); break;
        case PGR_OP_DIAG:              result = 0; reply_diag = 1; break;
        case PGR_OP_SHUTDOWN:          result = 0; shutdown = 1; break;
        default:                       result = -(long)PGR_ERR_BADOP; break;
        }

        if (reply_h != HANDLE_INVALID) {
            struct IrisMsg reply;
            pg_msg_zero(&reply);
            reply.label = IRIS_EP_REPLY_OK;
            reply.words[0] = (uint64_t)result;
            reply.word_count = 1u;
            if (reply_diag) {
                g_diag.cache_entries = pg_cache_entries();
                g_diag.pending_mask  = (uint32_t)g_pending;
                for (uint32_t i = 0; i < (uint32_t)sizeof(g_diag); i++) g_vfs_buf[i] = ((uint8_t *)&g_diag)[i];
                reply.buf_uptr = (uint64_t)(uintptr_t)g_vfs_buf;
                reply.buf_len  = (uint32_t)sizeof(g_diag);
            }
            /* Fase S1: reply_h is our reusable reply-object CPtr — no close. */
            (void)pg_sys2(SYS_REPLY, (long)reply_h, (long)&reply);
        }
        if (shutdown) { pg_sys1(SYS_EXIT, 0); for (;;) {} }
    }
}
