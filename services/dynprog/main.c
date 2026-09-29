/* SPDX-License-Identifier: Apache-2.0 */
/*
 * dynprog/main.c — a program with an INTERPRETER, and the check that it was
 * relocated exactly once.
 *
 * Stage 10-run step 5's evidence.  What has to be shown is that TWO objects
 * were loaded — this one and `ldso`, at two different biases in two disjoint
 * regions — and that the relocations in this image were applied by exactly one
 * of them.
 *
 * ── Why the relocations are the test ───────────────────────────────────────
 *
 * `R_X86_64_RELATIVE` writes `base + addend` and ignores what was there, so an
 * image relocated twice is relocated at whichever base went last.  If the
 * loader had also relocated this image, `g_names[0]` would hold
 * `base + base + addend`: a pointer roughly one load bias past the end of
 * everything, which faults on the first read and kills this process before it
 * can say anything.  If NOBODY relocated it, `g_names[0]` holds its link-time
 * value — a small number — and reading it faults too.
 *
 * Exactly one pass, at the right base, is the only way the strings below read
 * back as themselves.  That is why the check is a pointer dereference and not
 * a comparison of numbers: numbers can be wrong in ways that still compare
 * equal, and a wrong pointer here cannot be anything but wrong.
 */
#include <stdint.h>
#include <iris/program_abi.h>
#include <iris/paging.h>

/*
 * The interpreter's NAME, and it is a name rather than a path.
 *
 * Nothing searches for it.  The spawner reads this string, asks the object
 * registry for it, and mints the result into this program's object table
 * before this program exists — which is what ledger A-49 promised when it
 * retired charter §6's refusal of a POSIX personality: a dynamic linker here
 * consumes a set somebody chose, visibly, at launch.
 */
__attribute__((used, section(".interp")))
static const char dp_interp[] = "ldso";

#define DP_OK            42
#define DP_NO_AUXV        1
#define DP_NO_INTERP      2   /* AT_BASE was 0: only one object was loaded */
#define DP_BAD_STRINGS    3
#define DP_BAD_FNPTR      4
#define DP_BAD_SELF       5   /* AT_ENTRY is not where this actually is    */

/* Each of these costs one R_X86_64_RELATIVE, which is the point. */
static const char dp_a[] = "IRIS-A";
static const char dp_b[] = "IRIS-B";
static const char *const dp_names[2] = { dp_a, dp_b };

static int dp_one(void) { return 1; }
static int (*const dp_fn)(void) = dp_one;

int dynprog_main(const uint64_t *sp);

int dynprog_main(const uint64_t *sp) {
    uint64_t at_base = 0, at_entry = 0, at_phdr = 0;

    {
        uint64_t argc, envc = 0;
        const uint64_t *p;
        if (!sp) return DP_NO_AUXV;
        argc = sp[0];
        if (argc < 1u || argc > 64u) return DP_NO_AUXV;
        p = sp + 1 + argc + 1;
        while (p[envc] != 0u) { if (envc > 64u) return DP_NO_AUXV; envc++; }
        p += envc + 1u;
        for (; p[0] != 0u; p += 2) {
            if (p[0] == AT_BASE)       at_base  = p[1];
            else if (p[0] == AT_ENTRY) at_entry = p[1];
            else if (p[0] == AT_PHDR)  at_phdr  = p[1];
        }
        if (!at_phdr) return DP_NO_AUXV;
    }

    /*
     * Two objects, and they are in two REGIONS.
     *
     * `AT_BASE` is where the interpreter went.  A zero means the spawner
     * loaded one object and jumped straight here, which would be a perfectly
     * working program and not this test.  The regions are disjoint by
     * construction (docs/contracts/program.md §3) precisely so two `ET_DYN`
     * biases cannot collide, and checking it here is what would notice if that
     * ever stopped being true.
     */
    if (at_base == 0u) return DP_NO_INTERP;
    if (at_base <  USER_PRIVATE_BASE + IRIS_PROG_INTERP_OFF) return DP_NO_INTERP;
    if (at_base >= USER_PRIVATE_BASE + IRIS_PROG_INTERP_END_OFF) return DP_NO_INTERP;
    if (at_phdr <  USER_PRIVATE_BASE + IRIS_PROG_IMAGE_OFF) return DP_BAD_SELF;
    if (at_phdr >= USER_PRIVATE_BASE + IRIS_PROG_IMAGE_END_OFF) return DP_BAD_SELF;

    /* ── relocated exactly once ── */
    if (dp_names[0][5] != 'A' || dp_names[1][5] != 'B') return DP_BAD_STRINGS;
    if (dp_names[0][0] != 'I' || dp_names[1][0] != 'I') return DP_BAD_STRINGS;
    if (dp_fn == 0 || dp_fn() != 1) return DP_BAD_FNPTR;

    /*
     * ...and the interpreter entered THIS program where it was told to.
     *
     * `AT_ENTRY` is the spawner's statement of where the program begins, and
     * the interpreter jumped to it; if the two disagreed, control would be
     * somewhere else entirely and this line would never run.  Checking it is
     * cheap and it pins the one number the whole two-object handover turns on.
     */
    if (at_entry <  USER_PRIVATE_BASE + IRIS_PROG_IMAGE_OFF) return DP_BAD_SELF;
    if (at_entry >= USER_PRIVATE_BASE + IRIS_PROG_IMAGE_END_OFF) return DP_BAD_SELF;

    return DP_OK;
}
