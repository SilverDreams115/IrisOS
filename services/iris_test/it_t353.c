/* SPDX-License-Identifier: Apache-2.0 */
/*
 * it_t353.c — a driver, so that "the DMA is refused" stops being a claim.
 *
 * ── The hole this closes ────────────────────────────────────────────────────
 *
 * Stage 10-dma §10.2 step 6 says, in its own words, what the stage could not
 * prove: "There is no DMA engine under IRIS's control in the test environment,
 * so nothing here watches a device be refused."  Everything before this file
 * is about the KERNEL side — the DMAR is parsed, the units are probed, the
 * root table is installed with nothing present, an IOSpace refuses to bind a
 * device no unit covers, page-table levels are paid for by their holder.  All
 * of it could be true of a kernel that programmed the hardware perfectly and
 * was ignored by it, and the tests would read the same.
 *
 * So this file drives a real bus master.  QEMU's `edu` device is a PCI
 * function with one MMIO BAR, an internal four-kilobyte buffer and a DMA
 * engine that copies between that buffer and whatever physical address its
 * driver writes into a register.  It exists to be driven by a teaching driver,
 * and what it teaches here is the one fact the stage was missing.
 *
 * ── The shape of the proof ──────────────────────────────────────────────────
 *
 * One frame, two halves.  The processor writes a pattern into the first half
 * through its own mapping.  The device is then told to copy that pattern into
 * its internal buffer and copy it back out into the SECOND half, which holds a
 * sentinel the pattern does not resemble.  Reading the second half afterwards
 * answers, with no interpretation required, whether the device reached memory:
 *
 *   with a remapping unit and NO IOSpace mapping   the sentinel is intact
 *   with a remapping unit and the frame mapped     the pattern arrived
 *   with a remapping unit and the mapping revoked  the sentinel is intact
 *   with no remapping unit at all                  the pattern arrived
 *
 * The middle lines are what make the first one mean something.  A sentinel
 * left intact is also what a DMA that was never issued looks like, and a test
 * that stopped at the first line would pass just as happily against a device
 * that was not there.  The only thing that changes between the first and the
 * second is one INV_IOSPACE_MAP_FRAME, and the unit's fault record names the
 * source-id it refused — so the refusal is attributed, not merely observed.
 *
 * The third line is not a fallback, it is the other half of the claim.  On a
 * machine with no unit the device reaches an address nobody granted it, which
 * is exactly the situation the whole stage exists to end, and a suite that
 * only ever ran in the configuration that passes would be evidence of nothing.
 * The gate runs both.
 *
 * ── What the driver has to do, and why each step is a capability operation ──
 *
 *   1. The device is FOUND through the `pci` service (Stage 10), not by this
 *      test reading configuration space.  0xCF8/0xCFC is one pair of ports
 *      through which any device on the machine can be reprogrammed, so a
 *      driver holding a capability for it would hold the bus — exactly the
 *      thing Stage 10-dma closed for DMA, reopened through the config space
 *      that programs the DMA.  One task holds those ports; this one holds an
 *      endpoint and asks.
 *   2. The register window arrives as a FRAME capability, from the same
 *      service, which owns the PCI-hole device Untyped.  This test cannot
 *      retype one for itself, and that is the point rather than a limitation:
 *      it holds no device Untyped, so there is no window it could reach that
 *      it was not handed.  It maps it UNCACHED — a register read answered out
 *      of a cache line is a read of what the register said some time ago.
 *   3. The DMA target is an ordinary frame out of the suite's own Untyped.
 *      Its PHYSICAL address comes from INV_FRAME_GET_ADDRESS, because a device
 *      takes physical addresses and a capability holder is the only one who
 *      may learn where its own frame is.
 *   4. And the thing under test: an IOSpace bound to the device's source-id,
 *      three levels of translation the suite paid for, and the frame mapped
 *      into it.  Nothing else in the system had to be asked.
 *
 * ── What writing it cost, which is the argument for writing drivers ────────
 *
 * Three defects, all of them older than this file and none reachable from any
 * test that does not drive hardware:
 *
 *   · `paging_virt_to_phys` returned `entry & ~0xFFF`, which strips a
 *     page-table entry's LOW flags and keeps every HIGH one — so every
 *     physical address it produced for a non-executable page carried NX at
 *     bit 63.  Every caller in the kernel had got away with it.  The first one
 *     to hand such an address to HARDWARE wrote it into a VT-d root entry,
 *     whose bits 63:39 are reserved, and the unit answered fault reason 10 and
 *     refused every device on the bus — in BOTH arms of this test, which is
 *     what made it findable.
 *   · the I/O-port ABI had only byte-wide IN/OUT, so this driver could not
 *     read PCI configuration space at all.
 *   · every mapping was write-back, so there was no way to map a BAR.
 *
 * Invariants: D-9 (device memory is a capability), U11/U12, M3 (no kernel
 * allocation on this path), and the Stage 10-dma containment claim itself.
 */

