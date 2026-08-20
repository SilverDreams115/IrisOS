/*
 * svc_loader.c — ring-3 ELF loader for IRIS services.
 *
 * Implements svc_load() using the Phase 29 composable spawn primitives:
 *   SYS_INITRD_VMO(55) + SYS_PROCESS_CREATE(56) + SYS_VMO_MAP_INTO(57) +
 *   SYS_THREAD_START(58); pre-start caps are SYS_PROC_CSPACE_MINT CSpace
 *   mints (Fase 8) — the legacy SYS_HANDLE_INSERT step is gone (A1.8).
 *
 * Supports ET_DYN (static PIE, base=0) ELF64 x86-64 with R_X86_64_RELATIVE
 * RELA relocations.  RDTSC-seeded Xorshift64 ASLR bias applied per spawn.
 */

#include "svc_loader.h"
#include "iris_vspace.h"
#include <iris/endpoint_proto.h>
#include <iris/syscall.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/paging.h>

/* ── Freestanding syscall helpers ─────────────────────────────────── */

static inline long sl_sys0(long nr) {
    return iris_syscall4((long)nr, (long)0L, (long)0L, (long)0L, (long)0);
}

static inline long sl_sys1(long nr, long a0) {
    return iris_syscall4((long)nr, (long)a0, (long)0L, (long)0L, (long)0);
}

static inline long sl_sys2(long nr, long a0, long a1) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)0L, (long)0);
}

static inline long sl_sys3(long nr, long a0, long a1, long a2) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)0);
}

static inline long sl_sys4(long nr, long a0, long a1, long a2, long a3) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)a3);
}

/* Release a capability: delete its slot.  Stage 4 removed the handle branch —
 * every capability the loader creates is published into its workspace. */
static inline void sl_close_cap(handle_id_t h) {
    if (h == HANDLE_INVALID) return;
    (void)sl_sys2(SYS_CNODE_DELETE, (long)((uint32_t)h & 0xFFu),
                  (long)((uint32_t)h >> 8));
}

/* ── Minimal ELF64 types ─────────────────────────────────────────── */

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    uint8_t    e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Xword d_tag;
    Elf64_Xword d_val;
} Elf64_Dyn;

