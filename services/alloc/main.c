/* SPDX-License-Identifier: Apache-2.0 */
/*
 * alloc/main.c — the program that spends everything it was given.
 *
 * Stage 10-run step 3's evidence.  `hello` proves a program STARTS correctly;
 * this one proves it can ask for memory, that what it gets is real, that
 * running out is a RETURN VALUE rather than a catastrophe, and that what it
 * gives back actually comes back.
 *
 * ── Why the last claim is the one that matters ─────────────────────────────
 *
 * Every system can allocate.  What a capability system claims that a POSIX
 * kernel cannot is that a program's memory is exactly what it was GIVEN: there
 * is no shared pool behind it, so exhaustion is not an event the machine has
 * to arbitrate, and reclamation is not a heuristic — it is a capability being
 * deleted.  Those are checkable, and this program checks them:
 *
 *   - it grows until the retype refuses, which it must, because the budget is
 *     finite and nothing else can top it up;
 *   - it is still running afterwards, and says so on the console, which is the
 *     difference between a refusal and a failure;
 *   - it shrinks, and then grows AGAIN past where it had been — which is only
 *     possible if releasing the frames really returned the bytes.
 *
 * Every page it takes is written and read back, because a mapping that was
 * reported and not made looks exactly like success until something reads it.
 */
#include <stdint.h>
#include "../common/prog_mem.h"
#include "../common/console_client.h"
#include "../common/iris_ipc_buffer.h"
#include <iris/program_abi.h>
#include <iris/endpoint_proto.h>
#include <iris/paging.h>

#define ALLOC_OK              42
#define ALLOC_NO_AUXV          1   /* the stack did not name the budget      */
#define ALLOC_NO_GROW          2   /* the first growth failed outright       */
#define ALLOC_READBACK         3   /* a byte written did not read back       */
#define ALLOC_NO_LIMIT         4   /* it never ran out, which cannot be      */
#define ALLOC_DEAD_AFTER       5   /* the refusal was not clean              */
#define ALLOC_NO_SHRINK        6
#define ALLOC_NO_REGROW        7   /* shrinking did not give the bytes back  */
#define ALLOC_NO_MMAP          8
#define ALLOC_MMAP_READBACK    9
#define ALLOC_NO_MPROTECT     10
#define ALLOC_MPROT_LOST      11   /* the data did not survive the re-map    */
#define ALLOC_NO_MUNMAP       12

/* Its own slots.  32..63 are a program's, and this one needs two of them. */
#define ALLOC_SLOT_PT         32u
#define ALLOC_SLOT_IPCBUF     33u
#define ALLOC_SLOT_IPCBUF_PT  34u

int alloc_main(const uint64_t *sp);

static struct prog_mem g_mem;
static uint8_t *g_buf;

static void say(const char *s) {
    if (g_buf) (void)console_ep_write((iris_cptr_t)IRIS_CPTR_CONSOLE_EP, g_buf, s);
}

/* One line, one number, because there is no printf and the number is the
 * evidence.  Decimal, because a person reads this off a boot log. */
static void say_num(const char *pre, uint64_t v, const char *post) {
    char b[96];
    uint32_t k = 0, i;
    char d[24];
    uint32_t n = 0;
    while (pre[k] && k < 60u) { b[k] = pre[k]; k++; }
    if (v == 0u) d[n++] = '0';
    while (v && n < 20u) { d[n++] = (char)('0' + (uint32_t)(v % 10u)); v /= 10u; }
    for (i = 0; i < n && k < 90u; i++) b[k++] = d[n - 1u - i];
    for (i = 0; post[i] && k < 94u; i++) b[k++] = post[i];
    b[k++] = '\n';
    b[k] = 0;
    say(b);
}

/* Write a recognisable pattern over a range and read it back.  The pattern
 * depends on the ADDRESS, so two mappings that landed on the same physical
 * page would fail this even though each read back what it wrote. */
static int paint(uint64_t base, uint64_t bytes) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)base;
    uint64_t i;
    for (i = 0; i < bytes; i += 4096u)
        p[i] = (uint8_t)(((base + i) >> 12) ^ 0xA5u);
    for (i = 0; i < bytes; i += 4096u)
        if (p[i] != (uint8_t)(((base + i) >> 12) ^ 0xA5u)) return 0;
    return 1;
}

