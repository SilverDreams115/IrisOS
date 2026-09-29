/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libuser/main.c — a program that uses a shared object, and MEASURES that it is
 * shared.
 *
 * Stage 10-run step 4's evidence.  Two copies of this run at once against one
 * object in `objreg`, and what has to be shown is three things that a system
 * can very easily only appear to do:
 *
 *   1. both map the SAME PHYSICAL MEMORY for the library's text.  Not "both
 *      read the same bytes" — two independent copies of a file read the same
 *      bytes too.  `Frame_GetAddress` answers the physical base of the frame a
 *      capability names, and it needs only `RIGHT_READ`, so each process can
 *      simply ASK and report the number.  Two processes naming one physical
 *      address is a measurement, and it is why this step's close condition
 *      says measured rather than asserted.
 *   2. each has its OWN data.  The registry publishes the data segment as a
 *      read-only master; this program copies it into a frame from its own
 *      budget, writes its own id into the copy, and reports that copy's
 *      physical address too — which must DIFFER between the two processes, or
 *      the isolation is a story.
 *   3. a revoke in the registry reaches both at once.  Checked without
 *      touching the memory, by asking what the capability IS: a frame before,
 *      and nothing at all after.  Touching it would fault, which proves the
 *      same thing far less politely and kills the process that was supposed to
 *      report.
 *
 * It talks to whoever spawned it over the endpoint at `IRIS_CPTR_OWN_EP`, in
 * three rounds, because the parent has to be able to say WHEN — the revoke
 * happens between round two and round three and there is no other way to be
 * sure which side of it a report came from.
 */
#include <stdint.h>
#include "../common/prog_mem.h"
#include "../common/iris_msg.h"
#include "../common/iris_map.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <iris/ipc_msg.h>
#include <iris/program_abi.h>
#include <iris/objreg_ep_proto.h>
#include <iris/endpoint_proto.h>
#include <iris/paging.h>

#define LU_OK               42
#define LU_NO_AUXV           1
#define LU_NO_OBJECT         2   /* the object table was empty              */
#define LU_NO_TEXT_MAP       3
#define LU_BAD_TEXT          4   /* the text did not contain what it should */
#define LU_NO_TEXT_PADDR     5
#define LU_NO_DATA_MAP       6
#define LU_NO_PRIVATE        7   /* no frame of its own to copy the data to */
#define LU_DATA_READBACK     8
#define LU_STILL_THERE       9   /* the revoke did not reach it             */
#define LU_GONE_EARLY       10   /* ...or reached it before it was asked to */
#define LU_NO_PARENT        11

#define LU_SLOT_PT           32u
/* No IPC buffer is registered: every message this program sends is words, and
 * a bulk payload is the only thing that needs one. */

int libuser_main(const uint64_t *sp);

static struct prog_mem g_mem;

/* Say it, and wait to be told to go on.  A Call, because the point of each
 * round is the parent's answer as much as the report. */
static long lu_report(uint64_t round, uint64_t a, uint64_t b, uint64_t c) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = round;
    m.words[0]   = a;
    m.words[1]   = b;
    m.words[2]   = c;
    m.word_count = 3u;
    return iris_msg_call((long)IRIS_CPTR_OWN_EP, &m);
}