typedef struct {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

#define ET_DYN              3u
#define EM_X86_64           62u
#define PT_LOAD             1u
#define PT_DYNAMIC          2u
#define DT_NULL             0
#define DT_RELA             7
#define DT_RELASZ           8
#define DT_RELAENT          9
#define PF_X                1u
#define PF_W                2u
#define ELF64_R_TYPE(i)     ((uint32_t)((i) & 0xffffffffULL))
#define R_X86_64_RELATIVE   8u

/* ── Temp VMO window layout in loader's address space ─────────────── */

/* Slot 0 (ELF raw): [USER_VMO_BASE, USER_VMO_BASE + 4MB)  — read-only */
#define SL_ELF_VADDR      USER_VMO_BASE
/* Slots 1..N (segments): [USER_VMO_BASE + 4MB, +4MB each) — writable  */
#define SL_SEG_VADDR_BASE (USER_VMO_BASE + 0x400000ULL)
#define SL_SEG_SLOT_SIZE  0x400000ULL   /* 4 MB per segment slot */
#define SL_MAX_SEGS       8u

/* ── Utilities ───────────────────────────────────────────────────── */

static void sl_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static void sl_memzero(void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

static uint64_t sl_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint64_t sl_xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *state = x;
    return x;
}

/* ── ASLR bias: pick page-aligned offset in [USER_TEXT_BASE, USER_VMO_BASE - 16MB - vend) */
static uint64_t sl_choose_bias(uint64_t max_vend) {
    const uint64_t page = 4096ULL;
    uint64_t bias_min = USER_TEXT_BASE;
    uint64_t guard    = 0x1000000ULL + max_vend;   /* 16 MB + segment span */
    if (USER_VMO_BASE <= bias_min + guard)
        return bias_min;
    uint64_t range = (USER_VMO_BASE - bias_min - guard) & ~(page - 1ULL);
    if (range == 0) return bias_min;
    uint64_t seed = sl_rdtsc();
    if (seed == 0) seed = 0xdeadbeefcafe0001ULL;
    uint64_t rand = sl_xorshift64(&seed);
    return (bias_min + (rand % range)) & ~(page - 1ULL);
}

static uint64_t sl_page_floor(uint64_t v) {
    return v & ~0xFFFULL;
}

static uint64_t sl_page_ceil(uint64_t v) {
    return (v + 0xFFFULL) & ~0xFFFULL;
}

/* Find file offset of a given vaddr in an ET_DYN (base-0) ELF. */
static uint64_t sl_vaddr_to_foff(const Elf64_Ehdr *eh, uint64_t vaddr) {
    const Elf64_Phdr *phdrs =
        (const Elf64_Phdr *)(uintptr_t)(SL_ELF_VADDR + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (vaddr < ph->p_vaddr || vaddr >= ph->p_vaddr + ph->p_filesz) continue;
        return ph->p_offset + (vaddr - ph->p_vaddr);
    }
    return (uint64_t)-1;
}

/* ── Initrd name→index catalog (must match kernel/core/initrd/initrd.c) ── */
/*
 * Implemented as an if/else chain rather than a pointer table.  In a flat
 * binary (userboot) there is no dynamic linker to apply R_X86_64_RELATIVE
 * relocations, so any struct whose fields hold absolute string pointers would
 * carry wrong addresses at runtime.  Inline string literals use RIP-relative
 * LEA and are always correct regardless of load address.
 */

static int sl_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static long sl_name_to_index(const char *name) {
    if (sl_streq(name, "userboot")) return 0;
    if (sl_streq(name, "init"))     return 1;
    if (sl_streq(name, "svcmgr"))   return 2;
    if (sl_streq(name, "kbd"))      return 3;
    if (sl_streq(name, "vfs"))      return 4;
    if (sl_streq(name, "console"))  return 5;
    if (sl_streq(name, "fb"))       return 6;
    if (sl_streq(name, "sh"))        return 7;
    if (sl_streq(name, "iris_test")) return 8;
    if (sl_streq(name, "lifecycle_probe")) return 9;
    if (sl_streq(name, "pager"))    return 10;
    if (sl_streq(name, "badelf"))   return 11;
    return -1;
}

/* ── svc_load ────────────────────────────────────────────────────── */

long svc_initrd_count(uint64_t initrd_c) {
    return sl_sys2(SYS_INITRD_COUNT, (long)initrd_c, 0);
}

long svc_load(uint64_t proc_c, uint64_t initrd_c, const char *name,
              handle_id_t *out_proc_h, handle_id_t *out_chan_h) {
    return svc_load_minted(proc_c, initrd_c, name, out_proc_h, out_chan_h, 0, 0);
}

/* Legacy arity: RETIRED (Stage 4).  It existed so a caller with nowhere to put
 * the child's capabilities could take them as handles; every spawner supplies
 * a workspace CNode now, so the handle path underneath had no callers left.
 * Kept as a hard failure rather than deleted: a caller that reaches here has
 * skipped the workspace, and a silent fall back to handles is exactly what
 * this stage removes. */
long svc_load_minted(uint64_t proc_c, uint64_t initrd_c, const char *name,
                     handle_id_t *out_proc_h, handle_id_t *out_chan_h,
                     const struct svc_mint *mints, uint32_t mint_count) {
    (void)proc_c; (void)initrd_c; (void)name; (void)mints; (void)mint_count;
    if (out_proc_h) *out_proc_h = HANDLE_INVALID;
    if (out_chan_h) *out_chan_h = HANDLE_INVALID;
    return (long)IRIS_ERR_NOT_SUPPORTED;
}

/* ── Etapa 4: loader workspace ─────────────────────────────────────────────
 *
 * A spawn needs up to eleven capabilities alive at once (ELF image, process,
 * stack VMO, one per segment).  Publishing them into CSpace rather than the
 * handle table therefore needs eleven slots, and no spawning service has that
 * many free in its root CNode — svcmgr has two.  So the loader uses a
 * SECOND-LEVEL CNode: one root slot, 256 leaves, addressed (leaf << 8) | slot.
 *
 * The workspace is a PARAMETER, not loader state.  userboot is a flat binary
 * with no writable .data at all — its linker script says so — so anything the
 * loader remembers between calls is a fault waiting for its first caller.
 * `ws` packs the untyped to carve from in the low 32 bits and the root slot in
 * the high 32; zero means "no workspace", which keeps the legacy handle path.
 */
#define SL_WS_ELF    1u
#define SL_WS_PROC_BASE 16u  /* 16..99: one live process each */
#define SL_WS_PROC_LIMIT 100u
/* Stage 6 Etapa 5: one recyclable budget per live child, paired with its
 * process leaf (leaf L uses budget leaf 100 + L - 16).
 *
 * Everything a child costs — its address space, its process state, its segment
 * and stack VMOs — is carved from this, so when the child dies and its last
 * capability goes, the budget has no children left and RESET makes the whole
 * region reusable.  Without it, consumption is monotonic: a bump allocator
 * never rewinds, so every spawn would permanently spend a few hundred KiB of
 * the spawner's pool however short-lived the child was.  This is seL4's
 * reclamation pattern — revoke the Untyped you used for a child — in the form
 * IRIS has. */
#define SL_WS_CHILDPOOL(leaf) ((leaf) - SL_WS_PROC_BASE + SL_WS_PROC_LIMIT)
#define SL_CHILD_POOL_BYTES (1u << 20)
#define SL_WS_STACK  3u
/* Stage 6 Etapa 5: a reusable scratch budget for the ELF image copy.
 *
 * The kernel copies the whole image when SYS_INITRD_VMO is invoked, and the
 * loader drops it as soon as the segments are out.  Charged to the spawner's
 * main pool that transient copy would be permanent — a bump allocator does not
 * rewind — so every spawn would cost an image forever.  Pointing it at a
 * dedicated sub-untyped and RESETTING that between spawns bounds the cost at
 * ONE image, which is the seL4 reclamation pattern (revoke the Untyped you
 * used) in the form IRIS has. */
#define SL_WS_ELFPOOL 5u
/* Stage 6-pure Etapa 2: the child's VSpace, and one scratch slot the paging
 * levels pass through on their way into it.  One slot is enough for any depth
 * — installing hands the VSpace its own reference, so the capability here is
 * spent the moment it lands (see iris_vspace.h). */
#define SL_WS_CHILD_VSPACE 6u   /* the address space the loader RETYPES for it */
/* Two scratch slots, because the spare level a completed walk leaves behind
 * belongs to whoever paid for it.  The loader's own spare is retyped from the
 * loader's budget and is worth KEEPING across spawns; a child's spare is
 * charged to the CHILD's budget, and holding it past the spawn would keep a
 * child entry alive on a region svcmgr must be able to RESET when it restarts
 * that service.  Same reason, opposite lifetime — so, two slots. */
#define SL_WS_PTSCRATCH    7u   /* the loader's own levels */
#define SL_WS_PTSCRATCH_CH 2u   /* a child's levels; released at end of load */
/* The loader's OWN address space.  It maps every image it loads into itself to
 * parse it, so once the kernel stopped creating levels the loader owes its own
 * walk exactly as it owes the child's — and a loader running inside a spawned
 * service (init, svcmgr) has a budget, so it is not exempt. */
#define SL_WS_SELF_VSPACE  4u
#define SL_ELF_POOL_BYTES (4u << 20)
#define SL_WS_SEG    8u   /* 8..15: one per ELF segment */

#define SL_WS_UNTYPED(ws)  ((uint64_t)((ws) & 0xFFFFFFFFu))
#define SL_WS_SLOT(ws)     ((uint32_t)((ws) >> 32))

/* CPtr of leaf `n` inside the workspace CNode. */
static inline uint32_t sl_ws_cptr(uint64_t ws, uint32_t n) {
    return (uint32_t)((n << 8) | SL_WS_SLOT(ws));
}

/* Destination encoding for a publishing syscall: CNode | slot << 32. */
static inline long sl_ws_dest(uint64_t ws, uint32_t leaf) {
    return (long)((uint64_t)SL_WS_SLOT(ws) | ((uint64_t)leaf << 32));
}

/* Make sure the workspace CNode exists.  Stateless: retyping into an occupied
 * slot reports ALREADY_EXISTS, which is simply "it is already there" — the
 * loader cannot cache that answer because it may have no writable memory. */
static int sl_ws_ensure(uint64_t ws) {
    if (SL_WS_SLOT(ws) == 0u || SL_WS_UNTYPED(ws) == 0u) return 0;
    long r = sl_sys4(SYS_UNTYPED_RETYPE2, (long)SL_WS_UNTYPED(ws),
                     (long)((uint64_t)IRIS_KOBJ_CNODE | (1ULL << 32)),
                     (long)((uint64_t)SL_WS_SLOT(ws) << 32), 256);
    return (r == 0 || r == (long)IRIS_ERR_ALREADY_EXISTS);
}

long svc_load_minted_ws(uint64_t proc_c, uint64_t initrd_c, const char *name,
                        handle_id_t *out_proc_h, handle_id_t *out_chan_h,
                        const struct svc_mint *mints, uint32_t mint_count,
                        uint64_t ws, uint64_t child_budget,
                        uint32_t own_budget_slot) {
    *out_proc_h = HANDLE_INVALID;
    *out_chan_h = HANDLE_INVALID;
    long self_vs  = 0;   /* the loader's own address space, for parse windows */
    long child_vs = 0;   /* the child's address space, once it exists */
    long pool_c   = 0;   /* the budget its paging levels come from */

    long idx = sl_name_to_index(name);
    if (idx < 0) return (long)IRIS_ERR_NOT_FOUND;

    /* State tracking for cleanup */
    int         elf_mapped     = 0;
    uint32_t    segs_in_loader = 0;  /* bitmask of loader-mapped seg slots */
    handle_id_t elf_h          = HANDLE_INVALID;
    handle_id_t proc_h         = HANDLE_INVALID;
    handle_id_t ch_h           = HANDLE_INVALID;
    handle_id_t stack_vmo_h    = HANDLE_INVALID;
    handle_id_t seg_vmo[SL_MAX_SEGS];
    uint32_t    seg_count = 0;
    uint64_t    seg_p_vaddr [SL_MAX_SEGS];
    uint64_t    seg_p_memsz [SL_MAX_SEGS];
    uint64_t    seg_p_filesz[SL_MAX_SEGS];
    uint64_t    seg_p_offset[SL_MAX_SEGS];
    uint32_t    seg_p_flags [SL_MAX_SEGS];
    uint64_t    seg_map_base [SL_MAX_SEGS];
    uint64_t    seg_map_size [SL_MAX_SEGS];
    uint64_t    seg_page_off [SL_MAX_SEGS];
    long        r = (long)IRIS_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < SL_MAX_SEGS; i++) {
        seg_vmo[i]     = HANDLE_INVALID;
        seg_p_memsz[i] = 0;
        seg_map_base[i] = 0;
        seg_map_size[i] = 0;
        seg_page_off[i] = 0;
    }

    /* Stage 4: a workspace is mandatory — every capability this spawn creates
     * is published into a leaf of it and named by CPtr.  The alternative was the handle path,
     * which is gone; failing here names the missing argument instead of
     * quietly producing capabilities the caller cannot address. */
    if (!sl_ws_ensure(ws)) return (long)IRIS_ERR_INVALID_ARG;

    /* The loader's own address space, for the parse windows below.  Not fatal
     * if it cannot be had: the ONE address space with no budget is the root
     * task's, whose maps the kernel still funds, and that is also the only
     * caller for which SYS_VSPACE_SELF is not needed. */
    (void)sl_sys2(SYS_CNODE_DELETE, (long)SL_WS_SLOT(ws), (long)SL_WS_SELF_VSPACE);
    if (sl_sys1(SYS_VSPACE_SELF, sl_ws_dest(ws, SL_WS_SELF_VSPACE)) == 0)
        self_vs = (long)sl_ws_cptr(ws, SL_WS_SELF_VSPACE);

    /* 1. Get read-only eager VMO wrapping the ELF bytes in the initrd.
     *
     * Charged to the scratch budget: RESET it first (the previous spawn's copy
     * is long gone, so it has no children), and carve it on the first spawn. */
    {
        long pool = (long)sl_ws_cptr(ws, SL_WS_ELFPOOL);
        long pr   = sl_sys1(SYS_UNTYPED_RESET, pool);
        if (pr == (long)IRIS_ERR_BUSY) {
            /* The previous image is somehow still alive.  Charging this one to
             * the caller's own budget costs an image; deleting and re-carving
             * would cost the whole scratch region, permanently — the same
             * stranding the child budgets above refuse to do. */
            pool = 0;
        } else if (pr != 0) {
            if (sl_sys4(SYS_UNTYPED_RETYPE2, (long)SL_WS_UNTYPED(ws),
                        (long)((uint64_t)IRIS_KOBJ_UNTYPED | (1ULL << 32)),
                        sl_ws_dest(ws, SL_WS_ELFPOOL),
                        (long)SL_ELF_POOL_BYTES) != 0)
                pool = 0;   /* fall back to the caller's own budget */
        }
        r = sl_sys4(SYS_INITRD_VMO, (long)initrd_c, idx,
                    sl_ws_dest(ws, SL_WS_ELF), pool);
        if (r < 0) goto out;
        elf_h = (handle_id_t)sl_ws_cptr(ws, SL_WS_ELF);
    }

    /* 2. Map ELF read-only at SL_ELF_VADDR for parsing.  The levels for the
     *    parse window are the loader's own to supply now. */
    r = iris_vspace_map(SYS_VMO_MAP, (long)elf_h, (long)SL_ELF_VADDR, 0, 0,
                        self_vs, (long)SL_WS_UNTYPED(ws),
                        sl_ws_dest(ws, SL_WS_PTSCRATCH),
                        (long)sl_ws_cptr(ws, SL_WS_PTSCRATCH),
                        SL_ELF_VADDR);
    if (r < 0) goto out;
    elf_mapped = 1;

    {
        const Elf64_Ehdr *eh  = (const Elf64_Ehdr *)(uintptr_t)SL_ELF_VADDR;
        const Elf64_Phdr *phs = (const Elf64_Phdr *)(uintptr_t)
                                    (SL_ELF_VADDR + eh->e_phoff);

        /* 3. Validate ELF header. */
        r = (long)IRIS_ERR_INVALID_ARG;
        if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
            eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') goto out;
        if (eh->e_type    != (Elf64_Half)ET_DYN)    goto out;
        if (eh->e_machine != (Elf64_Half)EM_X86_64) goto out;
        if (eh->e_phnum   == 0 || eh->e_phentsize <
                (Elf64_Half)sizeof(Elf64_Phdr))      goto out;

        /* 4. Collect PT_LOAD segments; compute max virtual end for bias. */
        uint64_t max_vend = 0;
        for (uint16_t i = 0; i < eh->e_phnum && seg_count < SL_MAX_SEGS; i++) {
            const Elf64_Phdr *ph = &phs[i];
            uint64_t map_base, map_end, map_size, page_off;
            if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
            if (ph->p_filesz > ph->p_memsz) goto out;
            map_base = sl_page_floor(ph->p_vaddr);
            map_end  = sl_page_ceil(ph->p_vaddr + ph->p_memsz);
            map_size = map_end - map_base;
            page_off = ph->p_vaddr - map_base;
            if (map_size == 0 || map_size > SL_SEG_SLOT_SIZE) goto out;
            seg_p_vaddr [seg_count] = ph->p_vaddr;
            seg_p_memsz [seg_count] = ph->p_memsz;
            seg_p_filesz[seg_count] = ph->p_filesz;
            seg_p_offset[seg_count] = ph->p_offset;
            seg_p_flags [seg_count] = ph->p_flags;
            seg_map_base [seg_count] = map_base;
            seg_map_size [seg_count] = map_size;
            seg_page_off [seg_count] = page_off;
            {
                uint64_t vend = map_base + map_size;
                if (vend < map_base) goto out;
                if (vend > max_vend) max_vend = vend;
            }
            seg_count++;
        }
        if (seg_count == 0) goto out;

        /* 5. Choose page-aligned ASLR bias. */
        uint64_t bias = sl_choose_bias(max_vend);

        /* 5b. Create the empty target process FIRST (Fase 29): the child must
         * exist before its image VMOs so those VMOs can be charged to the CHILD
         * (its own resource domain), not to the loader.  The loader passes the
         * child process cap as the VMO charge-target; it holds RIGHT_MANAGE on
         * the process it just created.  This is the root-cause fix for the
         * caller-charged accounting bug — the loader's own_vmos / phys_pages
         * stay flat regardless of how many children it launches. */
        /* The process capability is HANDED BACK to the caller, so unlike every
         * other workspace leaf it is not released when this returns — it lives
         * as long as the service does.  A fixed leaf would therefore collide
         * with the previous spawn's process.  Scan for a free one instead:
         * publication is exclusive, so ALREADY_EXISTS simply means "taken",
         * and the loader keeps no state to remember where it got to. */
        /* Stage 6 Etapa 2: the child's page tables are charged to the
         * spawner's own Untyped — the one this workspace already carves from.
         *
         * A per-child sub-untyped was the first design and it was wrong: a
         * bump allocator never rewinds, so every spawn permanently consumed a
         * whole budget whether the child used it or not, and a churn workload
         * exhausted an 8 MiB pool in tens of spawns.  Charging the real cost
         * (about five pages per address space) to the pool the spawner already
         * manages is both cheaper and more honest — and the kernel counts each
         * table as a child of that pool, so the spawner cannot RESET it out
         * from under a live child. */
        uint32_t proc_leaf = 0u;
        /* The budget the child's address space is built from — its paging
         * levels come out of the same one (Stage 6-pure Etapa 2). */
        pool_c = 0;
        for (uint32_t l = SL_WS_PROC_BASE; l < SL_WS_PROC_LIMIT; l++) {
            /* Ask whether this leaf is taken BEFORE touching its budget: a
             * leaf whose process is still alive owns a budget that is still in
             * use, and resetting or re-carving it would strand the live
             * child's memory.  SYS_CAP_IDENTIFY answers exactly this question
             * and takes no authority to ask. */
            if (sl_sys1(SYS_CAP_IDENTIFY, (long)sl_ws_cptr(ws, l)) >= 0)
                continue;

            /* Prepare this leaf's child budget.
             *
             * BUSY is not a reason to re-carve — it is the answer to a
             * question the leaf scan above cannot ask.  A spawner may drop the
             * process capability while the child is still RUNNING (init does
             * exactly that for fb, console, svcmgr and the suite), which frees
             * the leaf while its budget is still feeding a live address space.
             * Deleting the slot and carving a fresh region then strands the
             * old one for good: no capability names it any more and a bump
             * allocator never rewinds, so it can never be RESET.  Every spawn
             * would burn another budget.
             *
             * So BUSY means "this leaf is occupied after all" and the scan
             * moves on.  A budget becomes reclaimable when its child actually
             * dies, and the leaf becomes reusable at the same moment — which
             * is the property the scan wanted in the first place. */
            uint32_t pl   = SL_WS_CHILDPOOL(l);
            long     pool = (long)sl_ws_cptr(ws, pl);
            long     rr   = sl_sys1(SYS_UNTYPED_RESET, pool);
            if (rr == (long)IRIS_ERR_BUSY) continue;
            if (rr != 0) {
                /* Nothing in the slot: first use of this leaf. */
                if (sl_sys4(SYS_UNTYPED_RETYPE2, (long)SL_WS_UNTYPED(ws),
                            (long)((uint64_t)IRIS_KOBJ_UNTYPED | (1ULL << 32)),
                            sl_ws_dest(ws, pl),
                            (long)(child_budget ? child_budget
                                                : (uint64_t)SL_CHILD_POOL_BYTES)) != 0) {
                    r = (long)IRIS_ERR_NO_MEMORY;
                    break;
                }
            }
            /*
             * Stage 6-pure Etapa 4: the loader RETYPES the child's address
             * space and hands it over, instead of handing over a budget for
             * the kernel to build one from.  A process is composed out of
             * objects its creator made.
             */
            (void)sl_sys2(SYS_CNODE_DELETE, (long)SL_WS_SLOT(ws),
                          (long)SL_WS_CHILD_VSPACE);
            if (sl_sys4(SYS_UNTYPED_RETYPE2, pool,
                        (long)((uint64_t)IRIS_KOBJ_VSPACE | (1ULL << 32)),
                        sl_ws_dest(ws, SL_WS_CHILD_VSPACE), 4096) != 0) {
                r = (long)IRIS_ERR_NO_MEMORY;
                break;
            }
            child_vs = (long)sl_ws_cptr(ws, SL_WS_CHILD_VSPACE);
            r = sl_sys3(SYS_PROCESS_CREATE, (long)proc_c,
                        sl_ws_dest(ws, l), child_vs);
            if (r == 0) { proc_leaf = l; pool_c = pool; break; }
            if (r != (long)IRIS_ERR_ALREADY_EXISTS) break;
        }
        if (proc_leaf == 0u && r == (long)IRIS_ERR_ALREADY_EXISTS)
            r = (long)IRIS_ERR_NO_MEMORY;
        if (r < 0) goto out;
        proc_h = (handle_id_t)sl_ws_cptr(ws, proc_leaf);

        /* 6. Create a sparse VMO for each segment (populated eagerly at map),
         * charged to the child.  Mapped into the loader's temp window below to
         * fill; the phys pages are charged to the child (VMO owner), so
         * unmapping from the loader never strands the charge on the loader. */
        for (uint32_t i = 0; i < seg_count; i++) {
            r = sl_sys3(SYS_VMO_CREATE_FOR, (long)seg_map_size[i],
                        (long)proc_h, sl_ws_dest(ws, SL_WS_SEG + i));
            if (r < 0) goto out;
            seg_vmo[i] = (handle_id_t)sl_ws_cptr(ws, SL_WS_SEG + i);
        }

        /* 7. Map each segment VMO writable in loader's temp window. */
        for (uint32_t i = 0; i < seg_count; i++) {
            uint64_t slot = SL_SEG_VADDR_BASE + (uint64_t)i * SL_SEG_SLOT_SIZE;
            for (uint64_t off = 0; off < seg_map_size[i] + 0x200000ULL;
                 off += 0x200000ULL) {
                r = iris_vspace_ensure(self_vs, (long)SL_WS_UNTYPED(ws),
                                       sl_ws_dest(ws, SL_WS_PTSCRATCH),
                                       (long)sl_ws_cptr(ws, SL_WS_PTSCRATCH),
                                       slot + off);
                if (r < 0) goto out;
                if (off >= seg_map_size[i]) break;
            }
            r = sl_sys3(SYS_VMO_MAP, (long)seg_vmo[i], (long)slot, 1 /*WRITABLE*/);
            if (r < 0) goto out;
            segs_in_loader |= (1u << i);
        }

        /* 8. Copy ELF file data into each segment slot.  SYS_VMO_MAP eagerly
         *    allocated and installed all pages, so this is a direct write. */
        for (uint32_t i = 0; i < seg_count; i++) {
            uint64_t slot = SL_SEG_VADDR_BASE + (uint64_t)i * SL_SEG_SLOT_SIZE;
            uint8_t *dst  = (uint8_t *)(uintptr_t)(slot + seg_page_off[i]);
            const uint8_t *src =
                (const uint8_t *)(uintptr_t)(SL_ELF_VADDR + seg_p_offset[i]);
            if (seg_p_filesz[i] > 0)
                sl_memcpy(dst, src, seg_p_filesz[i]);
            if (seg_p_memsz[i] > seg_p_filesz[i])
                sl_memzero(dst + seg_p_filesz[i],
                           seg_p_memsz[i] - seg_p_filesz[i]);
        }

        /* 9–10. Find PT_DYNAMIC and apply R_X86_64_RELATIVE relocations. */
        for (uint16_t pi = 0; pi < eh->e_phnum; pi++) {
            const Elf64_Phdr *ph = &phs[pi];
            if (ph->p_type != PT_DYNAMIC) continue;

            const Elf64_Dyn *dyn =
                (const Elf64_Dyn *)(uintptr_t)(SL_ELF_VADDR + ph->p_offset);
            uint64_t rela_vaddr = 0, rela_sz = 0, rela_ent = sizeof(Elf64_Rela);

            for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
                if (d->d_tag == DT_RELA)    rela_vaddr = d->d_val;
                else if (d->d_tag == DT_RELASZ)  rela_sz   = d->d_val;
                else if (d->d_tag == DT_RELAENT) rela_ent  = d->d_val;
            }
            if (rela_sz == 0 || rela_ent == 0) break;

            uint64_t rela_foff = sl_vaddr_to_foff(eh, rela_vaddr);
            if (rela_foff == (uint64_t)-1) goto out;

            const uint8_t *rela_raw =
                (const uint8_t *)(uintptr_t)(SL_ELF_VADDR + rela_foff);
            uint64_t n_rela = rela_sz / rela_ent;

            for (uint64_t ri = 0; ri < n_rela; ri++) {
                const Elf64_Rela *rel =
                    (const Elf64_Rela *)(const void *)(rela_raw + ri * rela_ent);
                if (ELF64_R_TYPE(rel->r_info) != R_X86_64_RELATIVE) continue;

                /* Find segment covering r_offset (the relocation target rva) */
                uint32_t si;
                for (si = 0; si < seg_count; si++) {
                    if (rel->r_offset >= seg_p_vaddr[si] &&
                        rel->r_offset <  seg_p_vaddr[si] + seg_p_memsz[si])
                        break;
                }
                r = (long)IRIS_ERR_INVALID_ARG;
                if (si >= seg_count) goto out;

                /* Loader address of the patch site */
                uint64_t slot = SL_SEG_VADDR_BASE + (uint64_t)si * SL_SEG_SLOT_SIZE;
                uint8_t *patch = (uint8_t *)(uintptr_t)
                                     (slot + rel->r_offset - seg_map_base[si]);
                uint64_t val = bias + (uint64_t)rel->r_addend;
                sl_memcpy(patch, &val, sizeof(val));
            }
            break;  /* one PT_DYNAMIC only */
        }

        /* Cache ELF header fields needed after the ELF is unmapped. */
        uint64_t elf_entry = eh->e_entry;

        /* 11. Unmap ELF from loader — no longer needed. */
        sl_sys2(SYS_VMO_UNMAP, (long)SL_ELF_VADDR, (long)SL_SEG_SLOT_SIZE);
        elf_mapped = 0;
        sl_close_cap(elf_h);
        elf_h = HANDLE_INVALID;

        /* 12. Target process created earlier (step 5b) so its image VMOs are
         * charged to it (Fase 29).
         * Fase 13 (Track I): the per-child bootstrap KChannel is retired — every
         * cap is a pre-start CSpace mint, so no channel is created or inserted
         * and the child starts with RBX = 0 (no bootstrap handle). */

        /*
         * Stage 6-pure Etapa 2: the child's paging levels are the child's, so
         * they are retyped from the CHILD's budget and installed into the
         * CHILD's address space.  The loader needs a capability to that
         * address space to do it — which is what SYS_PROCESS_VSPACE is for,
         * and the reason it exists at all now that the kernel no longer
         * creates levels on the child's behalf.
         */
        /* child_vs is already in hand: the loader retyped it above and the
         * process was composed from it.  SYS_PROCESS_VSPACE is no longer the
         * way to reach a child's address space at spawn time — the spawner
         * made it, so it never stopped holding it. */

        /* 14. Create user stack sparse VMO (charged to the child) and map it in. */
        r = sl_sys3(SYS_VMO_CREATE_FOR, (long)USER_STACK_SIZE,
                    (long)proc_h, sl_ws_dest(ws, SL_WS_STACK));
        if (r < 0) goto out;
        stack_vmo_h = (handle_id_t)sl_ws_cptr(ws, SL_WS_STACK);

        r = iris_vspace_map(SYS_VMO_MAP_INTO,
                            (long)stack_vmo_h, (long)proc_h,
                            (long)USER_STACK_BASE, 1 /*WRITABLE*/,
                            child_vs, pool_c,
                            sl_ws_dest(ws, SL_WS_PTSCRATCH_CH),
                            (long)sl_ws_cptr(ws, SL_WS_PTSCRATCH_CH),
                            USER_STACK_BASE);
        if (r < 0) goto out;

        /* 15. Map each segment sparse VMO into child with correct W^X flags. */
        for (uint32_t i = 0; i < seg_count; i++) {
            long flags = 0;
            if (seg_p_flags[i] & PF_W) flags |= 1; /* WRITABLE */
            if (seg_p_flags[i] & PF_X) flags |= 2; /* EXEC     */
            /* W^X: if both PF_W and PF_X, clear EXEC — code shouldn't be writable */
            if ((seg_p_flags[i] & (PF_W | PF_X)) == (PF_W | PF_X)) flags = 1;
            /* A segment can straddle levels, so every 2 MiB the walk covers is
             * ensured — starting from the 2 MiB FLOOR of the segment, not from
             * its first byte.  The image is loaded at an ASLR bias, so a
             * segment that begins just below a boundary needs the table on the
             * far side of it too, and stepping from the unaligned start would
             * skip exactly that one. */
            uint64_t sv0 = (bias + seg_map_base[i]) & ~0x1FFFFFULL;
            uint64_t sv1 = bias + seg_map_base[i] + seg_map_size[i];
            for (uint64_t va = sv0; va < sv1; va += 0x200000ULL) {
                r = iris_vspace_ensure(child_vs, pool_c,
                                       sl_ws_dest(ws, SL_WS_PTSCRATCH_CH),
                                       (long)sl_ws_cptr(ws, SL_WS_PTSCRATCH_CH),
                                       va);
                if (r < 0) goto out;
            }
            r = sl_sys4(SYS_VMO_MAP_INTO,
                        (long)seg_vmo[i], (long)proc_h,
                        (long)(bias + seg_map_base[i]), flags);
            if (r < 0) goto out;
        }

        /* 16. Unmap all segment slots from loader (pages[] remain in VMOs). */
        for (uint32_t i = 0; i < seg_count; i++) {
            if (!(segs_in_loader & (1u << i))) continue;
            uint64_t slot = SL_SEG_VADDR_BASE + (uint64_t)i * SL_SEG_SLOT_SIZE;
            sl_sys2(SYS_VMO_UNMAP, (long)slot, (long)seg_p_memsz[i]);
            segs_in_loader &= ~(1u << i);
        }

        /* 17. Release temporary handles — VMOs kept alive by child's mappings. */
        sl_close_cap(stack_vmo_h); stack_vmo_h = HANDLE_INVALID;
        for (uint32_t i = 0; i < seg_count; i++) {
            sl_close_cap(seg_vmo[i]); seg_vmo[i] = HANDLE_INVALID;
        }

        /* 18. (Track I) No bootstrap channel to insert — the child gets RBX = 0. */

        /*
         * Stage 6-pure Etapa 2: every child gets a capability to the budget
         * its own address space was built from.
         *
         * The kernel no longer creates paging levels, so a task that maps
         * anything has to be able to retype one — and the level for its own
         * address space belongs in the region that address space is already
         * charged to.  Without this the child could hold a VSpace capability
         * and still be unable to map, which is not a restriction anyone chose.
         *
         * Not a new authority: the region is the child's already, and its
         * spawner keeps the parent capability, so revoking still reaches it.
         */
        if (own_budget_slot && pool_c) {
            /* Publication is exclusive, so a manifest that already names this
             * slot would make the mint fail and leave the child with neither.
             * The spawner asked for both; the manifest is the more specific
             * request, so it wins and this is skipped. */
            int taken = 0;
            for (uint32_t mi = 0; mints && mi < mint_count; mi++)
                if (mints[mi].slot == own_budget_slot) taken = 1;
            if (!taken)
                (void)sl_sys4(SYS_CSPACE_MINT_INTO, (long)proc_h,
                              (long)own_budget_slot, pool_c,
                              (long)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE));
        }

        /* 18b (Fase 8). Mint the well-known CSpace slots BEFORE the first
         * thread starts: the child sees its slots populated from its first
         * instruction — no bootstrap barrier, no retry loop, no race.
         * Mint failures are deliberately non-fatal (consumers gate loudly
         * in smoke); invalid sources are skipped. */
        for (uint32_t mi = 0; mi < mint_count; mi++) {
            if (!mints) continue;
            uint64_t rb = (mints[mi].badge << 32) | (uint64_t)mints[mi].rights;
            if (mints[mi].src_cptr != 0u) {
                /* Fase S4: CSpace-sourced delegation — the child's cap is an
                 * MDB child of our slot, so SYS_CSPACE_REVOKE on it reaches
                 * into the child. */
                (void)sl_sys4(SYS_CSPACE_MINT_INTO,
                              (long)proc_h,
                              (long)mints[mi].slot,
                              (long)mints[mi].src_cptr,
                              (long)rb);
                continue;
            }
            if (mints[mi].src_h == HANDLE_INVALID) continue;
            (void)sl_sys4(SYS_PROC_CSPACE_MINT,
                          (long)proc_h,
                          (long)mints[mi].slot,
                          (long)mints[mi].src_h,
                          (long)rb);
        }

        /* 19. Start first thread: entry at bias+e_entry, RSP = stack top - 8,
         *     RBX = 0 (Track I: no bootstrap channel; every cap is a CSpace mint). */
        r = sl_sys4(SYS_THREAD_START,
                    (long)proc_h,
                    (long)(bias + elf_entry),
                    (long)(USER_STACK_TOP - 8ULL),
                    0);
        if (r < 0) goto out;
    }

    /* Hand back everything of the CHILD's that the loader was holding: an
     * unused level charged to its budget, and the capability to its address
     * space.  Both would outlive the child otherwise — a VSpace capability
     * keeps that address space alive after the process dies, and with it every
     * page table installed in it, which is a child entry on a budget its owner
     * is entitled to RESET the moment the child is gone. */
    (void)sl_sys2(SYS_CNODE_DELETE, (long)SL_WS_SLOT(ws), (long)SL_WS_PTSCRATCH_CH);
    (void)sl_sys2(SYS_CNODE_DELETE, (long)SL_WS_SLOT(ws), (long)SL_WS_CHILD_VSPACE);
    *out_proc_h = proc_h;
    *out_chan_h  = HANDLE_INVALID;
    return 0;

