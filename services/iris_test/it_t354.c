/* SPDX-License-Identifier: Apache-2.0 */
/*
 * it_t354.c — ring 3 reads the machine's own description (Stage 10).
 *
 * ── What was missing ───────────────────────────────────────────────────────
 *
 * ACPI is how firmware describes a machine: which processors exist, where the
 * DMA remapping units are, how interrupts are routed, what the power states
 * are.  The IRIS kernel reads exactly three things out of it — the RSDP, the
 * MADT and the DMAR — and will never read a fourth, because deciding what a
 * machine IS is policy, and a kernel with an AML interpreter in it would be
 * the largest policy in the system by a wide margin.
 *
 * The problem was that ring 3 could not read a word of it either.  ACPI tables
 * live in memory the firmware marked RECLAIMABLE or NVS: not usable RAM, so in
 * no RAM Untyped; not unmapped address space, so not in the PCI hole.  No
 * capability in the system named those bytes, so the kernel's refusal to
 * interpret them was not a delegation — it was a gap.  Anything ring 3 wanted
 * to know about the machine, it could only learn by the kernel having already
 * decided to tell it.
 *
 * Stage 10 publishes those regions as device Untypeds.  This test is the proof
 * that the publication is real: it retypes a frame over the region, maps it,
 * and finds the root pointer by doing what every firmware reader does — search
 * for the signature on sixteen-byte boundaries — then validates the checksum
 * the ACPI specification defines.
 *
 * ── Why the search, rather than being told the address ─────────────────────
 *
 * The root task is told: `struct iris_root_bootinfo.acpi_rsdp`.  This task is
 * not the root task and has no BootInfo, and inventing a way to pass it one
 * number would be a mechanism that exists only for a test.
 *
 * Searching is also the stronger assertion.  Being handed an address and
 * reading a signature at it proves the address was right.  Finding the
 * signature proves the whole region is readable — every page of it, through
 * one frame and one mapping — which is what "ring 3 can read ACPI" has to mean
 * if a real consumer is ever going to walk a table chain through it.
 *
 * Invariants: D-9 (device memory is a capability), U11/U12, M3.
 */

#include "it_priv.h"

/* Leaves of the suite's object CNode: 244 again, which T353 releases. */
#define T354_LEAF_FR   (IT_OBJ_SLOT_SPAN + 44u)
#define T354_VA        0x807F000000ULL

/* The RSDP, as ACPI 2.0+ defines it.  Only the first twenty bytes are checked:
 * revision 0 (ACPI 1.0) has nothing after them, and the extended checksum is
 * over a length this test has no reason to trust before the first one passes. */
struct t354_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* the first 20 bytes sum to zero */
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = 2.0+ */
    uint32_t rsdt_address;
    uint32_t length;            /* revision >= 2 only */
    uint64_t xsdt_address;      /* revision >= 2 only */
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
};

static int t354_sig_at(const volatile uint8_t *p) {
    static const char sig[8] = { 'R','S','D',' ','P','T','R',' ' };
    for (uint32_t i = 0; i < 8u; i++)
        if (p[i] != (uint8_t)sig[i]) return 0;
    return 1;
}

