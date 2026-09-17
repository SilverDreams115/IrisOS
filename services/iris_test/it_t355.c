/*
 * it_t355.c — the bytes a ring-3 driver read are the bytes on the disk.
 *
 * ── What T353 and T354 do not cover ────────────────────────────────────────
 *
 * T353 proves a device's DMA is containable, with a device that exists to be
 * driven.  T354 proves ring 3 can read the firmware's description of the
 * machine.  Neither proves the arrangement can carry a SUBSYSTEM: a driver
 * that is useful, that a real system would depend on, doing the thing it
 * exists for.
 *
 * This does.  `blk` is an AHCI driver in ring 3 that found its controller
 * through `pci`, built command structures in memory it owns, contained the
 * controller's DMA behind a remapping unit when the machine has one, and
 * issued READ DMA EXT to a real disk.  What is asserted here is the last link:
 * that what came back is what is on the medium.
 *
 * ── How you can tell ───────────────────────────────────────────────────────
 *
 * The disk QEMU attaches is a FAT filesystem, and the last two bytes of a FAT
 * boot sector are 0x55 0xAA.  That signature is not something a driver can
 * produce by accident: a transfer that never happened leaves the buffer as the
 * driver zeroed it, a transfer to the wrong address leaves it unchanged, and a
 * transfer of the wrong sector lands on data that does not end in 0x55AA.  One
 * comparison separates "the command completed" from "the data arrived", and
 * those are very different claims about a bus master.
 *
 * The buffer arrives as a READ-ONLY frame capability, which is the ownership
 * story in `blk_ep_proto.h`: the service reuses one frame and revokes what it
 * handed out before each read, so a capability from the last read is gone
 * rather than watching its data change underneath.
 *
 * Invariants: D-9, D-10, M3, and the Stage 10-dma containment claim as a
 * subsystem depends on it.
 */

#include "it_priv.h"
#include <iris/blk_ep_proto.h>

#define T355_LEAF_BUF  (IT_OBJ_SLOT_SPAN + 44u)   /* 244, which T354 releases */
#define T355_VA        0x8080000000ULL

static long t355_blk(uint64_t op, uint64_t a0, uint64_t a1,
                     long recv, struct iris_msg *out) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = op;
    m.words[0]   = a0;
    m.words[1]   = a1;
    m.word_count = 2u;
    m.recv_slot  = recv;
    long r = iris_msg_call((long)IRIS_CPTR_BLK_EP_TEST, &m);
    if (out) *out = m;
    if (r != 0) return r;
    return (m.label == BLK_REP_OK) ? 0 : (long)IRIS_ERR_NOT_FOUND;
}

void test_t355(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "disk read";

    if (!it_setup_self_vspace()) { it_fail("T355", "vspace self"); return; }

    /* ── 1. is there a disk, and is its controller contained? ────────────*/
    struct iris_msg info;
    if (t355_blk(BLK_OP_INFO, 0, 0, 0, &info) != 0) {
        it_fail("T355", "the disk service did not answer"); return;
    }
    if (!(info.words[0] & 1u)) {
        /* Not a kernel failure — a machine with no disk, or a controller the
         * driver could not bring up.  Said out loud, because a run that
         * quietly stopped proving anything must not look like one that
         * passed. */
        it_serial_write("[IRIS][TEST] T355 no disk behind the service\n");
        it_fail("T355", "no disk present"); return;
    }

    uint32_t w7[4];
    int have_iommu = it_sched_ext7(w7) && (w7[IT_S7_TRANSLATING] != 0u);
    /*
     * A bus master on a machine with a remapping unit must be contained, and
     * this one is the reason the unit is worth having: AHCI takes physical
     * addresses FROM THE DRIVER, so a driver that lied would have the
     * controller write wherever it liked.  On a machine with no unit it is
     * open, and that is the machine rather than the driver.
     */
    if (ok && have_iommu && !(info.words[3] & 1u)) {
        ok = 0; why = "a bus master is loose on a machine that can contain it";
    }
    if (ok && !have_iommu && (info.words[3] & 1u)) {
        ok = 0; why = "the driver claims containment with no unit to contain it";
    }

    /* ── 2. read sector zero, and take the frame it landed in ────────────*/
    struct iris_msg r;
    long fr = (long)IT_OBJ_CPTR(T355_LEAF_BUF);
    if (ok) {
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE,
                         (long)T355_LEAF_BUF);
        if (t355_blk(BLK_OP_READ, 0, 1, fr, &r) != 0) {
            ok = 0; why = "the read was refused";
        } else if (r.got_caps == 0u) {
            ok = 0; why = "the read returned no buffer";
        } else if (r.words[0] != BLK_SECTOR_BYTES) {
            ok = 0; why = "the read transferred the wrong number of bytes";
        }
    }
    /* The buffer is handed over READ-ONLY.  A client of a block service has no
     * business writing the service's DMA target, and the rights the reply
     * carried are how it is told so. */
    if (ok && (r.got_caps & RIGHT_WRITE) != 0u) {
        ok = 0; why = "the disk buffer arrived writable";
    }

    if (ok && it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)T355_VA, 0) != 0) {
        ok = 0; why = "map the disk buffer";
    }

    /* ── 3. and it is what is on the disk ────────────────────────────────*/
    if (ok) {
        const volatile uint8_t *sec = (const volatile uint8_t *)(uintptr_t)T355_VA;
        if (sec[510] != 0x55u || sec[511] != 0xAAu) {
            ok = 0; why = "sector zero is not a boot sector";
        } else {
            it_serial_write("[IRIS][TEST] T355 sector 0 read, boot signature ok,"
                            " controller sid ");
            it_log_hex(info.words[2]);
            it_serial_write(have_iommu ? " (dma contained)\n" : " (dma open)\n");
        }
    }

    /*
     * ── 4. and the previous buffer is GONE ───────────────────────────────
     *
     * The service reuses one frame, so it revokes what it handed out before
     * each read.  A second read must therefore take this mapping away: not
     * change what it shows — take it away, because a client watching its data
     * change under a capability it still holds is the failure mode the revoke
     * exists to prevent.
     */
    if (ok) {
        struct iris_msg r2;
        if (t355_blk(BLK_OP_READ, 0, 1, 0, &r2) != 0) {
            ok = 0; why = "the second read was refused";
        } else if (r2.words[1] <= r.words[1]) {
            ok = 0; why = "the buffer generation did not advance";
        } else if (it_invoke2(fr, INV_FRAME_UNMAP, IT_VS, (long)T355_VA) == 0) {
            /* The capability survived a revoke that should have taken it. */
            ok = 0; why = "a revoked buffer capability still worked";
        }
    }

    (void)it_invoke2(fr, INV_FRAME_UNMAP, IT_VS, (long)T355_VA);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)T355_LEAF_BUF);
    it_quiesce_reaper();
    if (ok) it_pass("T355"); else it_fail("T355", why);
}
