/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_COMMON_ELF_RELOC_H
#define IRIS_COMMON_ELF_RELOC_H

#include <stdint.h>
#include "elf64.h"

/*
 * `R_X86_64_RELATIVE`, applied exactly once.
 *
 * ── Why this is its own file with its own test ─────────────────────────────
 *
 * Because the pass is NOT IDEMPOTENT and nothing about reading it says so.
 * A relative relocation writes `base + addend` at a site whose previous
 * contents it ignores — so applying it twice writes `base + base + addend`,
 * which is a pointer into nothing, at a site that looked perfectly relocated
 * after the first pass.  There is no error, no fault at the time, and no way
 * to tell by inspection which pass a given image has had.
 *
 * That becomes reachable the moment there are TWO things that could relocate a
 * program: the loader, and an interpreter.  Both are correct on their own and
 * together they corrupt every relative relocation in the image.  The rule is
 * therefore a rule about who does it, and the answer is the same as every
 * other system's: **if the image has a `PT_INTERP`, the loader does not
 * relocate it — the interpreter will.**
 *
 * `elf_image_has_interp` is that decision, in one place, so the loader and the
 * spawner cannot disagree about it.
 *
 * The arithmetic lives here rather than inside the loader's segment loop so
 * that a host test can apply the pass twice ON PURPOSE and assert the damage,
 * which is what makes the rule above a checked fact instead of a comment.
 */

/*
 * One loadable segment, as the caller currently has it: where the image says
 * it lives, how big it is, and where the CALLER can write to it right now.
 *
 * The two addresses are separate because they always are — a loader patches a
 * child's segment through a mapping in its own address space, and an
 * interpreter patches its own through the address it is already running at.
 */
struct elf_seg_view {
    uint64_t vaddr;   /* p_vaddr, unbiased, as the image declares it */
    uint64_t bytes;   /* p_memsz                                     */
    uint8_t *host;    /* where vaddr is writable from HERE           */
};

/* Does this image declare an interpreter?  `phs` is the program header table,
 * already validated to be inside the image. */
int elf_image_has_interp(const Elf64_Phdr *phs, uint32_t phnum,
                         uint32_t phentsize);

/*
 * Apply every `R_X86_64_RELATIVE` in the table, and nothing else.
 *
 * Returns the number applied, or a negative `iris_error_t` when a relocation
 * names a site outside every segment — which is a malformed image, and is
 * reported rather than skipped, because a relocation that could not be applied
 * leaves a pointer that will be followed later.
 *
 * Other relocation types are SKIPPED rather than refused: a static PIE emits
 * only RELATIVE, and an image with `JUMP_SLOT` or `GLOB_DAT` is one whose
 * interpreter is expected to resolve them.
 */
long elf_apply_relative(const struct elf_seg_view *segs, uint32_t nseg,
                        uint64_t bias, const uint8_t *rela, uint64_t rela_bytes,
                        uint64_t rela_ent);

/*
 * Find `DT_RELA` / `DT_RELASZ` / `DT_RELAENT` in a dynamic section.
 *
 * `dyn` is the section as the caller can read it now; the vaddr it answers is
 * the image's own, unbiased, and it is the caller's business to turn that into
 * something it can read.  Returns 1 when there is a table to apply.
 */
int elf_find_rela(const Elf64_Dyn *dyn, uint64_t max_entries,
                  uint64_t *out_vaddr, uint64_t *out_bytes, uint64_t *out_ent);

#endif /* IRIS_COMMON_ELF_RELOC_H */