#include "it_priv.h"
#include <iris/pci_ep_proto.h>

/*
 * The device.  1234:11e8 is QEMU's `edu`; the register map below is its
 * documented one (docs/specs/edu.txt in the QEMU tree).
 */
#define EDU_VENDOR_DEVICE  0x11E81234u   /* device:vendor as one dword */

#define EDU_REG_ID         0x00u  /* RO, 0x010000ed */
#define EDU_REG_LIVENESS   0x04u  /* RW, reads back the bitwise NOT of a write */
#define EDU_REG_DMA_SRC    0x80u
#define EDU_REG_DMA_DST    0x88u
#define EDU_REG_DMA_CNT    0x90u
#define EDU_REG_DMA_CMD    0x98u

#define EDU_DMA_RUN        0x01u
#define EDU_DMA_TO_PCI     0x02u  /* set: device buffer → memory; clear: memory → device */

/* The device side of a transfer is not memory: it is an offset into the
 * device's own four-kilobyte buffer, and the device treats anything outside
 * that window as a programming error rather than an address. */
#define EDU_BUF_BASE       0x40000ull
#define EDU_BUF_BYTES      4096ull

/* Configuration space is not reachable from here at all — that is the point.
 * `PCI_CMD_*` come from the bus service's protocol, which is the only way this
 * test can ask for a device to be turned on. */

/*
 * Leaves of the suite's object CNode.  244..250 again: T352 is the last test
 * to use them and releases every one before this runs, and the reason to reuse
 * rather than take seven more is the one written above T352's map — the
 * ROTATING pool has a recycle ceiling T324 measures, and a test that holds
 * seven objects across a whole arc spends a budget another test is counting.
 */
#define T353_LEAF_IO    (IT_OBJ_SLOT_SPAN + 44u)      /* 244, the IOSpace */
#define T353_LEAF_L(i)  (IT_OBJ_SLOT_SPAN + 45u + (uint32_t)(i))  /* 245..247 */
#define T353_LEAF_BUF   (IT_OBJ_SLOT_SPAN + 48u)      /* 248, the DMA target */
#define T353_LEAF_BAR   (IT_OBJ_SLOT_SPAN + 49u)      /* 249, the register window */

/* Two windows in the suite's own address space, clear of every VA above. */
#define T353_BAR_VA     0x807D000000ULL
#define T353_BUF_VA     0x807E000000ULL

/* The two halves of the target frame, and what goes in them. */
#define T353_SRC_OFF    0x000u
#define T353_DST_OFF    0x800u
#define T353_BYTES      64u
#define T353_PATTERN    0xD1A9C0DE00000000ull
#define T353_SENTINEL   0x5EE5000000000000ull

/* SYS_FRAME_MAP flags: writable, and NOT cached (bit 2, added for this). */
#define T353_MAP_MMIO   (IT_MAP_W | 4ULL)

/* Long enough for a transfer the device schedules a hundred milliseconds out,
 * and short enough that a device that never finishes fails the test instead of
 * the run. */
#define T353_DMA_WAIT   40u

/* ── the MMIO window, read and written through the suite's mapping ───────── */

static volatile uint32_t *edu_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(T353_BAR_VA + off);
}
static uint32_t edu_rd32(uint32_t off) { return *edu_reg(off); }
static void     edu_wr32(uint32_t off, uint32_t v) { *edu_reg(off) = v; }
static void     edu_wr64(uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(uintptr_t)(T353_BAR_VA + off) = v;
}

/* ── the bus, asked rather than read ─────────────────────────────────────── */