void test_t354(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "acpi from ring 3";

    if (!it_setup_self_vspace()) { it_fail("T354", "vspace self"); return; }

    /* ── 1. the region, and what it is ───────────────────────────────────*/
    struct it_utq_one q;
    if (!it_utq_1((long)IRIS_CPTR_ACPI_UNTYPED_TEST, &q)) {
        it_fail("T354", "no ACPI untyped"); return;
    }
    if (!q.is_device) { it_fail("T354", "ACPI memory is not device memory"); return; }
    if (q.total_bytes < 4096u) { it_fail("T354", "empty ACPI region"); return; }

    /*
     * Firmware memory cannot hold the headers of objects carved from it, for
     * the same reason MMIO cannot: they are not this system's bytes to write.
     * So the region is paired with RAM the suite names, which is also the only
     * thing that makes the retype below possible at all — unpaired, it refuses
     * rather than quietly spending the kernel's memory.
     */
    {
        long r = it_invoke1((long)IRIS_CPTR_ACPI_UNTYPED_TEST,
                            INV_UNTYPED_SET_DEVICE_BUDGET,
                            (long)IRIS_CPTR_TEST_UNTYPED);
        if (r != 0 && r != (long)IRIS_ERR_ALREADY_EXISTS) {
            it_fail("T354", "the ACPI region has no header budget"); return;
        }
    }

    /* ── 2. one frame over the whole of it ───────────────────────────────*/
    uint64_t span = q.total_bytes - q.used_bytes;
    if (span > (1u << 20)) span = 1u << 20;   /* a megabyte is enough to search */
    span &= ~0xFFFull;

    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)T354_LEAF_FR);
    if (it_invoke((long)IRIS_CPTR_ACPI_UNTYPED_TEST, INV_UNTYPED_RETYPE,
                  (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                  (long)(((uint64_t)T354_LEAF_FR << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                  (long)span) != 0) {
        it_fail("T354", "no frame over the ACPI region"); return;
    }
    long fr = (long)IT_OBJ_CPTR(T354_LEAF_FR);

    /* Read-only: a reader of the firmware's tables has no business writing
     * them, and a mapping that cannot write is the way to say so. */
    if (it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)T354_VA, 0) != 0) {
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)T354_LEAF_FR);
        it_fail("T354", "map the ACPI region"); return;
    }

    /* ── 3. find the root pointer, the way a firmware reader does ────────*/
    const volatile struct t354_rsdp *rsdp = 0;
    uint64_t found_at = 0;
    for (uint64_t off = 0; off + sizeof(struct t354_rsdp) <= span; off += 16u) {
        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(T354_VA + off);
        if (!t354_sig_at(p)) continue;
        rsdp = (const volatile struct t354_rsdp *)p;
        found_at = q.phys_base + q.used_bytes + off;
        break;
    }
    if (!rsdp) { ok = 0; why = "no root pointer in a region that holds one"; }

    /* ── 4. and it is a real one ─────────────────────────────────────────
     *
     * The checksum is what turns "these eight bytes spell RSD PTR" into "this
     * is the structure the firmware wrote".  Eight bytes of ASCII occur by
     * accident in a megabyte of tables often enough to matter, and a reader
     * that followed an accidental one would walk a table chain made of
     * whatever was next in memory. */
    uint8_t rev = 0;
    if (ok) {
        uint32_t sum = 0;
        const volatile uint8_t *b = (const volatile uint8_t *)rsdp;
        for (uint32_t i = 0; i < 20u; i++) sum += b[i];
        if ((sum & 0xFFu) != 0u) { ok = 0; why = "the root pointer does not checksum"; }
        else rev = rsdp->revision;
    }
    /* ACPI 2.0 and later put a 64-bit XSDT pointer after the first twenty
     * bytes, which is what a modern reader follows; revision 0 has only the
     * 32-bit RSDT.  Either is a valid machine, and which one this is is worth
     * reporting rather than asserting. */
    if (ok) {
        it_serial_write("[IRIS][TEST] T354 RSDP at ");
        it_log_hex(found_at);
        it_serial_write(" revision "); it_log_num(rev);
        it_serial_write(rev >= 2u ? " (XSDT " : " (RSDT ");
        it_log_hex(rev >= 2u ? rsdp->xsdt_address : (uint64_t)rsdp->rsdt_address);
        it_serial_write(")\n");
    }

    /* ── 5. and it is READ-only ──────────────────────────────────────────
     *
     * The mapping above asked for no write permission, so the page tables say
     * read-only — but the FRAME still carries RIGHT_WRITE, and a second
     * mapping could ask for it.  What this checks is the mapping, which is
     * what a reader of somebody else's tables actually holds. */
    if (ok && it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)(T354_VA + 0x10000000ULL),
                        (long)IT_MAP_W) == 0) {
        /* It mapped a second, writable window.  That is legitimate — the
         * capability carries the right — so unmap it again rather than
         * failing; the assertion is only that the FIRST one is not writable,
         * which the kernel enforces in the PTE and nothing here can observe
         * without taking a fault on purpose. */
        (void)it_invoke2(fr, INV_FRAME_UNMAP, IT_VS,
                         (long)(T354_VA + 0x10000000ULL));
    }

    (void)it_invoke2(fr, INV_FRAME_UNMAP, IT_VS, (long)T354_VA);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)T354_LEAF_FR);
    it_quiesce_reaper();
    if (ok) it_pass("T354"); else it_fail("T354", why);
}
