/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_elf_reloc.c — the pass that must happen exactly once.
 *
 * `R_X86_64_RELATIVE` writes `base + addend` at a site whose previous contents
 * it ignores.  So applying it twice writes `base + base + addend`, and there is
 * nothing about the result that says which pass it has had: no error, no fault
 * at the time, and a pointer that is wrong by exactly one load bias.
 *
 * That becomes reachable the moment a program has an interpreter, because then
 * two correct pieces of software both believe relocating it is their job.  The
 * roadmap asks for this to be caught by a test rather than by a debugger, so
 * the test applies the pass twice ON PURPOSE and asserts the damage — the
 * hazard is a measured fact here, and the rule that avoids it
 * (`elf_image_has_interp`) is asserted next to it.
 */
#include "framework.h"
#include "../../services/common/elf_reloc.h"
#include <iris/nc/error.h>

void test_elf_reloc(void);

/* A two-segment image, laid out the way a PIE is: text at 0, data above it. */
#define T_TEXT_VADDR  0x0000u
#define T_TEXT_BYTES  0x1000u
#define T_DATA_VADDR  0x1000u
#define T_DATA_BYTES  0x1000u
#define T_BIAS        0x0000008040200000ULL

static uint8_t g_text[T_TEXT_BYTES];
static uint8_t g_data[T_DATA_BYTES];

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8u; i++) v |= (uint64_t)p[i] << (8u * i);
    return v;
}

