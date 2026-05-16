/*
 * svc_loader.c — ring-3 ELF loader for IRIS services.
 *
 * Implements svc_load() using the Phase 29 composable spawn primitives:
 *   SYS_INITRD_VMO(55) + SYS_PROCESS_CREATE(56) + SYS_VMO_MAP_INTO(57) +
 *   SYS_THREAD_START(58) + SYS_HANDLE_INSERT(59)
 *
 * Supports ET_DYN (static PIE, base=0) ELF64 x86-64 with R_X86_64_RELATIVE
 * RELA relocations.  RDTSC-seeded Xorshift64 ASLR bias applied per spawn.
 */

#include "svc_loader.h"
#include <iris/syscall.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/paging.h>

/* ── Freestanding syscall helpers ─────────────────────────────────── */

static inline long sl_sys0(long nr) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long sl_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long sl_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long sl_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long sl_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}

static inline void sl_close(handle_id_t h) {
    if (h != HANDLE_INVALID)
        sl_sys1(SYS_HANDLE_CLOSE, (long)h);
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
    if (sl_streq(name, "sh"))       return 7;
    return -1;
}

/* ── svc_load ────────────────────────────────────────────────────── */

long svc_initrd_count(handle_id_t spawn_cap_h) {
    return sl_sys2(SYS_INITRD_COUNT, (long)spawn_cap_h, 0);
}

long svc_load(handle_id_t spawn_cap_h, const char *name,
              handle_id_t *out_proc_h, handle_id_t *out_chan_h) {
    *out_proc_h = HANDLE_INVALID;
    *out_chan_h = HANDLE_INVALID;

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
    long        r = (long)IRIS_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < SL_MAX_SEGS; i++) {
        seg_vmo[i]     = HANDLE_INVALID;
        seg_p_memsz[i] = 0;
    }

    /* 1. Get read-only eager VMO wrapping the ELF bytes in the initrd. */
    r = sl_sys2(SYS_INITRD_VMO, (long)spawn_cap_h, idx);
    if (r < 0) goto out;
    elf_h = (handle_id_t)r;

    /* 2. Map ELF read-only at SL_ELF_VADDR for parsing. */
    r = sl_sys3(SYS_VMO_MAP, (long)elf_h, (long)SL_ELF_VADDR, 0);
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
            if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
            seg_p_vaddr [seg_count] = ph->p_vaddr;
            seg_p_memsz [seg_count] = ph->p_memsz;
            seg_p_filesz[seg_count] = ph->p_filesz;
            seg_p_offset[seg_count] = ph->p_offset;
            seg_p_flags [seg_count] = ph->p_flags;
            uint64_t vend = ph->p_vaddr + ph->p_memsz;
            if (vend > max_vend) max_vend = vend;
            seg_count++;
        }
        if (seg_count == 0) goto out;

        /* 5. Choose page-aligned ASLR bias. */
        uint64_t bias = sl_choose_bias(max_vend);

        /* 6. Create a demand VMO for each segment. */
        for (uint32_t i = 0; i < seg_count; i++) {
            r = sl_sys1(SYS_VMO_CREATE, (long)seg_p_memsz[i]);
            if (r < 0) goto out;
            seg_vmo[i] = (handle_id_t)r;
        }

        /* 7. Map each segment VMO writable in loader's temp window. */
        for (uint32_t i = 0; i < seg_count; i++) {
            uint64_t slot = SL_SEG_VADDR_BASE + (uint64_t)i * SL_SEG_SLOT_SIZE;
            r = sl_sys3(SYS_VMO_MAP, (long)seg_vmo[i], (long)slot, 1 /*WRITABLE*/);
            if (r < 0) goto out;
            segs_in_loader |= (1u << i);
        }

        /* 8. Copy ELF file data into each segment slot (triggers demand faults
         *    in the loader → populates seg_vmo[i]->pages[]). */
        for (uint32_t i = 0; i < seg_count; i++) {
            uint64_t slot = SL_SEG_VADDR_BASE + (uint64_t)i * SL_SEG_SLOT_SIZE;
            uint8_t *dst  = (uint8_t *)(uintptr_t)slot;
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
                                     (slot + rel->r_offset - seg_p_vaddr[si]);
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
        sl_close(elf_h);
        elf_h = HANDLE_INVALID;

        /* 12. Create empty target process. */
        r = sl_sys0(SYS_PROCESS_CREATE);
        if (r < 0) goto out;
        proc_h = (handle_id_t)r;

        /* 13. Create IPC bootstrap channel (parent end = ch_h). */
        r = sl_sys0(SYS_CHAN_CREATE);
        if (r < 0) goto out;
        ch_h = (handle_id_t)r;

        /* 14. Create user stack demand VMO and map into child. */
        r = sl_sys1(SYS_VMO_CREATE, (long)USER_STACK_SIZE);
        if (r < 0) goto out;
        stack_vmo_h = (handle_id_t)r;

        r = sl_sys4(SYS_VMO_MAP_INTO,
                    (long)stack_vmo_h, (long)proc_h,
                    (long)USER_STACK_BASE, 1 /*WRITABLE*/);
        if (r < 0) goto out;

        /* 15. Map each segment demand VMO into child with correct W^X flags. */
        for (uint32_t i = 0; i < seg_count; i++) {
            long flags = 0;
            if (seg_p_flags[i] & PF_W) flags |= 1; /* WRITABLE */
            if (seg_p_flags[i] & PF_X) flags |= 2; /* EXEC     */
            /* W^X: if both PF_W and PF_X, clear EXEC — code shouldn't be writable */
            if ((seg_p_flags[i] & (PF_W | PF_X)) == (PF_W | PF_X)) flags = 1;
            r = sl_sys4(SYS_VMO_MAP_INTO,
                        (long)seg_vmo[i], (long)proc_h,
                        (long)(bias + seg_p_vaddr[i]), flags);
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
        sl_close(stack_vmo_h); stack_vmo_h = HANDLE_INVALID;
        for (uint32_t i = 0; i < seg_count; i++) {
            sl_close(seg_vmo[i]); seg_vmo[i] = HANDLE_INVALID;
        }

        /* 18. Insert the channel handle into the child's handle table. */
        r = sl_sys4(SYS_HANDLE_INSERT,
                    (long)proc_h, (long)ch_h,
                    (long)(RIGHT_READ | RIGHT_WRITE |
                           RIGHT_DUPLICATE | RIGHT_TRANSFER),
                    0);
        if (r < 0) goto out;
        handle_id_t child_ch_id = (handle_id_t)r;

        /* 19. Start first thread: entry at bias+e_entry, RSP = stack top - 8,
         *     RBX = child_ch_id (bootstrap convention from entry.S). */
        r = sl_sys4(SYS_THREAD_START,
                    (long)proc_h,
                    (long)(bias + elf_entry),
                    (long)(USER_STACK_TOP - 8ULL),
                    (long)child_ch_id);
        if (r < 0) goto out;
    }

    *out_proc_h = proc_h;
    *out_chan_h  = ch_h;
    return 0;

out:
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
    sl_close(elf_h);
    sl_close(stack_vmo_h);
    sl_close(proc_h);
    sl_close(ch_h);
    for (uint32_t i = 0; i < SL_MAX_SEGS; i++) sl_close(seg_vmo[i]);
    return r;
}
