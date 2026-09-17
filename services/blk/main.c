/*
 * blk/main.c — an AHCI disk driver in ring 3 (Stage 10).
 *
 * The design and the ownership rules are in `iris/blk_ep_proto.h`.  This file
 * is the driver: find the controller through `pci`, bring up a port, contain
 * its DMA behind a remapping unit when the machine has one, and read sectors.
 *
 * ── The shape of an AHCI transfer, and why it is all about physical addresses
 *
 * The controller is a bus master.  A read is not "ask the controller for
 * bytes"; it is "write a command table into memory, tell the controller where
 * that table is, and let it fetch the table and write the data itself".  Four
 * physical addresses are involved and the driver must know every one of them:
 * the command list, the received-FIS area, the command table, and the data
 * buffer.  `INV_FRAME_GET_ADDRESS` is the only way ring 3 can learn any of
 * them, and is why that invocation exists.
 *
 * It is also why this driver is the one that most needs Stage 10-dma.  Those
 * four addresses are supplied BY THE DRIVER, so a driver that lied would have
 * the controller write wherever it liked.  With a remapping unit the addresses
 * are translated through a table only this driver's IOSpace names, and the
 * worst a lie can do is fault.
 */
#include <stdint.h>
#include "../common/iris_msg.h"
#include "../common/iris_map.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/pci_ep_proto.h>
#include <iris/blk_ep_proto.h>

static void blk_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}

/* ── where this service maps things ──────────────────────────────────────── */
#define BLK_VA_ABAR  0x8090000000ULL   /* the controller's registers, uncached */
#define BLK_VA_CMD   0x8091000000ULL   /* command list / FIS / command table   */
#define BLK_VA_DATA  0x8092000000ULL   /* the data a read lands in             */

/* ── AHCI, as much of it as a read needs ─────────────────────────────────── */
#define AHCI_CAP      0x00u
#define AHCI_GHC      0x04u
#define AHCI_PI       0x0Cu
#define AHCI_GHC_AE   (1u << 31)   /* AHCI enable */

#define AHCI_PORT(n)  (0x100u + (n) * 0x80u)
#define PORT_CLB      0x00u
#define PORT_CLBU     0x04u
#define PORT_FB       0x08u
#define PORT_FBU      0x0Cu
#define PORT_IS       0x10u
#define PORT_CMD      0x18u
#define PORT_TFD      0x20u
#define PORT_SIG      0x24u
#define PORT_SSTS     0x28u
#define PORT_SERR     0x30u
#define PORT_CI       0x38u

#define PORT_CMD_ST   (1u << 0)
#define PORT_CMD_FRE  (1u << 4)
#define PORT_CMD_FR   (1u << 14)
#define PORT_CMD_CR   (1u << 15)

#define TFD_BSY       (1u << 7)
#define TFD_DRQ       (1u << 3)
#define TFD_ERR       (1u << 0)

#define SIG_ATA       0x00000101u
#define DET_PRESENT   3u

/* Layout inside the one command frame. */
#define CMD_LIST_OFF  0x000u   /* 32 headers * 32 bytes = 1 KiB, 1 KiB aligned */
#define CMD_FIS_OFF   0x400u   /* 256 bytes, 256-byte aligned                  */
#define CMD_TBL_OFF   0x500u   /* CFIS + ACMD + reserved + one PRDT entry      */

static volatile uint32_t *abar_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(BLK_VA_ABAR + off);
}
static uint32_t ab_rd(uint32_t off)            { return *abar_reg(off); }
static void     ab_wr(uint32_t off, uint32_t v){ *abar_reg(off) = v; }

/* ── what the driver found and built ─────────────────────────────────────── */
static uint32_t g_ready;          /* a disk is initialised and readable */
static uint32_t g_port;
static uint16_t g_source_id;
static uint32_t g_contained;      /* the controller's DMA is behind a unit */
static uint64_t g_cmd_phys, g_data_phys;
static uint64_t g_generation;

/* ── talking to the bus service ──────────────────────────────────────────── */
static long blk_pci(uint64_t op, uint64_t a0, uint64_t a1,
                    long recv, struct iris_msg *out) {
    struct iris_msg m;
    blk_msg_zero(&m);
    m.label      = op;
    m.words[0]   = a0;
    m.words[1]   = a1;
    m.word_count = 2u;
    m.recv_slot  = recv;
    long r = iris_msg_call((long)BLK_SLOT_PCI_EP, &m);
    if (out) *out = m;
    if (r != 0) return r;
    return (m.label == PCI_REP_OK) ? 0 : -1;
}

