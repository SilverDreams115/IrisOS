/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ldso/main.c — the interpreter, after it has relocated itself.
 *
 * `dlstart.S` did the part that cannot be written in C.  Everything here runs
 * with this image's own relocations applied, so globals and literals work
 * normally, and the job left is the one an interpreter exists for: relocate
 * the PROGRAM, then enter it.
 *
 * ── Why the loader did not do this ─────────────────────────────────────────
 *
 * It could have — it relocates every service in the system.  It must not,
 * because `R_X86_64_RELATIVE` is not idempotent and carries no record of
 * having been applied: an image relocated by both the loader and an
 * interpreter ends up relocated by whichever went last, at whichever base that
 * one believed, with no error anywhere.  `test_elf_reloc` applies the pass
 * twice on purpose and asserts the damage, and `svc_loader` skips the pass for
 * any image with a `PT_INTERP`.  This file is the other half of that rule.
 *
 * ── What it is not, yet ────────────────────────────────────────────────────
 *
 * There is no symbol resolution, no `DT_NEEDED`, no `GLOB_DAT` and no
 * `JUMP_SLOT`.  A program with those needs step 6, where the interpreter is
 * musl and the object table it resolves against is the one the SPAWNER filled
 * — never a search path.  What is here is the two-object load itself, which is
 * the thing that had to exist before any of that could.
 */
#include <stdint.h>
#include "../common/elf_reloc.h"
#include <iris/program_abi.h>

/* How many loadable segments of a program this will relocate through.  A PIE
 * built by this tree has two; the bound is here so a malformed image cannot
 * make this walk an array it did not size. */
#define DL_MAX_SEGS 8u

/*
 * A relocation of this interpreter's OWN, and a read of it.
 *
 * `dlstart.S` relocates this image before calling here, and that code is the
 * hardest thing in the system to test: it runs once, before anything can
 * report, and an image with no relative relocations would exercise none of it
 * while looking perfectly healthy.  This pointer costs one
 * `R_X86_64_RELATIVE`, so the self-relocation is LOAD-BEARING — if it ever
 * stops working, the read below faults here instead of somewhere in musl
 * months from now.
 */
static const char dl_tag_text[] = "ldso";
static const char *const dl_tag = dl_tag_text;

uint64_t dl_main(const uint64_t *sp);

/*
 * Returns the address to enter the program at, or 0 — and 0 means the
 * interpreter refuses, which `dlstart.S` turns into an exit status rather than
 * a jump to somewhere arbitrary.  An interpreter that cannot relocate its
 * program must not run it: every relative relocation left unapplied is a
 * pointer that will be followed later, and the fault will have no connection
 * to this.
 */
uint64_t dl_main(const uint64_t *sp) {
    uint64_t at_phdr = 0, at_phnum = 0, at_phent = 0, at_entry = 0;
    /* Our own relocations were applied before this ran, or nothing here is
     * what it says it is. */
    if (!dl_tag || dl_tag[0] != 'l' || dl_tag[3] != 'o') return 0;
    uint64_t prog_base = 0;
    const Elf64_Phdr *phs;
    const Elf64_Phdr *dyn_ph = 0;
    struct elf_seg_view segs[DL_MAX_SEGS];
    uint32_t nseg = 0;
    int have_phdr = 0;

    /* ── what the stack says ── */
    {
        uint64_t argc, envc = 0;
        const uint64_t *p;
        if (!sp) return 0;
        argc = sp[0];
        if (argc > 64u) return 0;
        p = sp + 1 + argc + 1;
        while (p[envc] != 0u) { if (envc > 64u) return 0; envc++; }
        p += envc + 1u;
        for (; p[0] != 0u; p += 2) {
            switch (p[0]) {
                case AT_PHDR:  at_phdr  = p[1]; break;
                case AT_PHNUM: at_phnum = p[1]; break;
                case AT_PHENT: at_phent = p[1]; break;
                case AT_ENTRY: at_entry = p[1]; break;
                default: break;
            }
        }
    }
    if (!at_phdr || !at_entry) return 0;
    if (at_phnum == 0u || at_phnum > 64u) return 0;
    if (at_phent < sizeof(Elf64_Phdr)) return 0;

    phs = (const Elf64_Phdr *)(uintptr_t)at_phdr;

    /*
     * Where the program was loaded.
     *
     * `AT_PHDR` is a MAPPED address of the program header table and `PT_PHDR`
     * is that table's own unbiased vaddr, so their difference is the base.
     * That is the only derivation that does not assume something about the
     * layout — and it is why `services/link_program.ld` declares `PT_PHDR`
     * explicitly rather than letting the linker decide whether to bother.
     */
    for (uint64_t i = 0; i < at_phnum; i++) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(const void *)
                                  ((const uint8_t *)phs + i * at_phent);
        if (p->p_type != PT_PHDR) continue;
        if (at_phdr < p->p_vaddr) return 0;
        prog_base = at_phdr - p->p_vaddr;
        have_phdr = 1;
        break;
    }
    if (!have_phdr) return 0;

    /* The segments, as addresses in the address space this is already running
     * in — which is the program's, because an interpreter is loaded INTO the
     * process it serves. */
    for (uint64_t i = 0; i < at_phnum; i++) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(const void *)
                                  ((const uint8_t *)phs + i * at_phent);
        if (p->p_type == PT_DYNAMIC) { dyn_ph = p; continue; }
        if (p->p_type != PT_LOAD || p->p_memsz == 0u) continue;
        if (nseg >= DL_MAX_SEGS) return 0;
        segs[nseg].vaddr = p->p_vaddr;
        segs[nseg].bytes = p->p_memsz;
        segs[nseg].host  = (uint8_t *)(uintptr_t)(prog_base + p->p_vaddr);
        nseg++;
    }
    if (nseg == 0u) return 0;

    /*
     * No dynamic section is not an error: a program can have an interpreter
     * and no relocations at all, and there is nothing to do for it.  A
     * dynamic section whose relocations cannot be applied IS an error.
     */
    if (dyn_ph) {
        const Elf64_Dyn *dyn = (const Elf64_Dyn *)(uintptr_t)
                                   (prog_base + dyn_ph->p_vaddr);
        uint64_t rela_vaddr = 0, rela_bytes = 0, rela_ent = 0;
        if (elf_find_rela(dyn, dyn_ph->p_memsz / sizeof(Elf64_Dyn),
                          &rela_vaddr, &rela_bytes, &rela_ent)) {
            if (elf_apply_relative(segs, nseg, prog_base,
                                   (const uint8_t *)(uintptr_t)
                                       (prog_base + rela_vaddr),
                                   rela_bytes, rela_ent) < 0)
                return 0;
        }
    }

    return at_entry;
}