out:
    (void)sl_sys2(SYS_CNODE_DELETE, (long)SL_WS_SLOT(ws), (long)SL_WS_PTSCRATCH_CH);
    (void)sl_sys2(SYS_CNODE_DELETE, (long)SL_WS_SLOT(ws), (long)SL_WS_CHILD_VSPACE);
    /* Unmap ELF if still mapped. */
    if (elf_mapped)
        sl_sys2(SYS_VMO_UNMAP, (long)SL_ELF_VADDR, (long)SL_SEG_SLOT_SIZE);
    /* Unmap any segment slots still mapped in loader. */
    for (uint32_t i = 0; i < SL_MAX_SEGS; i++) {
        if (!(segs_in_loader & (1u << i))) continue;
        uint64_t slot = SL_SEG_VADDR_BASE + (uint64_t)i * SL_SEG_SLOT_SIZE;
        if (seg_p_memsz[i] > 0)
            sl_sys2(SYS_VMO_UNMAP, (long)slot, (long)seg_p_memsz[i]);
    }
    /* Close all handles. */
    sl_close_cap(elf_h);
    sl_close_cap(stack_vmo_h);
    sl_close_cap(proc_h);
    sl_close_cap(ch_h);
    for (uint32_t i = 0; i < SL_MAX_SEGS; i++) sl_close_cap(seg_vmo[i]);
    return r;
}
