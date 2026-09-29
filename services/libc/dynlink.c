/* SPDX-License-Identifier: Apache-2.0 */
/*
 * dynlink.c — symbol resolution, against the table the spawner gave us.
 *
 * ── What a stock dynamic linker does, and what this does instead ───────────
 *
 * A stock linker reads `DT_NEEDED`, then searches `DT_RPATH`, then
 * `LD_LIBRARY_PATH`, then `/lib`, opening whatever it finds by name.  That is
 * "open any path it can name" arriving through the LOADER rather than through
 * the program, and charter §6 refused a personality that did it.
 *
 * Here the spawner resolved the object set BEFORE this process existed and
 * minted one pair of capabilities per object into slots 64 upward.  There is
 * no search, no path, and no environment variable that changes the answer: the
 * set of things this program can link against is fixed, visibly, at launch, by
 * whoever launched it.  `DT_NEEDED` is READ — to say which of those objects a
 * relocation should resolve in — and never used to go looking.
 *
 * Today the set is one object: this libc, which is also the interpreter.  That
 * is not a simplification of the design, it is how many objects there are; the
 * loop below walks a list because the list is the design.
 *
 * ── What it resolves ───────────────────────────────────────────────────────
 *
 * `R_X86_64_RELATIVE` (no symbol), `GLOB_DAT` and `JUMP_SLOT` (a symbol's
 * address), and `R_X86_64_64` (symbol plus addend).  `R_X86_64_COPY` is
 * REFUSED rather than ignored: it asks the linker to copy a definition's bytes
 * into the executable, which only makes sense for a non-PIE and would silently
 * give the program a stale copy of something it believed it shared.
 */
#include "libc_internal.h"
#include "../common/elf64.h"
#include "../common/elf_reloc.h"

/* One object this program may resolve against. */
struct dl_obj {
    uint64_t         base;
    const Elf64_Sym *symtab;
    const char      *strtab;
    uint32_t         nsym;
};

#define DL_MAX_OBJ 8u
#define DL_MAX_SEG 8u

/*
 * Read an object's dynamic section into the three things a lookup needs.
 *
 * `nsym` comes from `DT_HASH`'s `nchain`, which is the ELF file format's own
 * statement of how many symbols the table has — there is no `DT_SYMSZ`, and
 * walking until something looks wrong is how a linker reads past the end of a
 * malformed image.
 */
static int dl_scan_dynamic(uint64_t base, const Elf64_Dyn *dyn, uint64_t maxent,
                           struct dl_obj *o) {
    uint64_t symtab = 0, strtab = 0, hash = 0, syment = sizeof(Elf64_Sym);

    o->base = base; o->symtab = 0; o->strtab = 0; o->nsym = 0;
    for (uint64_t i = 0; i < maxent && dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
            case DT_SYMTAB: symtab = dyn[i].d_val; break;
            case DT_STRTAB: strtab = dyn[i].d_val; break;
            case DT_HASH:   hash   = dyn[i].d_val; break;
            case DT_SYMENT: syment = dyn[i].d_val; break;
            default: break;
        }
    }
    if (!symtab || !strtab || !hash) return 0;
    if (syment != sizeof(Elf64_Sym)) return 0;

    o->symtab = (const Elf64_Sym *)(uintptr_t)(base + symtab);
    o->strtab = (const char *)(uintptr_t)(base + strtab);
    {
        const uint32_t *h = (const uint32_t *)(uintptr_t)(base + hash);
        o->nsym = h[1];                     /* nchain == the symbol count */
    }
    if (o->nsym == 0u || o->nsym > 65536u) return 0;
    return 1;
}

static int dl_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/*
 * Find a DEFINED symbol by name, across the objects this program was given.
 *
 * Linear, because the tables here hold tens of symbols and a hash walk is more
 * code to get wrong than it saves.  `SHN_UNDEF` entries are skipped: an object
 * that merely REFERENCES a name has not defined it, and treating a reference
 * as a definition is how a program resolves a symbol to address zero.
 */