/*
 * The SATA controller, by CLASS rather than by identity.
 *
 * A disk driver that matched on vendor:device would drive one machine.  The
 * class code says "this is a SATA controller in AHCI mode" and is the same on
 * every implementation of the specification this driver speaks — which is the
 * entire reason PCI has class codes.
 */
#define CLASS_SATA_AHCI 0x01060100u   /* class 01 subclass 06 prog-if 01 */

static int blk_find_controller(uint32_t *out_index) {
    struct iris_msg r;
    if (blk_pci(PCI_OP_COUNT, 0, 0, 0, &r) != 0) return 0;
    uint32_t n = (uint32_t)r.words[0];
    for (uint32_t i = 0; i < n; i++) {
        if (blk_pci(PCI_OP_INFO, i, 0, 0, &r) != 0) continue;
        if (((uint32_t)r.words[1] & 0xFFFFFF00u) != CLASS_SATA_AHCI) continue;
        *out_index  = i;
        g_source_id = (uint16_t)r.words[2];
        return 1;
    }
    return 0;
}

/* ── memory the controller will reach ────────────────────────────────────── */

static long blk_frame(uint32_t slot, uint64_t bytes, uint64_t *out_phys) {
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)slot);
    if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)slot << 32), (long)bytes) != 0) return -1;
    long a = iris_invoke0((long)slot, INV_FRAME_GET_ADDRESS);
    if (a <= 0) return -1;
    *out_phys = (uint64_t)a;
    return 0;
}

/*
 * Contain the controller, if the machine can.
 *
 * Identity translation — the device address IS the physical address — because
 * the driver then has one number per buffer rather than two, and a command
 * table full of addresses is exactly where a second numbering would go wrong.
 * Nothing requires it: an IOSpace is an address space and its holder chooses
 * the layout.
 *
 * Returns 1 when the controller is contained, 0 when the machine has no unit
 * to contain it with.  The second is not an error — it is the machine.
 */
static int blk_contain(uint64_t a, uint64_t b) {
    if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_IOSPACE | (1ULL << 32)),
                    (long)((uint64_t)BLK_SLOT_IOSPACE << 32), 0) != 0) return 0;
    if (iris_invoke2((long)BLK_SLOT_IOSPACE, INV_IOSPACE_BIND,
                     (long)BLK_SLOT_IOSPACE_C, (long)g_source_id) != 0) return 0;

    /* One walk per buffer.  They are two frames from one Untyped and will
     * usually share every level above the last, which the kernel reports by
     * refusing the install — so a refusal here is not a failure. */
    const uint64_t at[2] = { a, b };
    for (uint32_t w = 0; w < 2u; w++) {
        for (uint32_t i = 0; i < 3u; i++) {
            (void)iris_invoke1(0, INV_CNODE_DELETE, (long)BLK_SLOT_IOPT(i));
            if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                            (long)((uint64_t)IRIS_KOBJ_IO_PAGE_TABLE | (1ULL << 32)),
                            (long)((uint64_t)BLK_SLOT_IOPT(i) << 32), 4096) != 0)
                return 0;
            if (iris_invoke2((long)BLK_SLOT_IOSPACE, INV_IOSPACE_MAP_TABLE,
                             (long)BLK_SLOT_IOPT(i), (long)at[w]) != 0) {
                /* The level is already there, from the other buffer's walk. */
                (void)iris_invoke1(0, INV_CNODE_DELETE, (long)BLK_SLOT_IOPT(i));
            }
        }
    }
    if (iris_invoke((long)BLK_SLOT_IOSPACE, INV_IOSPACE_MAP_FRAME,
                    (long)BLK_SLOT_CMD, (long)a,
                    (long)(RIGHT_READ | RIGHT_WRITE)) != 0) return 0;
    if (iris_invoke((long)BLK_SLOT_IOSPACE, INV_IOSPACE_MAP_FRAME,
                    (long)BLK_SLOT_DATA, (long)b,
                    (long)(RIGHT_READ | RIGHT_WRITE)) != 0) return 0;
    return 1;
}

/* ── bringing a port up ──────────────────────────────────────────────────── */

static int port_stop(uint32_t p) {
    uint32_t off = AHCI_PORT(p) + PORT_CMD;
    ab_wr(off, ab_rd(off) & ~(uint32_t)(PORT_CMD_ST | PORT_CMD_FRE));
    for (uint32_t i = 0; i < 100000u; i++)
        if (!(ab_rd(off) & (PORT_CMD_CR | PORT_CMD_FR))) return 1;
    return 0;
}