/*
 * One request to the `pci` service.  `recv` is the slot a delivered capability
 * should land in, or 0 when the answer is only numbers.
 */
static long t353_pci(uint64_t op, uint64_t a0, uint64_t a1,
                     long recv, struct iris_msg *out) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = op;
    m.words[0]   = a0;
    m.words[1]   = a1;
    m.word_count = 2u;
    m.recv_slot  = recv;
    long r = iris_msg_call((long)IRIS_CPTR_PCI_EP, &m);
    if (out) *out = m;
    if (r != 0) return r;
    return (m.label == PCI_REP_OK) ? 0 : (long)IRIS_ERR_NOT_FOUND;
}

/*
 * The device, by identity.
 *
 * A driver knows what it drives and nothing else: it walks what the bus
 * service reports and matches on vendor:device.  It never sees a config dword
 * it did not ask for and cannot address a function it did not match.
 */
static int t353_find_device(uint32_t *out_index, uint16_t *out_sid) {
    struct iris_msg r;
    if (t353_pci(PCI_OP_COUNT, 0, 0, 0, &r) != 0) return 0;
    uint32_t n = (uint32_t)r.words[0];
    for (uint32_t i = 0; i < n; i++) {
        if (t353_pci(PCI_OP_INFO, i, 0, 0, &r) != 0) continue;
        if ((uint32_t)r.words[0] != EDU_VENDOR_DEVICE) continue;
        *out_index = i;
        *out_sid   = (uint16_t)r.words[2];
        return 1;
    }
    return 0;
}

/* ── the transfer ────────────────────────────────────────────────────────── */

/*
 * One transfer, waited out.
 *
 * The device clears the run bit when it is DONE, and it is done whether the
 * copy reached memory or was refused at the remapping unit — which is what
 * makes this a wait and not a result.  What happened is a question for the
 * memory afterwards.
 */
static int t353_dma(uint64_t src, uint64_t dst, uint64_t cnt, uint32_t dir) {
    edu_wr64(EDU_REG_DMA_SRC, src);
    edu_wr64(EDU_REG_DMA_DST, dst);
    edu_wr64(EDU_REG_DMA_CNT, cnt);
    edu_wr32(EDU_REG_DMA_CMD, EDU_DMA_RUN | dir);

    for (uint32_t i = 0; i < T353_DMA_WAIT; i++) {
        if (!(edu_rd32(EDU_REG_DMA_CMD) & EDU_DMA_RUN)) return 1;
        it_settle(1);
    }
    return 0;
}

/* The round trip: memory → the device's buffer → memory, into the other half
 * of the same frame.  Either leg being refused leaves the sentinel standing,
 * which is the whole point — a device stopped on the way in is as contained as
 * one stopped on the way out. */
static int t353_round_trip(uint64_t dma_base) {
    if (!t353_dma(dma_base + T353_SRC_OFF, EDU_BUF_BASE, T353_BYTES, 0u))
        return 0;
    if (!t353_dma(EDU_BUF_BASE, dma_base + T353_DST_OFF, T353_BYTES,
                  EDU_DMA_TO_PCI))
        return 0;
    return 1;
}

static void t353_fill(volatile uint64_t *buf) {
    for (uint32_t i = 0; i < T353_BYTES / 8u; i++) {
        buf[(T353_SRC_OFF / 8u) + i] = T353_PATTERN | (uint64_t)i;
        buf[(T353_DST_OFF / 8u) + i] = T353_SENTINEL | (uint64_t)i;
    }
}

static int t353_arrived(volatile uint64_t *buf) {
    for (uint32_t i = 0; i < T353_BYTES / 8u; i++)
        if (buf[(T353_DST_OFF / 8u) + i] != (T353_PATTERN | (uint64_t)i)) return 0;
    return 1;
}

static int t353_untouched(volatile uint64_t *buf) {
    for (uint32_t i = 0; i < T353_BYTES / 8u; i++)
        if (buf[(T353_DST_OFF / 8u) + i] != (T353_SENTINEL | (uint64_t)i)) return 0;
    return 1;
}

/* Whichever unit recorded something, drained so the next attempt starts from
 * nothing.  The unit index is not exposed and does not need to be: there are
 * at most a handful and a record belongs to whichever one holds it. */