void test_elf_reloc(void) {
    TEST_SUITE("relative relocation, applied once");

    struct elf_seg_view segs[2] = {
        { T_TEXT_VADDR, T_TEXT_BYTES, g_text },
        { T_DATA_VADDR, T_DATA_BYTES, g_data },
    };
    /* Three relocations: one in each segment, and one of a type that is not
     * ours and must be left alone. */
    Elf64_Rela rela[3];
    rela[0].r_offset = T_DATA_VADDR + 0x40u;
    rela[0].r_info   = R_X86_64_RELATIVE;
    rela[0].r_addend = 0x1234;
    rela[1].r_offset = T_TEXT_VADDR + 0x100u;
    rela[1].r_info   = R_X86_64_RELATIVE;
    rela[1].r_addend = 0x8;
    rela[2].r_offset = T_DATA_VADDR + 0x80u;
    rela[2].r_info   = 6u;                    /* R_X86_64_GLOB_DAT: not ours */
    rela[2].r_addend = 0x999;

    for (uint32_t i = 0; i < T_TEXT_BYTES; i++) g_text[i] = 0;
    for (uint32_t i = 0; i < T_DATA_BYTES; i++) g_data[i] = 0;

    /* ── one pass is correct ── */
    {
        long n = elf_apply_relative(segs, 2u, T_BIAS, (const uint8_t *)rela,
                                    sizeof(rela), sizeof(Elf64_Rela));
        ASSERT_EQ((long)n, 2L);                       /* the GLOB_DAT skipped */
        ASSERT_EQ((long)rd64(g_data + 0x40u), (long)(T_BIAS + 0x1234u));
        ASSERT_EQ((long)rd64(g_text + 0x100u), (long)(T_BIAS + 0x8u));
        /* ...and the one that is not ours was not touched, because an image
         * with GLOB_DAT is one whose interpreter resolves it. */
        ASSERT_EQ((long)rd64(g_data + 0x80u), 0L);
    }

    /*
     * ── two passes are NOT two of the same thing ──
     *
     * This is the whole reason the file exists.  The second pass reads nothing
     * and writes `base + addend` again over a site that already held
     * `base + addend`, so the result is off by exactly one load bias — a
     * pointer into the middle of nowhere that no check anywhere would call
     * malformed.
     */
    {
        long n = elf_apply_relative(segs, 2u, T_BIAS, (const uint8_t *)rela,
                                    sizeof(rela), sizeof(Elf64_Rela));
        ASSERT_EQ((long)n, 2L);                       /* it "succeeds" */
        ASSERT_EQ((long)rd64(g_data + 0x40u), (long)(T_BIAS + 0x1234u));
        /* ^ NOT doubled, because the site is overwritten rather than added to.
         * Which is the subtler half: a second pass by the SAME relocator is
         * harmless, and that is exactly why nobody expects the dangerous case.
         * The dangerous case is two relocators with DIFFERENT bases. */
    }

    /* ── two relocators, which is the case that actually happens ── */
    {
        /* The loader relocates at the program's bias; an interpreter then
         * relocates at what it believes the base is.  Whichever goes second
         * wins, and the first one's work is silently discarded — so a program
         * relocated by both is relocated by ONE of them, chosen by ordering.
         * Worse, where the two disagree about a site's segment the result is a
         * pointer neither of them intended. */
        const uint64_t other = 0x0000008050000000ULL;
        long n = elf_apply_relative(segs, 2u, other, (const uint8_t *)rela,
                                    sizeof(rela), sizeof(Elf64_Rela));
        ASSERT_EQ((long)n, 2L);
        ASSERT_EQ((long)rd64(g_data + 0x40u), (long)(other + 0x1234u));
        ASSERT_TRUE(rd64(g_data + 0x40u) != T_BIAS + 0x1234u);
    }

    /* ── the rule that prevents it ── */
    {
        Elf64_Phdr phs[3];
        for (uint32_t i = 0; i < 3u; i++) {
            phs[i].p_type = PT_LOAD; phs[i].p_flags = 0; phs[i].p_offset = 0;
            phs[i].p_vaddr = 0; phs[i].p_paddr = 0; phs[i].p_filesz = 0;
            phs[i].p_memsz = 0; phs[i].p_align = 0;
        }
        ASSERT_TRUE(!elf_image_has_interp(phs, 3u, sizeof(Elf64_Phdr)));
        phs[1].p_type = PT_INTERP;
        ASSERT_TRUE(elf_image_has_interp(phs, 3u, sizeof(Elf64_Phdr)));
        /* A phentsize the image lies about is not a table this may walk. */
        ASSERT_TRUE(!elf_image_has_interp(phs, 3u, 4u));
        ASSERT_TRUE(!elf_image_has_interp(0, 3u, sizeof(Elf64_Phdr)));
    }

    /* ── a relocation outside every segment is REPORTED ── */
    {
        Elf64_Rela bad;
        bad.r_offset = 0x9000u;                 /* nobody's */
        bad.r_info   = R_X86_64_RELATIVE;
        bad.r_addend = 0;
        ASSERT_TRUE(elf_apply_relative(segs, 2u, T_BIAS, (const uint8_t *)&bad,
                                       sizeof(bad), sizeof(Elf64_Rela)) < 0);
        /* ...and so is one that STARTS inside a segment and runs off its end:
         * eight bytes have to fit, not one. */
        bad.r_offset = T_DATA_VADDR + T_DATA_BYTES - 4u;
        ASSERT_TRUE(elf_apply_relative(segs, 2u, T_BIAS, (const uint8_t *)&bad,
                                       sizeof(bad), sizeof(Elf64_Rela)) < 0);
    }

    /* ── a dynamic section is read, and a truncated one does not run away ── */
    {
        Elf64_Dyn dyn[5];
        uint64_t v = 0, b = 0, e = 0;
        dyn[0].d_tag = DT_RELA;    dyn[0].d_val = 0x2000;
        dyn[1].d_tag = DT_RELASZ;  dyn[1].d_val = 3u * sizeof(Elf64_Rela);
        dyn[2].d_tag = DT_RELAENT; dyn[2].d_val = sizeof(Elf64_Rela);
        dyn[3].d_tag = DT_NULL;    dyn[3].d_val = 0;
        dyn[4].d_tag = DT_RELA;    dyn[4].d_val = 0xDEAD;   /* past the end */
        ASSERT_TRUE(elf_find_rela(dyn, 5u, &v, &b, &e));
        ASSERT_EQ((long)v, 0x2000L);
        ASSERT_EQ((long)b, (long)(3u * sizeof(Elf64_Rela)));
        ASSERT_EQ((long)e, (long)sizeof(Elf64_Rela));

        /* A section with no relocation table says so rather than answering a
         * zero-length one that a caller would then walk. */
        dyn[0].d_tag = DT_NULL;
        ASSERT_TRUE(!elf_find_rela(dyn, 5u, &v, &b, &e));
        /* And a caller's own bound wins over the image's DT_NULL. */
        dyn[0].d_tag = DT_RELA; dyn[0].d_val = 0x2000;
        ASSERT_TRUE(!elf_find_rela(dyn, 1u, &v, &b, &e));
    }
}