static int blk_port_init(uint32_t p) {
    if (!port_stop(p)) return 0;

    ab_wr(AHCI_PORT(p) + PORT_CLB,  (uint32_t)((g_cmd_phys + CMD_LIST_OFF) & 0xFFFFFFFFu));
    ab_wr(AHCI_PORT(p) + PORT_CLBU, (uint32_t)((g_cmd_phys + CMD_LIST_OFF) >> 32));
    ab_wr(AHCI_PORT(p) + PORT_FB,   (uint32_t)((g_cmd_phys + CMD_FIS_OFF) & 0xFFFFFFFFu));
    ab_wr(AHCI_PORT(p) + PORT_FBU,  (uint32_t)((g_cmd_phys + CMD_FIS_OFF) >> 32));

    ab_wr(AHCI_PORT(p) + PORT_SERR, 0xFFFFFFFFu);   /* write-1-to-clear */
    ab_wr(AHCI_PORT(p) + PORT_IS,   0xFFFFFFFFu);

    uint32_t off = AHCI_PORT(p) + PORT_CMD;
    ab_wr(off, ab_rd(off) | PORT_CMD_FRE);
    ab_wr(off, ab_rd(off) | PORT_CMD_ST);
    return 1;
}

/* ── one READ DMA EXT ────────────────────────────────────────────────────── */

static int blk_read(uint64_t lba, uint32_t sectors) {
    volatile uint32_t *list = (volatile uint32_t *)(uintptr_t)(BLK_VA_CMD + CMD_LIST_OFF);
    volatile uint8_t  *tbl  = (volatile uint8_t  *)(uintptr_t)(BLK_VA_CMD + CMD_TBL_OFF);
    uint64_t tbl_phys = g_cmd_phys + CMD_TBL_OFF;

    for (uint32_t i = 0; i < 256u; i++) tbl[i] = 0;

    /* Command header 0: a five-dword command FIS, one PRDT entry, and where
     * the command table is.  The controller fetches all of this itself. */
    list[0] = 5u | (1u << 16);
    list[1] = 0u;                                   /* PRD byte count, written back */
    list[2] = (uint32_t)(tbl_phys & 0xFFFFFFFFu);
    list[3] = (uint32_t)(tbl_phys >> 32);
    for (uint32_t i = 4; i < 8u; i++) list[i] = 0u;

    /* The command itself: a host-to-device register FIS carrying READ DMA EXT
     * with a 48-bit LBA.  `device` bit 6 selects LBA rather than CHS. */
    tbl[0]  = 0x27u;                 /* FIS type: register H2D */
    tbl[1]  = 0x80u;                 /* this is a COMMAND, not a control update */
    tbl[2]  = 0x25u;                 /* READ DMA EXT */
    tbl[4]  = (uint8_t)(lba      );
    tbl[5]  = (uint8_t)(lba >>  8);
    tbl[6]  = (uint8_t)(lba >> 16);
    tbl[7]  = 0x40u;                 /* device: LBA mode */
    tbl[8]  = (uint8_t)(lba >> 24);
    tbl[9]  = (uint8_t)(lba >> 32);
    tbl[10] = (uint8_t)(lba >> 40);
    tbl[12] = (uint8_t)(sectors     );
    tbl[13] = (uint8_t)(sectors >> 8);

    /* PRDT entry 0 at offset 0x80: where the data goes, and how much. */
    {
        volatile uint32_t *prd = (volatile uint32_t *)(tbl + 0x80);
        prd[0] = (uint32_t)(g_data_phys & 0xFFFFFFFFu);
        prd[1] = (uint32_t)(g_data_phys >> 32);
        prd[2] = 0u;
        prd[3] = (uint32_t)(sectors * BLK_SECTOR_BYTES - 1u);   /* byte count - 1 */
    }

    /* Wait for the port to be idle, issue, and wait for it to finish.  Bounded
     * because a driver that spins forever on a device that never answers is a
     * service that stops answering. */
    uint32_t tfd = AHCI_PORT(g_port) + PORT_TFD;
    for (uint32_t i = 0; ; i++) {
        if (!(ab_rd(tfd) & (TFD_BSY | TFD_DRQ))) break;
        if (i >= 1000000u) return 0;
    }
    ab_wr(AHCI_PORT(g_port) + PORT_IS, 0xFFFFFFFFu);
    ab_wr(AHCI_PORT(g_port) + PORT_CI, 1u);

    for (uint32_t i = 0; ; i++) {
        if (!(ab_rd(AHCI_PORT(g_port) + PORT_CI) & 1u)) break;
        if (i >= 2000000u) return 0;
    }
    if (ab_rd(tfd) & TFD_ERR) return 0;
    return 1;
}

/* ── startup ─────────────────────────────────────────────────────────────── */