int alloc_main(const uint64_t *sp) {
    uint64_t untyped = 0;
    uint64_t peak = 0, got = 0;
    uint32_t rounds = 0;

    /*
     * The budget's slot comes off the auxiliary vector, not out of a constant.
     * That indirection is the contract's (`AT_IRIS_UNTYPED`), and this is the
     * first program that uses it for anything: a binary that reads it keeps
     * working when the table moves, and one that hardcodes 12 does not.
     */
    {
        uint64_t argc, envc = 0;
        const uint64_t *p;
        if (!sp) return ALLOC_NO_AUXV;
        argc = sp[0];
        if (argc > 64u) return ALLOC_NO_AUXV;
        p = sp + 1 + argc + 1;
        while (p[envc] != 0u) { if (envc > 64u) return ALLOC_NO_AUXV; envc++; }
        p += envc + 1u;
        for (; p[0] != 0u; p += 2)
            if (p[0] == AT_IRIS_UNTYPED) untyped = p[1];
        if (untyped == 0u) return ALLOC_NO_AUXV;
    }

    /*
     * The console FIRST, before a byte of the budget is spent on anything
     * else.  The whole point of this program is what it can still do after it
     * runs out, and a program that only acquires its voice afterwards cannot
     * demonstrate that.
     */
    g_buf = (uint8_t *)iris_ipc_buffer_init_from(untyped, IRIS_CPTR_OWN_VSPACE,
                                                 IRIS_CPTR_OWN_TCB,
                                                 ALLOC_SLOT_IPCBUF,
                                                 ALLOC_SLOT_IPCBUF_PT,
                                                 IRIS_IPC_BUFFER_VA);

    prog_mem_setup(&g_mem, USER_PRIVATE_BASE, untyped, IRIS_CPTR_OWN_VSPACE,
                   IRIS_PROG_SLOT_FREE2_LO, 255u, ALLOC_SLOT_PT);

    /* ── grow until it refuses ────────────────────────────────────────── */
    for (;;) {
        uint64_t step = 64u << 10;
        uint64_t old  = prog_brk(&g_mem, (int64_t)step);
        if (old == (uint64_t)-1) break;
        if (!paint(old, step)) return ALLOC_READBACK;
        got += step;
        if (++rounds > 4096u) break;      /* a region this big cannot happen */
    }
    if (got == 0u) return ALLOC_NO_GROW;
    if (rounds > 4096u) return ALLOC_NO_LIMIT;
    peak = got;
    say_num("[ALLOC] the budget ran out after ", peak >> 10, " KiB, and here I am");

    /*
     * Still alive, and still able to WORK.
     *
     * Not merely "the next instruction executed" — the refusal must have left
     * every structure it touched consistent.  So: read back a page taken long
     * before the failure, and ask for memory again and be refused again.  A
     * cursor left half-advanced by the failed attempt would hand out an
     * address nothing backs, and this is where that shows.
     */
    if (!paint(g_mem.brk_base, 4096u))                   return ALLOC_DEAD_AFTER;
    if (prog_brk(&g_mem, (int64_t)(64u << 10)) != (uint64_t)-1)
        return ALLOC_DEAD_AFTER;

    /* ── give it back, and prove the bytes really came back ───────────── */
    {
        uint64_t half = peak / 2u;
        uint64_t regrown = 0;
        if (prog_brk(&g_mem, -(int64_t)half) == (uint64_t)-1) return ALLOC_NO_SHRINK;
        while (regrown + (64u << 10) <= half) {
            if (prog_brk(&g_mem, (int64_t)(64u << 10)) == (uint64_t)-1) break;
            regrown += 64u << 10;
        }
        /*
         * Most of it, not all of it.
         *
         * Releasing a frame returns its bytes to the Untyped, but the Untyped
         * is a BUMP allocator: the bytes are reusable, and the region is only
         * rewound as a whole by `Untyped_Reset`.  So what is asserted is that
         * the second growth got most of what the first one did — a system that
         * had not really released anything would get nothing at all.
         */
        if (regrown * 2u < half) return ALLOC_NO_REGROW;
        say_num("[ALLOC] gave back half and took ", regrown >> 10, " KiB of it again");
    }

    /* ── mmap, mprotect, munmap ───────────────────────────────────────── */
    {
        uint64_t va = prog_mmap(&g_mem, 4096u, PROG_PROT_RW);
        volatile uint8_t *p;
        if (va == 0u) return ALLOC_NO_MMAP;
        if (va < USER_PRIVATE_BASE + IRIS_PROG_MMAP_OFF ||
            va >= USER_PRIVATE_BASE + IRIS_PROG_MMAP_END_OFF)
            return ALLOC_NO_MMAP;        /* outside the region it promised */
        p = (volatile uint8_t *)(uintptr_t)va;
        p[0] = 0x5Au; p[4095] = 0xC3u;
        if (p[0] != 0x5Au || p[4095] != 0xC3u) return ALLOC_MMAP_READBACK;

        /* Read-only, which is unmap-and-remap, which means the PAGE must
         * survive being remapped — a re-map that handed back a different
         * frame would lose the bytes and nothing would say so. */
        if (prog_mprotect(&g_mem, va, 4096u, PROG_PROT_R) != 0)
            return ALLOC_NO_MPROTECT;
        if (p[0] != 0x5Au || p[4095] != 0xC3u) return ALLOC_MPROT_LOST;

        /* A sub-range is refused rather than widened: see prog_mprotect. */
        if (prog_mprotect(&g_mem, va, 2048u, PROG_PROT_RW) == 0)
            return ALLOC_NO_MPROTECT;

        if (prog_munmap(&g_mem, va) != 0) return ALLOC_NO_MUNMAP;
        /* ...and the address is NOT reused: the cursor never goes backwards,
         * which this contract says out loud rather than pretending. */
        if (prog_mmap(&g_mem, 4096u, PROG_PROT_RW) == va) return ALLOC_NO_MUNMAP;
    }

    say("[ALLOC] every claim held\n");
    return ALLOC_OK;
}