static int dl_lookup(const struct dl_obj *objs, uint32_t nobj, const char *name,
                     uint64_t *out) {
    for (uint32_t i = 0; i < nobj; i++) {
        const struct dl_obj *o = &objs[i];
        for (uint32_t s = 1; s < o->nsym; s++) {
            if (o->symtab[s].st_shndx == SHN_UNDEF) continue;
            if (!dl_streq(o->strtab + o->symtab[s].st_name, name)) continue;
            *out = o->base + o->symtab[s].st_value;
            return 1;
        }
    }
    return 0;
}

/* Apply one relocation table.  Returns 0 or negative. */
static long dl_apply(const struct elf_seg_view *segs, uint32_t nseg,
                     uint64_t base, const struct dl_obj *objs, uint32_t nobj,
                     const struct dl_obj *self,
                     const uint8_t *rela, uint64_t bytes, uint64_t ent) {
    uint64_t n;

    if (ent < sizeof(Elf64_Rela)) return -1;
    n = bytes / ent;
    for (uint64_t i = 0; i < n; i++) {
        const Elf64_Rela *r = (const Elf64_Rela *)(const void *)(rela + i * ent);
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uint32_t si   = ELF64_R_SYM(r->r_info);
        uint64_t value = 0;
        uint8_t *site = 0;
        uint32_t s;

        /* RELATIVE is the shared pass, and it is the one with a test of its
         * own — see elf_reloc.h for why applying it twice is silent. */
        if (type == R_X86_64_RELATIVE) {
            if (elf_apply_relative(segs, nseg, base, (const uint8_t *)r,
                                   sizeof(Elf64_Rela), sizeof(Elf64_Rela)) < 0)
                return -1;
            continue;
        }
        if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT &&
            type != R_X86_64_64) {
            /* COPY is refused rather than skipped: see the file header. */
            if (type == R_X86_64_COPY) return -1;
            continue;
        }

        if (si == 0u || !self || si >= self->nsym) return -1;
        {
            const char *name = self->strtab + self->symtab[si].st_name;
            if (!dl_lookup(objs, nobj, name, &value)) {
                /*
                 * An unresolved symbol is a REFUSAL, not a zero.
                 *
                 * Filling it with zero produces a program that runs until it
                 * calls the thing it could not find, and then faults at an
                 * address that names nothing.  The set of objects was chosen
                 * by the spawner; a name missing from it is the spawner's
                 * mistake and belongs at launch.
                 */
                return -1;
            }
        }
        if (type == R_X86_64_64) value += (uint64_t)r->r_addend;

        for (s = 0; s < nseg; s++) {
            if (r->r_offset < segs[s].vaddr) continue;
            if (r->r_offset - segs[s].vaddr + sizeof(uint64_t) > segs[s].bytes)
                continue;
            break;
        }
        if (s == nseg) return -1;
        site = segs[s].host + (r->r_offset - segs[s].vaddr);
        for (uint32_t b = 0; b < 8u; b++) site[b] = (uint8_t)(value >> (8u * b));
    }
    return 0;
}