static void blk_bring_up(void) {
    uint32_t dev;
    if (!blk_find_controller(&dev)) return;

    /* The register window, as a capability, from the service that owns the
     * PCI hole.  BAR5 is where AHCI puts its registers. */
    struct iris_msg r;
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)BLK_SLOT_ABAR);
    if (blk_pci(PCI_OP_CLAIM, dev, 5u, (long)BLK_SLOT_ABAR, &r) != 0) return;
    if (r.got_caps == 0u) return;
    if (blk_pci(PCI_OP_ENABLE, dev,
                PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0, &r) != 0) return;

    /* Uncached: a controller register read answered from a cache line is a
     * read of what the register said some time ago. */
    if (iris_map_frame(BLK_SLOT_ABAR, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, BLK_SLOT_PT,
                       BLK_VA_ABAR, r.words[1], 1ull | 4ull) != 0) return;

    if (blk_frame(BLK_SLOT_CMD,  4096u, &g_cmd_phys)  != 0) return;
    if (blk_frame(BLK_SLOT_DATA, 4096u, &g_data_phys) != 0) return;

    /* Before the controller is told any address, decide what it may reach. */
    g_contained = (uint32_t)blk_contain(g_cmd_phys, g_data_phys);

    if (iris_map_frame(BLK_SLOT_CMD, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, BLK_SLOT_PT,
                       BLK_VA_CMD, 4096u, 1ull) != 0) return;
    if (iris_map_frame(BLK_SLOT_DATA, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, BLK_SLOT_PT,
                       BLK_VA_DATA, 4096u, 1ull) != 0) return;
    {
        volatile uint8_t *z = (volatile uint8_t *)(uintptr_t)BLK_VA_CMD;
        for (uint32_t i = 0; i < 4096u; i++) z[i] = 0;
    }

    ab_wr(AHCI_GHC, ab_rd(AHCI_GHC) | AHCI_GHC_AE);

    /* The first implemented port with an ATA disk on it.  A machine with more
     * than one disk has more than one port, and serving them all is a table
     * rather than a variable — which this driver does not have yet, and says
     * so rather than pretending the first is the only. */
    uint32_t pi = ab_rd(AHCI_PI);
    for (uint32_t p = 0; p < 32u; p++) {
        if (!(pi & (1u << p))) continue;
        if ((ab_rd(AHCI_PORT(p) + PORT_SSTS) & 0xFu) != DET_PRESENT) continue;
        if (ab_rd(AHCI_PORT(p) + PORT_SIG) != SIG_ATA) continue;
        g_port = p;
        if (!blk_port_init(p)) return;
        /* Prove it before claiming it: a port that was configured and cannot
         * read is not a disk this service should advertise. */
        if (!blk_read(0, 1)) return;
        g_ready = 1u;
        g_generation = 1u;
        return;
    }
}

/* ── the service loop ────────────────────────────────────────────────────── */

void blk_main(iris_cptr_t bootstrap_ch_h);
void blk_main(iris_cptr_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    blk_bring_up();

    for (;;) {
        struct iris_msg m;
        blk_msg_zero(&m);
        m.reply = (long)BLK_SLOT_REPLY;
        if (iris_msg_recv((long)BLK_SLOT_CTRL_EP, &m) != 0) continue;

        struct iris_msg rep;
        blk_msg_zero(&rep);
        rep.label = BLK_REP_ERR;

        if (m.label == BLK_OP_INFO) {
            rep.label      = BLK_REP_OK;
            rep.words[0]   = g_ready;
            rep.words[1]   = BLK_SECTOR_BYTES;
            rep.words[2]   = g_source_id;
            rep.words[3]   = g_contained;
            rep.word_count = 4u;
        } else if (m.label == BLK_OP_READ && g_ready) {
            uint32_t sectors = (uint32_t)m.words[1];
            if (sectors >= 1u && sectors <= BLK_MAX_SECTORS) {
                /*
                 * Revoke before reuse.  The buffer is one frame and the next
                 * read overwrites it, so a client still holding the last
                 * read's capability must LOSE it rather than watch its data
                 * change underneath.  That is the whole ownership story, and
                 * it is why this can be one frame instead of one per request.
                 */
                (void)iris_invoke0((long)BLK_SLOT_DATA, INV_CSPACE_REVOKE);
                if (blk_read(m.words[0], sectors)) {
                    g_generation++;
                    rep.label      = BLK_REP_OK;
                    rep.words[0]   = sectors * BLK_SECTOR_BYTES;
                    rep.words[1]   = g_generation;
                    rep.word_count = 2u;
                    rep.cap        = (long)BLK_SLOT_DATA;
                    rep.cap_rights = RIGHT_READ;
                }
            }
        }

        (void)iris_msg_reply((long)BLK_SLOT_REPLY, &rep);
    }
}