static int t353_fault(struct iris_iommu_fault_info *out) {
    for (uint32_t unit = 0; unit < 4u; unit++) {
        long r = it_invoke(IRIS_CPTR_IOSPACE_CONTROL_TEST, INV_IOSPACE_FAULT,
                           (long)unit, (long)(uintptr_t)out,
                           (long)IRIS_IOMMU_FAULT_CLEAR);
        if (r == 1) return 1;
    }
    return 0;
}

static void t353_drain_faults(void) {
    struct iris_iommu_fault_info f;
    for (uint32_t i = 0; i < 8u; i++) if (!t353_fault(&f)) return;
}

static void t353_cleanup(void) {
    (void)it_invoke2((long)IT_OBJ_CPTR(T353_LEAF_BAR), INV_FRAME_UNMAP, IT_VS,
                     (long)T353_BAR_VA);
    (void)it_invoke2((long)IT_OBJ_CPTR(T353_LEAF_BUF), INV_FRAME_UNMAP, IT_VS,
                     (long)T353_BUF_VA);
    for (uint32_t i = 0; i < 3u; i++)
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE,
                         (long)T353_LEAF_L(i));
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)T353_LEAF_IO);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)T353_LEAF_BUF);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)T353_LEAF_BAR);
}

static long t353_retype_leaf(long ut, uint32_t type, uint32_t leaf, uint64_t bytes) {
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)leaf);
    if (it_invoke(ut, INV_UNTYPED_RETYPE,
                  (long)((uint64_t)type | (1ULL << 32)),
                  (long)(((uint64_t)leaf << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                  (long)bytes) != 0) return -1;
    return (long)IT_OBJ_CPTR(leaf);
}

void test_t353(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "dma containment";
    uint32_t w7[4];
    if (!it_sched_ext7(w7)) { it_fail("T353", "containment tier"); return; }
    int have_iommu = (w7[IT_S7_TRANSLATING] != 0u);

    if (!it_setup_self_vspace()) { it_fail("T353", "vspace self"); return; }

    /* ── 1. find the device, through the service that owns the bus ───────*/
    uint32_t dev_index = 0;
    uint16_t source_id = 0;
    if (!t353_find_device(&dev_index, &source_id)) {
        /* Not a failure of the kernel — a failure of the machine to have the
         * device the gate asks for, or of `pci` to have started.  Said out
         * loud so a run that quietly stopped proving anything cannot look like
         * a run that passed. */
        it_serial_write("[IRIS][TEST] T353 no DMA device on the bus\n");
        t353_cleanup();
        it_fail("T353", "no DMA-capable device present");
        return;
    }

    /* ── 2. its register window, as a capability ─────────────────────────*/
    uint64_t bar = 0, bar_size = 0;
    long bar_fr = (long)IT_OBJ_CPTR(T353_LEAF_BAR);
    {
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE,
                         (long)T353_LEAF_BAR);
        struct iris_msg r;
        if (t353_pci(PCI_OP_CLAIM, dev_index, 0, bar_fr, &r) != 0) {
            ok = 0; why = "the bus service would not hand over the window";
        } else if (r.got_caps == 0u) {
            /* A-33: the MessageInfo says whether a capability landed.  A reply
             * carrying only numbers is the answer to "how big is it", and this
             * request was not that. */
            ok = 0; why = "the window arrived without a frame";
        } else {
            bar      = r.words[0];
            bar_size = r.words[1];
            it_serial_write("[IRIS][TEST] T353 device 1234:11e8 sid ");
            it_log_hex(source_id);
            it_serial_write(" bar "); it_log_hex(bar);
            it_serial_write(" size "); it_log_hex(bar_size);
            it_serial_write("\n");
        }
    }
    /* The frame is over the window the service said it was.  Checked rather
     * than trusted: this is the one number the driver did not measure itself,
     * and a frame over the wrong window would drive some other device. */
    if (ok && (uint64_t)it_invoke0(bar_fr, INV_FRAME_GET_ADDRESS) != bar) {
        ok = 0; why = "the frame is not over the window the service named";
    }
    /* Uncached: see PAGE_PCD.  A register read answered from a cache line is
     * not a register read. */
    if (ok && it_invoke(bar_fr, INV_FRAME_MAP, IT_VS, (long)T353_BAR_VA,
                        (long)T353_MAP_MMIO) != 0) {
        ok = 0; why = "map the device window";
    }

    /* ── 3. what the service will NOT do ─────────────────────────────────
     *
     * A driver asks for its own device and gets its own device.  The refusals
     * are what make that a property rather than a convention: an index past
     * the end of the scan and a BAR past the end of a function are both
     * requests for a window that would belong to somebody else if it existed.
     *
     * What this test CANNOT prove is the other half — that a driver is unable
     * to reach configuration space directly — because iris_test holds
     * IRIS_CPTR_IOPORT_CONTROL for two dozen other tests and could therefore
     * claim 0xCF8 for itself.  That is a fact about this task, not about
     * drivers, and pretending otherwise by asserting a refusal that would not
     * happen is worse than saying so.  Where the claim IS made good is init's
     * manifest: it derives the port capability once, hands it to `pci`, and
     * deletes its own copy, so no later task can be given one.
     */
    if (ok) {
        struct iris_msg r;
        if (t353_pci(PCI_OP_CLAIM, 0xFFFFu, 0, 0, &r) == 0) {
            ok = 0; why = "the service claimed a device that does not exist";
        }
    }
    if (ok) {
        struct iris_msg r;
        if (t353_pci(PCI_OP_BAR, dev_index, 99u, 0, &r) == 0) {
            ok = 0; why = "the service described a BAR that cannot exist";
        }
    }

    /* ── 4. the device answers ───────────────────────────────────────────*/
    if (ok) {
        struct iris_msg r;
        if (t353_pci(PCI_OP_ENABLE, dev_index,
                     PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0, &r) != 0) {
            ok = 0; why = "the bus service would not enable the device";
        }
    }
    if (ok) {
        edu_wr32(EDU_REG_LIVENESS, 0x12345678u);
        if (edu_rd32(EDU_REG_LIVENESS) != ~0x12345678u) {
            ok = 0; why = "the device did not answer through its BAR";
        }
    }

    /* ── 5. the memory it will be pointed at ─────────────────────────────*/
    long buf_fr = -1;
    uint64_t dma_base = 0;
    volatile uint64_t *buf = (volatile uint64_t *)(uintptr_t)T353_BUF_VA;
    if (ok) {
        buf_fr = t353_retype_leaf((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_FRAME,
                                  T353_LEAF_BUF, 4096u);
        if (buf_fr < 0) { ok = 0; why = "dma frame"; }
    }
    if (ok) {
        long a = it_invoke0(buf_fr, INV_FRAME_GET_ADDRESS);
        if (a <= 0) { ok = 0; why = "frame address"; }
        else dma_base = (uint64_t)a;
    }
    if (ok && it_invoke(buf_fr, INV_FRAME_MAP, IT_VS, (long)T353_BUF_VA,
                        (long)IT_MAP_W) != 0) {
        ok = 0; why = "map the dma frame";
    }

    /* ── 6. the attempt NOBODY authorised ────────────────────────────────*/
    if (ok) {
        t353_drain_faults();
        t353_fill(buf);
        if (!t353_round_trip(dma_base)) {
            ok = 0; why = "the device never finished a transfer";
        }
    }

    struct iris_iommu_fault_info fault;
    int faulted = 0;
    if (ok) {
        faulted = t353_fault(&fault);
        if (have_iommu) {
            if (!t353_untouched(buf)) {
                ok = 0; why = "a device reached memory nobody mapped for it";
            }
            /* And it was THIS device that was stopped.  "Some fault happened"
             * would also be true on a machine where an idle SATA controller
             * touched memory nobody mapped for it either. */
            if (ok && !faulted) {
                ok = 0; why = "the refusal left no record";
            }
            if (ok && fault.source_id != source_id) {
                ok = 0; why = "the record names another device";
            }
            if (ok) {
                it_serial_write("[IRIS][TEST] T353 refused sid ");
                it_log_hex(fault.source_id);
                it_serial_write(" addr "); it_log_hex(fault.address);
                it_serial_write(" reason "); it_log_num(fault.reason);
                it_serial_write(fault.is_read ? " (read)\n" : " (write)\n");
            }
        } else {
            /*
             * No unit, and so no containment — and this is the arm that says
             * what that costs.  The device was handed a physical address by
             * its driver and reached it, through no translation and past every
             * check this kernel makes.  Asserting it explicitly is what keeps
             * the claim on the other side honest.
             */
            if (!t353_arrived(buf)) {
                ok = 0; why = "the device did not transfer at all";
            }
            if (ok && faulted) { ok = 0; why = "a fault with no unit"; }
            if (ok)
                it_serial_write("[IRIS][TEST] T353 no unit: the device reached "
                                "memory nobody granted it\n");
        }
    }

    /* ── 7. the attempt somebody DID authorise ───────────────────────────*/
    if (ok && have_iommu) {
        long space = t353_retype_leaf((long)IRIS_CPTR_TEST_UNTYPED,
                                      IRIS_KOBJ_IOSPACE, T353_LEAF_IO, 0);
        if (space < 0) { ok = 0; why = "iospace"; }
        if (ok && it_invoke2(space, INV_IOSPACE_BIND,
                             (long)IRIS_CPTR_IOSPACE_CONTROL_TEST,
                             (long)source_id) != 0) {
            ok = 0; why = "bind the device found on the bus";
        }
        for (uint32_t i = 0; ok && i < 3u; i++) {
            long lvl = t353_retype_leaf((long)IRIS_CPTR_TEST_UNTYPED,
                                        IRIS_KOBJ_IO_PAGE_TABLE,
                                        T353_LEAF_L(i), 4096u);
            if (lvl < 0) { ok = 0; why = "io page table"; break; }
            if (it_invoke2(space, INV_IOSPACE_MAP_TABLE, lvl, (long)dma_base) != 0) {
                ok = 0; why = "install a translation level"; break;
            }
        }
        /* The device address is the physical one, so the driver has one number
         * to keep rather than two.  Nothing requires that — an IOSpace is an
         * address space and the holder chooses its layout — but a driver that
         * picks identity is a driver whose descriptors need no translation
         * table of its own. */
        if (ok && it_invoke(space, INV_IOSPACE_MAP_FRAME, buf_fr,
                            (long)dma_base,
                            (long)(RIGHT_READ | RIGHT_WRITE)) != 0) {
            ok = 0; why = "map the frame for the device";
        }

        if (ok) {
            t353_drain_faults();
            t353_fill(buf);
            if (!t353_round_trip(dma_base)) {
                ok = 0; why = "the device never finished the granted transfer";
            }
        }
        if (ok && !t353_arrived(buf)) {
            ok = 0; why = "a device could not reach the frame it was granted";
        }
        if (ok) {
            struct iris_iommu_fault_info f2;
            if (t353_fault(&f2)) { ok = 0; why = "a granted transfer faulted"; }
        }
        if (ok)
            it_serial_write("[IRIS][TEST] T353 granted: the device reached "
                            "exactly the frame it was mapped\n");

        /* And taken back.  The unmap has to reach the unit's cache as well as
         * the table, which is the claim INV_IOSPACE_UNMAP makes; what this
         * checks is that the device is refused again afterwards. */
        if (ok && it_invoke1(space, INV_IOSPACE_UNMAP, (long)dma_base) != 0) {
            ok = 0; why = "unmap";
        }
        if (ok) {
            t353_drain_faults();
            t353_fill(buf);
            if (!t353_round_trip(dma_base)) {
                ok = 0; why = "the device never finished after the revoke";
            }
        }
        if (ok && !t353_untouched(buf)) {
            ok = 0; why = "a revoked mapping still let the device through";
        }
        if (ok && !t353_fault(&fault)) {
            ok = 0; why = "the revoked transfer left no record";
        }
        if (ok)
            it_serial_write("[IRIS][TEST] T353 revoked: the device is refused "
                            "again\n");
    }

    /*
     * The device is left decoding and bus-mastering.
     *
     * Turning it off would take a PCI_OP_DISABLE, and there is deliberately no
     * such operation: a caller that could clear another device's command bits
     * could stop somebody else's hardware, and this test is not a special case
     * — it holds the same endpoint every driver holds.  What makes leaving it
     * on safe is the thing under test: with a remapping unit the device
     * reaches exactly the frame it was mapped, and that mapping is revoked
     * below.  Without one it reaches whatever its registers say, and its
     * registers are left pointing at a frame this test still owns.
     */
    t353_cleanup();
    it_quiesce_reaper();
    if (ok) it_pass("T353"); else it_fail("T353", why);
}