int libuser_main(const uint64_t *sp) {
    uint64_t untyped = 0, objc = 0, objv = 0;
    uint64_t id = 0;
    uint64_t text_va, data_va, priv_va;
    uint64_t text_pa, priv_pa;
    uint64_t text_bytes, data_bytes;
    uint32_t text_slot, data_slot;

    /* ── what the stack says ── */
    {
        uint64_t argc, envc = 0;
        const uint64_t *p;
        if (!sp) return LU_NO_AUXV;
        argc = sp[0];
        if (argc < 1u || argc > 64u) return LU_NO_AUXV;
        /* argv[1], when there is one, is this process's id: a single digit,
         * because the only thing it has to do is differ between the two. */
        if (argc >= 2u) {
            const char *a1 = (const char *)(uintptr_t)sp[2];
            if (a1 && a1[0] >= '0' && a1[0] <= '9') id = (uint64_t)(a1[0] - '0');
        }
        p = sp + 1 + argc + 1;
        while (p[envc] != 0u) { if (envc > 64u) return LU_NO_AUXV; envc++; }
        p += envc + 1u;
        for (; p[0] != 0u; p += 2) {
            if (p[0] == AT_IRIS_UNTYPED) untyped = p[1];
            else if (p[0] == AT_IRIS_OBJC) objc = p[1];
            else if (p[0] == AT_IRIS_OBJV) objv = p[1];
        }
        if (untyped == 0u || objv == 0u) return LU_NO_AUXV;
        if (objc == 0u) return LU_NO_OBJECT;
    }

    text_slot = (uint32_t)objv;             /* OBJV + 2*0     */
    data_slot = (uint32_t)objv + 1u;        /* OBJV + 2*0 + 1 */

    prog_mem_setup(&g_mem, USER_PRIVATE_BASE, untyped, IRIS_CPTR_OWN_VSPACE,
                   IRIS_PROG_SLOT_FREE2_LO, 255u, LU_SLOT_PT);

    /* ── the library's text: mapped read+execute, and never writable ── */
    {
        long sz = iris_invoke0((long)text_slot, INV_FRAME_SIZE);
        long pa;
        if (sz <= 0) return LU_NO_TEXT_MAP;
        text_bytes = (uint64_t)sz;
        if (!prog_mem_reserve(&g_mem, text_bytes, /*heap=*/0, &text_va))
            return LU_NO_TEXT_MAP;
        /*
         * `PROG_PROT_RX`, which the kernel will accept and `PROG_PROT_RW|RX`
         * would not: W^X is enforced on the map.  It would refuse anyway for a
         * second reason — this capability carries no `RIGHT_WRITE`, so a
         * writable map of it is impossible however the flags are spelled.
         */
        /*
         * `iris_map_frame`, not a bare `Frame_Map`.
         *
         * The kernel creates no page tables.  This address is in a window this
         * program has never touched, so the walk is incomplete and the map
         * answers MISSING_TABLE — the holder retypes a level and installs it,
         * out of its own budget, which is what the helper does.  A bare map
         * here fails on a fresh address space every time.
         */
        if (iris_map_frame(text_slot, IRIS_CPTR_OWN_VSPACE, untyped,
                           LU_SLOT_PT, text_va, text_bytes,
                           (uint64_t)PROG_PROT_RX) != 0)
            return LU_NO_TEXT_MAP;

        /* It is an ELF image, so its first four bytes are known.  A mapping
         * that "succeeded" onto the wrong memory reads back something else. */
        {
            const volatile uint8_t *t = (const volatile uint8_t *)(uintptr_t)text_va;
            if (t[0] != 0x7fu || t[1] != 'E' || t[2] != 'L' || t[3] != 'F')
                return LU_BAD_TEXT;
        }

        pa = iris_invoke0((long)text_slot, INV_FRAME_GET_ADDRESS);
        if (pa <= 0) return LU_NO_TEXT_PADDR;
        text_pa = (uint64_t)pa;
    }

    /* ── the library's data: read the master, keep a copy of its own ── */
    {
        long sz = iris_invoke0((long)data_slot, INV_FRAME_SIZE);
        long pa;
        if (sz <= 0) return LU_NO_DATA_MAP;
        data_bytes = (uint64_t)sz;
        if (!prog_mem_reserve(&g_mem, data_bytes, /*heap=*/0, &data_va))
            return LU_NO_DATA_MAP;
        if (iris_map_frame(data_slot, IRIS_CPTR_OWN_VSPACE, untyped,
                           LU_SLOT_PT, data_va, data_bytes,
                           (uint64_t)PROG_PROT_R) != 0)
            return LU_NO_DATA_MAP;

        /*
         * A frame of its OWN, out of its OWN budget.
         *
         * This is where "there is no copy-on-write" stops being a limitation
         * and becomes the design: the copy is explicit, it is charged to the
         * process that has it, and nothing about it is shared with the other
         * consumer.  A system that mapped the master writable would look
         * identical until the first write.
         */
        priv_va = prog_mmap(&g_mem, data_bytes, PROG_PROT_RW);
        if (priv_va == 0u) return LU_NO_PRIVATE;
        {
            const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)data_va;
            volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)priv_va;
            for (uint64_t i = 0; i < data_bytes; i++) dst[i] = src[i];
            /* ...and now write into it, which is the thing the master cannot
             * do and the thing the other process must not see. */
            dst[0] = (uint8_t)(0xE0u + id);
            if (dst[0] != (uint8_t)(0xE0u + id)) return LU_DATA_READBACK;
        }
        /* The master has been copied; holding a mapping of it past that point
         * is a mapping of somebody else's memory for no reason. */
        (void)iris_invoke2((long)data_slot, INV_FRAME_UNMAP,
                           (long)IRIS_CPTR_OWN_VSPACE, (long)data_va);

        {
            int idx = prog_mem_find(&g_mem, priv_va, 1u);
            if (idx < 0) return LU_NO_PRIVATE;
            pa = iris_invoke0((long)g_mem.maps[idx].slot, INV_FRAME_GET_ADDRESS);
            if (pa <= 0) return LU_NO_PRIVATE;
            priv_pa = (uint64_t)pa;
        }
    }

    /* Round one: where the text physically is, where this process's private
     * data physically is, and which process is speaking. */
    if (lu_report(OBJREG_ROUND_MAPPED, text_pa, priv_pa, id) != 0) return LU_NO_PARENT;

    /* Round two: the object is still ours.  Asked rather than touched — a read
     * would prove it too, and would kill this process the moment it stopped
     * being true, which is precisely the moment the parent is waiting to hear
     * about. */
    {
        long what = iris_invoke0((long)text_slot, INV_CAP_IDENTIFY);
        if (what < 0) return LU_GONE_EARLY;
        if (lu_report(OBJREG_ROUND_BEFORE, (uint64_t)what, text_pa, id) != 0)
            return LU_NO_PARENT;
    }

    /*
     * Round three, after the parent has revoked it in the registry.
     *
     * Two facts, and they are DIFFERENT facts, which is the whole reason this
     * round reports both.
     *
     * The CAPABILITY must be gone — in this process and in the other one, from
     * one invocation in a third.  That is what revoke is.
     *
     * The MAPPING is not.  `CSpace_Revoke` is capability-scoped: it destroys
     * the derived capabilities and does not touch a page table, because a live
     * mapping holds its own reference to the frame (T137 pins this).  So this
     * program can still READ the library it can no longer name, until it
     * unmaps it — and it reports that rather than assuming either way, because
     * a system that changed its mind about this would otherwise change it
     * silently.
     *
     * Revoke is therefore withdrawal of AUTHORITY, not eviction: the holder
     * cannot map it again, cannot pass it on, and cannot ask anything about
     * it, and the memory is not reclaimable until the last mapping goes.
     */
    {
        long what = iris_invoke0((long)text_slot, INV_CAP_IDENTIFY);
        const volatile uint8_t *t = (const volatile uint8_t *)(uintptr_t)text_va;
        uint64_t still_readable;
        if (what >= 0) return LU_STILL_THERE;
        still_readable = (t[0] == 0x7fu && t[1] == 'E') ? 1u : 0u;
        if (lu_report(OBJREG_ROUND_AFTER, (uint64_t)(uint32_t)(-what),
                      still_readable, id) != 0)
            return LU_NO_PARENT;
    }

    /* Its own memory is untouched by any of that: the private copy was never
     * the registry's to take. */
    {
        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)priv_va;
        if (p[0] != (uint8_t)(0xE0u + id)) return LU_DATA_READBACK;
    }
    return LU_OK;
}
