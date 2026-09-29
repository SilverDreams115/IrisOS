/* SPDX-License-Identifier: Apache-2.0 */
/*
 * elf_reloc.c — the relative-relocation pass, in one place.
 *
 * See `elf_reloc.h` for why it is not inside the loader.  Everything here is
 * pure: it reads the caller's descriptors and writes through the pointers the
 * caller supplied, and invokes nothing.  That is what lets the host suite
 * apply the pass twice on purpose and assert the damage.
 *
 * It is also what lets the INTERPRETER use it before it has relocated itself:
 * these functions take every address as an argument and touch no global of
 * their own, so calling one is a PC-relative call into code that needs no GOT.
 * A helper here that referenced a global would work in the loader and crash in
 * `ldso`, at the one moment nothing can report anything.
 */
#include "elf_reloc.h"
#include <iris/nc/error.h>

int elf_image_has_interp(const Elf64_Phdr *phs, uint32_t phnum,
                         uint32_t phentsize) {
    if (!phs || phentsize < sizeof(Elf64_Phdr)) return 0;
    for (uint32_t i = 0; i < phnum; i++) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(const void *)
                                  ((const uint8_t *)phs + (uint64_t)i * phentsize);
        if (p->p_type == PT_INTERP) return 1;
    }
    return 0;
}

int elf_find_rela(const Elf64_Dyn *dyn, uint64_t max_entries,
                  uint64_t *out_vaddr, uint64_t *out_bytes, uint64_t *out_ent) {
    uint64_t vaddr = 0, bytes = 0, ent = sizeof(Elf64_Rela);
    uint64_t i;

    if (!dyn || !out_vaddr || !out_bytes || !out_ent) return 0;
    /* Bounded, because `DT_NULL` is the image's own promise that the section
     * ends and a malformed one does not have to keep it. */
    for (i = 0; i < max_entries && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_RELA)         vaddr = dyn[i].d_val;
        else if (dyn[i].d_tag == DT_RELASZ)  bytes = dyn[i].d_val;
        else if (dyn[i].d_tag == DT_RELAENT) ent   = dyn[i].d_val;
    }
    if (bytes == 0u || ent < sizeof(Elf64_Rela)) return 0;
    *out_vaddr = vaddr;
    *out_bytes = bytes;
    *out_ent   = ent;
    return 1;
}

long elf_apply_relative(const struct elf_seg_view *segs, uint32_t nseg,
                        uint64_t bias, const uint8_t *rela, uint64_t rela_bytes,
                        uint64_t rela_ent) {
    uint64_t n, i;
    long applied = 0;

    if (!segs || nseg == 0u || !rela) return (long)IRIS_ERR_INVALID_ARG;
    if (rela_ent < sizeof(Elf64_Rela)) return (long)IRIS_ERR_INVALID_ARG;
    n = rela_bytes / rela_ent;

    for (i = 0; i < n; i++) {
        const Elf64_Rela *r = (const Elf64_Rela *)(const void *)
                                  (rela + i * rela_ent);
        uint32_t s;
        uint64_t val;
        uint8_t *site;

        /* Everything that is not RELATIVE belongs to somebody else: a static
         * PIE emits only these, and an image with JUMP_SLOT or GLOB_DAT is one
         * whose interpreter is expected to resolve them. */
        if (ELF64_R_TYPE(r->r_info) != R_X86_64_RELATIVE) continue;

        for (s = 0; s < nseg; s++) {
            if (r->r_offset < segs[s].vaddr) continue;
            if (r->r_offset - segs[s].vaddr + sizeof(uint64_t) > segs[s].bytes)
                continue;
            break;
        }
        /* Reported, not skipped.  A relocation that could not be applied
         * leaves a pointer that something will follow later, and the fault
         * will be somewhere with no connection to this image. */
        if (s == nseg) return (long)IRIS_ERR_INVALID_ARG;

        site = segs[s].host + (r->r_offset - segs[s].vaddr);
        val  = bias + (uint64_t)r->r_addend;
        /* Byte at a time: the site's alignment is the image's business, and a
         * 64-bit store to an address this code did not choose is a fault the
         * caller cannot do anything about. */
        for (uint32_t b = 0; b < 8u; b++) site[b] = (uint8_t)(val >> (8u * b));
        applied++;
    }
    return applied;
}