long __libc_link_program(uint64_t prog_base, const void *phdrs,
                         uint64_t phnum, uint64_t phent) {
    const uint8_t *ph = (const uint8_t *)phdrs;
    const Elf64_Phdr *dyn_ph = 0;
    struct elf_seg_view segs[DL_MAX_SEG];
    struct dl_obj objs[DL_MAX_OBJ];
    struct dl_obj prog;
    uint32_t nseg = 0, nobj = 0;

    /*
     * The objects this program may resolve against.
     *
     * Object zero is this libc, which is also the interpreter — its own
     * dynamic section is reachable from `interp_base`.  When the table the
     * spawner filled holds more, they are appended here, and a name that is in
     * none of them is a spawn that should have named one more.
     */
    {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(uintptr_t)__libc.interp_base;
        const uint8_t *iph = (const uint8_t *)(uintptr_t)
                                 (__libc.interp_base + eh->e_phoff);
        for (uint32_t i = 0; i < eh->e_phnum; i++) {
            const Elf64_Phdr *p = (const Elf64_Phdr *)(const void *)
                                      (iph + (uint64_t)i * eh->e_phentsize);
            if (p->p_type != PT_DYNAMIC) continue;
            if (!dl_scan_dynamic(__libc.interp_base,
                                 (const Elf64_Dyn *)(uintptr_t)
                                     (__libc.interp_base + p->p_vaddr),
                                 p->p_memsz / sizeof(Elf64_Dyn), &objs[0]))
                return -1;
            nobj = 1u;
            break;
        }
    }
    if (nobj == 0u) return -1;

    /* The program's own segments, as addresses in the space we are running in
     * — which is the program's, because an interpreter is loaded into the
     * process it serves. */
    for (uint64_t i = 0; i < phnum; i++) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(const void *)(ph + i * phent);
        if (p->p_type == PT_DYNAMIC) { dyn_ph = p; continue; }
        if (p->p_type != PT_LOAD || p->p_memsz == 0u) continue;
        if (nseg >= DL_MAX_SEG) return -1;
        segs[nseg].vaddr = p->p_vaddr;
        segs[nseg].bytes = p->p_memsz;
        segs[nseg].host  = (uint8_t *)(uintptr_t)(prog_base + p->p_vaddr);
        nseg++;
    }
    if (nseg == 0u) return -1;
    if (!dyn_ph) return 0;              /* nothing dynamic: nothing to do */

    {
        const Elf64_Dyn *dyn = (const Elf64_Dyn *)(uintptr_t)
                                   (prog_base + dyn_ph->p_vaddr);
        uint64_t maxent = dyn_ph->p_memsz / sizeof(Elf64_Dyn);
        uint64_t rela = 0, relasz = 0, relaent = sizeof(Elf64_Rela);
        uint64_t jmprel = 0, pltrelsz = 0;

        if (!dl_scan_dynamic(prog_base, dyn, maxent, &prog)) {
            /* A program with no symbol table of its own can still have
             * RELATIVE relocations, and those need no names. */
            prog.symtab = 0; prog.strtab = 0; prog.nsym = 0;
        }
        for (uint64_t i = 0; i < maxent && dyn[i].d_tag != DT_NULL; i++) {
            switch (dyn[i].d_tag) {
                case DT_RELA:     rela     = dyn[i].d_val; break;
                case DT_RELASZ:   relasz   = dyn[i].d_val; break;
                case DT_RELAENT:  relaent  = dyn[i].d_val; break;
                case DT_JMPREL:   jmprel   = dyn[i].d_val; break;
                case DT_PLTRELSZ: pltrelsz = dyn[i].d_val; break;
                default: break;
            }
        }
        if (relasz && dl_apply(segs, nseg, prog_base, objs, nobj, &prog,
                               (const uint8_t *)(uintptr_t)(prog_base + rela),
                               relasz, relaent) < 0)
            return -1;
        /*
         * The PLT's relocations, applied EAGERLY.
         *
         * Lazy binding needs a resolver the PLT can trampoline into, a
         * writable GOT entry pointing at it, and an interpreter that stays
         * resident to answer — three mechanisms to save work a program with
         * tens of symbols does not have.  Binding everything now also means a
         * missing symbol is found at launch instead of at the first call to
         * it, which is the difference between a spawn that fails and a program
         * that dies somewhere unrelated.
         */
        if (pltrelsz && dl_apply(segs, nseg, prog_base, objs, nobj, &prog,
                                 (const uint8_t *)(uintptr_t)(prog_base + jmprel),
                                 pltrelsz, sizeof(Elf64_Rela)) < 0)
            return -1;
    }
    return 0;
}
