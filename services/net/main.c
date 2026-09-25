/*
 * net/main.c — an Intel e1000 driver in ring 3 (Stage 10).
 *
 * The interface and the buffer-ownership rules are in `iris/net_ep_proto.h`.
 * This file is the driver: find the NIC through `pci`, build descriptor rings
 * in memory it owns, contain the NIC's DMA when the machine can, and move
 * Ethernet frames.
 *
 * ── Why a network card is the clearest case for Stage 10-dma ───────────────
 *
 * A NIC does not read a buffer when you ask it to.  It reads a RING of
 * descriptors, continuously, at physical addresses the driver wrote into two
 * registers — and it writes received packets into addresses it found in that
 * ring.  Nothing tells it to stop.  A driver that got the addresses wrong
 * would have the card scribbling into memory forever, asynchronously, with no
 * call to attribute it to.
 *
 * That is the shape containment was built for, and it is why this driver binds
 * an IOSpace to its own controller before it writes a single ring address.
 * With a remapping unit, the worst a wrong address can do is fault.
 *
 * ── What it does not do ────────────────────────────────────────────────────
 *
 * It does not parse anything.  No ARP, no IP, no checksum offload, no
 * interrupts — it polls.  A driver that understood ARP would be policy inside
 * a driver; whoever wants a stack builds one on the endpoint.
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
#include <iris/net_ep_proto.h>

static void net_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}

#define NET_VA_BAR   0x80A0000000ULL
#define NET_VA_RING  0x80A1000000ULL
#define NET_VA_RX    0x80A2000000ULL
#define NET_VA_TX    0x80A3000000ULL

/* ── e1000 registers, as much as moving a frame needs ────────────────────── */
#define E1000_CTRL    0x0000u
#define E1000_STATUS  0x0008u
#define E1000_ICR     0x00C0u
#define E1000_IMC     0x00D8u
#define E1000_RCTL    0x0100u
#define E1000_TCTL    0x0400u
#define E1000_TIPG    0x0410u
#define E1000_RDBAL   0x2800u
#define E1000_RDBAH   0x2804u
#define E1000_RDLEN   0x2808u
#define E1000_RDH     0x2810u
#define E1000_RDT     0x2818u
#define E1000_TDBAL   0x3800u
#define E1000_TDBAH   0x3804u
#define E1000_TDLEN   0x3808u
#define E1000_TDH     0x3810u
#define E1000_TDT     0x3818u
#define E1000_MTA     0x5200u
#define E1000_RAL     0x5400u
#define E1000_RAH     0x5404u

#define CTRL_SLU      (1u << 6)    /* set link up */
#define CTRL_ASDE     (1u << 5)    /* auto-speed detect */

#define RCTL_EN       (1u << 1)
#define RCTL_BAM      (1u << 15)   /* accept broadcast */
#define RCTL_SECRC    (1u << 26)   /* strip the ethernet CRC */
#define RCTL_BSIZE1024 (1u << 16)  /* with BSEX clear: 1024-byte buffers */

#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)    /* pad short packets */
#define TCTL_CT_SHIFT 4u
#define TCTL_COLD_SHIFT 12u

#define TXD_CMD_EOP   (1u << 0)
#define TXD_CMD_IFCS  (1u << 1)    /* insert the frame check sequence */
#define TXD_CMD_RS    (1u << 3)    /* report status */
#define TXD_STAT_DD   (1u << 0)
#define RXD_STAT_DD   (1u << 0)
#define RXD_STAT_EOP  (1u << 1)

/* How long a transmit may take before the card is called unresponsive.
 * Generous on purpose: the cost of being wrong the other way is a frame
 * reported lost that the card was about to acknowledge. */
#define NET_TX_MS     1000u

#define NET_RING_LEN  8u           /* RDLEN must be a multiple of 128 bytes,
                                    * and a descriptor is 16 — so eight is the
                                    * smallest ring the hardware will take. */
#define RING_BYTES    (NET_RING_LEN * 16u)
#define RX_RING_OFF   0x000u
#define TX_RING_OFF   0x200u       /* clear of the RX ring, 16-byte aligned */

static volatile uint32_t *reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(NET_VA_BAR + off);
}
static uint32_t rd(uint32_t off)             { return *reg(off); }
static void     wr(uint32_t off, uint32_t v) { *reg(off) = v; }

/* ── state ───────────────────────────────────────────────────────────────── */
static uint32_t g_ready;
static uint16_t g_source_id;
static uint32_t g_contained;
static uint64_t g_mac;
static uint64_t g_ring_phys, g_rx_phys[NET_RX_FRAMES], g_tx_phys;
static uint32_t g_rx_next;        /* the descriptor we will look at next */
static uint64_t g_rx_count;

/* ── the bus ─────────────────────────────────────────────────────────────── */
static long net_pci(uint64_t op, uint64_t a0, uint64_t a1,
                    long recv, struct iris_msg *out) {
    struct iris_msg m;
    net_msg_zero(&m);
    m.label = op; m.words[0] = a0; m.words[1] = a1; m.word_count = 2u;
    m.recv_slot = recv;
    long r = iris_msg_call((long)NET_SLOT_PCI_EP, &m);
    if (out) *out = m;
    if (r != 0) return r;
    return (m.label == PCI_REP_OK) ? 0 : -1;
}

/* Class 02 subclass 00: an Ethernet controller.  By class, not by identity,
 * by CLASS and not by vendor:device -- and that was the wrong lesson to carry
 * here, for a reason worth writing down.
 *
 * The disk driver matches class 01:06:01 because AHCI IS a programming
 * interface: a device that reports it has the register layout the driver
 * knows, whoever made it.  Ethernet class 02:00 says only "this is a network
 * controller".  An e1000 and a Realtek share that class and share no
 * registers at all.
 *
 * So this matched any Ethernet controller on the machine and then wrote e1000
 * register offsets into its BAR.  On the first real machine it found one and
 * reported no link, which is the polite version of what it was doing.
 *
 * It matches the family it was actually written against now, and what it
 * refuses it REPORTS -- "there is a network controller here and it is not one
 * I know" is a different fact from "there is no network controller", and the
 * difference is the whole question of what to write next. */
#define CLASS_ETHERNET 0x02000000u

/* Intel 82540EM / 82545EM: the e1000 this driver was written and tested
 * against.  A device outside this list may well be an e1000 too; it is not one
 * anybody has run this code on, and guessing is what the paragraph above is
 * about. */
#define E1000_VENDOR 0x8086u
static int e1000_known(uint32_t device) {
    return device == 0x100Eu || device == 0x100Fu;
}

/* What was seen and refused, so the machine can say so. */
static uint32_t g_seen_eth;
static uint32_t g_seen_vd;
uint32_t net_seen_eth(void);
uint32_t net_seen_eth(void) { return g_seen_eth; }

static int net_find(uint32_t *out_index) {
    struct iris_msg r;
    g_seen_eth = 0u; g_seen_vd = 0u;
    if (net_pci(PCI_OP_COUNT, 0, 0, 0, &r) != 0) return 0;
    uint32_t n = (uint32_t)r.words[0];
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (net_pci(PCI_OP_INFO, i, 0, 0, &r) != 0) continue;
        if (((uint32_t)r.words[1] & 0xFFFF0000u) != CLASS_ETHERNET) continue;
        uint32_t vd = (uint32_t)r.words[0];
        g_seen_eth++;
        if (!g_seen_vd) g_seen_vd = vd;
        if (found) continue;
        if ((vd & 0xFFFFu) != E1000_VENDOR) continue;
        if (!e1000_known((vd >> 16) & 0xFFFFu)) continue;
        *out_index  = i;
        g_source_id = (uint16_t)r.words[2];
        found = 1;
    }
    return found;
}

/* ── memory the NIC will reach ───────────────────────────────────────────── */
static long net_frame(uint32_t slot, uint64_t *out_phys) {
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)slot);
    if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)slot << 32), 4096) != 0) return -1;
    long a = iris_invoke0((long)slot, INV_FRAME_GET_ADDRESS);
    if (a <= 0) return -1;
    *out_phys = (uint64_t)a;
    return 0;
}

/*
 * Bind an IOSpace to the NIC and map exactly the three frames it may touch.
 * Identity translation, so the addresses in the descriptor rings are the ones
 * the driver already knows.
 */
static int net_contain(void) {
    if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_IOSPACE | (1ULL << 32)),
                    (long)((uint64_t)NET_SLOT_IOSPACE << 32), 0) != 0) return 0;
    if (iris_invoke2((long)NET_SLOT_IOSPACE, INV_IOSPACE_BIND,
                     (long)NET_SLOT_IOSPACE_C, (long)g_source_id) != 0) return 0;

    uint64_t at[2u + NET_RX_FRAMES];
    uint32_t slot[2u + NET_RX_FRAMES];
    at[0] = g_ring_phys; slot[0] = NET_SLOT_RING;
    at[1] = g_tx_phys;   slot[1] = NET_SLOT_TXBUF;
    for (uint32_t i = 0; i < NET_RX_FRAMES; i++) {
        at[2u + i] = g_rx_phys[i]; slot[2u + i] = NET_SLOT_RXBUF(i);
    }
    for (uint32_t w = 0; w < 2u + NET_RX_FRAMES; w++) {
        for (uint32_t i = 0; i < 3u; i++) {
            (void)iris_invoke1(0, INV_CNODE_DELETE, (long)NET_SLOT_IOPT(i));
            if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                            (long)((uint64_t)IRIS_KOBJ_IO_PAGE_TABLE | (1ULL << 32)),
                            (long)((uint64_t)NET_SLOT_IOPT(i) << 32), 4096) != 0)
                return 0;
            /* A refusal means the level is already there from another frame's
             * walk, which is the common case for frames out of one Untyped. */
            if (iris_invoke2((long)NET_SLOT_IOSPACE, INV_IOSPACE_MAP_TABLE,
                             (long)NET_SLOT_IOPT(i), (long)at[w]) != 0)
                (void)iris_invoke1(0, INV_CNODE_DELETE, (long)NET_SLOT_IOPT(i));
        }
        if (iris_invoke((long)NET_SLOT_IOSPACE, INV_IOSPACE_MAP_FRAME,
                        (long)slot[w], (long)at[w],
                        (long)(RIGHT_READ | RIGHT_WRITE)) != 0) return 0;
    }
    return 1;
}

/* ── bring-up ────────────────────────────────────────────────────────────── */
static void net_bring_up(void) {
    uint32_t dev;
    if (!net_find(&dev)) return;

    struct iris_msg r;
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)NET_SLOT_BAR);
    if (net_pci(PCI_OP_CLAIM, dev, 0u, (long)NET_SLOT_BAR, &r) != 0) return;
    if (r.got_caps == 0u) return;
    if (net_pci(PCI_OP_ENABLE, dev,
                PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0, &r) != 0) return;
    if (iris_map_frame(NET_SLOT_BAR, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, NET_SLOT_PT,
                       NET_VA_BAR, r.words[1], 1ull | 4ull) != 0) return;

    if (net_frame(NET_SLOT_RING,  &g_ring_phys) != 0) return;
    for (uint32_t i = 0; i < NET_RX_FRAMES; i++)
        if (net_frame(NET_SLOT_RXBUF(i), &g_rx_phys[i]) != 0) return;
    if (net_frame(NET_SLOT_TXBUF, &g_tx_phys)   != 0) return;

    /* Decide what the card may reach BEFORE telling it any address. */
    g_contained = (uint32_t)net_contain();

    if (iris_map_frame(NET_SLOT_RING, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, NET_SLOT_PT,
                       NET_VA_RING, 4096u, 1ull) != 0) return;
    for (uint32_t i = 0; i < NET_RX_FRAMES; i++)
        if (iris_map_frame(NET_SLOT_RXBUF(i), IRIS_CPTR_OWN_VSPACE,
                           IRIS_CPTR_OWN_UNTYPED, NET_SLOT_PT,
                           NET_VA_RX + (uint64_t)i * 4096u, 4096u, 1ull) != 0) return;
    if (iris_map_frame(NET_SLOT_TXBUF, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, NET_SLOT_PT,
                       NET_VA_TX, 4096u, 1ull) != 0) return;
    {
        volatile uint8_t *z = (volatile uint8_t *)(uintptr_t)NET_VA_RING;
        for (uint32_t i = 0; i < 4096u; i++) z[i] = 0;
    }

    /* Interrupts off: this driver polls, and a card raising an interrupt
     * nobody has routed is a line that stays asserted. */
    wr(E1000_IMC, 0xFFFFFFFFu);
    (void)rd(E1000_ICR);

    /* Link up, and the multicast table filter cleared — whatever the firmware
     * left in it would otherwise decide which frames this interface sees. */
    wr(E1000_CTRL, rd(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);
    for (uint32_t i = 0; i < 128u; i++) wr(E1000_MTA + i * 4u, 0u);

    /* The MAC the card came with.  QEMU programs RAL/RAH from the command
     * line, so reading them is both simpler and more correct than walking the
     * EEPROM — the address that matters is the one the card will answer to. */
    {
        uint32_t lo = rd(E1000_RAL), hi = rd(E1000_RAH);
        g_mac = (uint64_t)lo | ((uint64_t)(hi & 0xFFFFu) << 32);
        /* Address Valid.  A card whose receive address is not marked valid
         * filters every unicast frame, including the replies to its own. */
        wr(E1000_RAH, hi | (1u << 31));
    }

    /* Receive ring: eight descriptors, each pointing at a 256-byte buffer, all
     * eight inside the one frame the NIC was granted. */
    {
        volatile uint64_t *rxd = (volatile uint64_t *)(uintptr_t)(NET_VA_RING + RX_RING_OFF);
        for (uint32_t i = 0; i < NET_RING_LEN; i++) {
            /* Descriptor i takes the i'th buffer, which lives in frame
             * i/4 at offset (i%4)*1024 — the ring is longer than one page. */
            rxd[i * 2u]      = g_rx_phys[i / NET_RX_PER_FRAME] +
                               (uint64_t)(i % NET_RX_PER_FRAME) * NET_FRAME_BYTES;
            rxd[i * 2u + 1u] = 0u;
        }
        wr(E1000_RDBAL, (uint32_t)(g_ring_phys + RX_RING_OFF));
        wr(E1000_RDBAH, (uint32_t)((g_ring_phys + RX_RING_OFF) >> 32));
        wr(E1000_RDLEN, RING_BYTES);
        wr(E1000_RDH, 0u);
        /* The tail is the last descriptor the card may WRITE INTO, so it
         * trails the head by one — a tail equal to the head means the ring is
         * full and nothing is received. */
        wr(E1000_RDT, NET_RING_LEN - 1u);
        wr(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE1024);
    }

    /* Transmit ring: same size, one buffer, because this driver sends one
     * frame at a time and waits for it. */
    {
        volatile uint64_t *txd = (volatile uint64_t *)(uintptr_t)(NET_VA_RING + TX_RING_OFF);
        for (uint32_t i = 0; i < NET_RING_LEN; i++) { txd[i * 2u] = 0; txd[i * 2u + 1u] = 0; }
        wr(E1000_TDBAL, (uint32_t)(g_ring_phys + TX_RING_OFF));
        wr(E1000_TDBAH, (uint32_t)((g_ring_phys + TX_RING_OFF) >> 32));
        wr(E1000_TDLEN, RING_BYTES);
        wr(E1000_TDH, 0u);
        wr(E1000_TDT, 0u);
        wr(E1000_TIPG, 0x0060200Au);            /* the spec's IEEE 802.3 values */
        wr(E1000_TCTL, TCTL_EN | TCTL_PSP |
                       (0x10u << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
    }

    g_ready = 1u;
}

/* ── moving a frame ──────────────────────────────────────────────────────── */

static uint32_t net_send(uint32_t len) {
    if (len == 0u || len > NET_FRAME_BYTES) return 0;
    volatile uint32_t *txd = (volatile uint32_t *)(uintptr_t)(NET_VA_RING + TX_RING_OFF);
    uint32_t tail = rd(E1000_TDT) % NET_RING_LEN;

    ((volatile uint64_t *)txd)[tail * 2u] = g_tx_phys;
    txd[tail * 4u + 2u] = (uint32_t)len |
                          ((uint32_t)(TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS) << 24);
    txd[tail * 4u + 3u] = 0u;

    wr(E1000_TDT, (tail + 1u) % NET_RING_LEN);

    /*
     * Bounded by TIME, not by a count of reads.
     *
     * A driver that spins for ever on a card that never reports is a service
     * that stops answering -- but two million register reads is a different
     * amount of patience on every machine, and it was chosen against the only
     * one this ran on.  An emulated card completes a transmit before the loop
     * begins; a real one at the far end of a PCI bridge, with the link
     * negotiating, does not.
     *
     * A clock that will not answer leaves this unbounded rather than
     * instantaneous: the failure being guarded is a card that never replies,
     * and treating a missing clock as an expired deadline turns a working card
     * into a broken one.
     */
    {
        long t0 = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
        for (;;) {
            if (txd[tail * 4u + 3u] & TXD_STAT_DD) return len;
            long now = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
            if (t0 > 0 && now > 0 &&
                (uint64_t)(now - t0) > (uint64_t)NET_TX_MS * 1000000ull) break;
        }
    }
    return 0;
}

/*
 * The oldest descriptor the card has finished with, or nothing.
 *
 * `g_rx_next` walks the ring rather than reading RDH, because the head is
 * where the card will write NEXT and says nothing about which descriptors the
 * driver has already consumed.
 */
static uint32_t g_rx_skipping;    /* mid-way through a frame too big to hold */

static uint32_t net_recv(uint32_t *out_off) {
    volatile uint32_t *rxd = (volatile uint32_t *)(uintptr_t)(NET_VA_RING + RX_RING_OFF);
    uint32_t i = g_rx_next;
    uint32_t status = (rxd[i * 4u + 3u] >> 0) & 0xFFu;
    uint32_t len    = rxd[i * 4u + 2u] & 0xFFFFu;
    if (!(status & RXD_STAT_DD)) return 0;

    /*
     * A frame longer than one buffer is SPLIT across descriptors by the card,
     * and only the last of them carries EOP.  Dropping the pieces without EOP
     * is not enough: the last piece has EOP and a plausible length, so it
     * would be handed up as if it were a frame — a tail with no Ethernet
     * header, which is a worse failure than losing the frame, because it is
     * one the layer above has no way to recognise.
     *
     * So a split frame is dropped WHOLE: once a piece arrives without EOP,
     * every piece up to and including the next EOP is discarded.
     */
    if (g_rx_skipping) {
        if (status & RXD_STAT_EOP) g_rx_skipping = 0u;
        len = 0;
    } else if (!(status & RXD_STAT_EOP)) {
        g_rx_skipping = 1u;
        len = 0;
    } else if (len == 0u || len > NET_FRAME_BYTES) {
        len = 0;
    }

    /* Where the caller finds it: the frame index and the offset inside it,
     * because the ring no longer fits in one page. */
    *out_off = (i / NET_RX_PER_FRAME) * 4096u +
               (i % NET_RX_PER_FRAME) * NET_FRAME_BYTES;

    /* Hand the descriptor back: clear its status, then move the tail to it so
     * the card may write into it again.  In that order — a tail moved first
     * offers the card a descriptor still marked done. */
    rxd[i * 4u + 3u] = 0u;
    wr(E1000_RDT, i);
    g_rx_next = (i + 1u) % NET_RING_LEN;
    if (len) g_rx_count++;
    return len;
}

/* ── the service loop ────────────────────────────────────────────────────── */

void net_main(iris_cptr_t bootstrap_ch_h);
void net_main(iris_cptr_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    net_bring_up();

    for (;;) {
        struct iris_msg m;
        net_msg_zero(&m);
        m.reply = (long)NET_SLOT_REPLY;
        if (iris_msg_recv((long)NET_SLOT_CTRL_EP, &m) != 0) continue;

        struct iris_msg rep;
        net_msg_zero(&rep);
        rep.label = NET_REP_ERR;

        if (m.label == NET_OP_INFO) {
            rep.label      = NET_REP_OK;
            /*
             * When there is no card, say whether there was NOTHING or
             * something this driver does not know.  Packed into the flags word
             * because a message carries four and the other three are spoken
             * for -- bit 0 is containment, the rest is how many Ethernet
             * controllers were seen and the first one's vendor:device.
             */
            rep.words[0]   = g_ready;
            rep.words[1]   = g_mac;
            rep.words[2]   = g_source_id;
            rep.words[3]   = (g_contained ? 1u : 0u) |
                             ((uint64_t)(g_seen_eth & 0xFFu) << 8) |
                             ((uint64_t)g_seen_vd << 32);
            rep.word_count = 4u;
        } else if (m.label == NET_OP_TXBUF && g_ready) {
            rep.label      = NET_REP_OK;
            rep.words[0]   = NET_FRAME_BYTES;
            rep.word_count = 1u;
            /* Read-WRITE: the driver supplies the memory, the client supplies
             * the frame.  Revoked on the next handout, like every other buffer
             * here, so a stale capability stops working rather than aliasing
             * somebody else's outgoing frame. */
            (void)iris_invoke0((long)NET_SLOT_TXBUF, INV_CSPACE_REVOKE);
            rep.cap        = (long)NET_SLOT_TXBUF;
            rep.cap_rights = RIGHT_READ | RIGHT_WRITE;
        } else if (m.label == NET_OP_SEND && g_ready) {
            uint32_t sent = net_send((uint32_t)m.words[0]);
            if (sent) {
                rep.label      = NET_REP_OK;
                rep.words[0]   = sent;
                rep.word_count = 1u;
            }
        } else if (m.label == NET_OP_RECV && g_ready) {
            uint32_t off = 0;
            uint32_t len = net_recv(&off);
            rep.label      = NET_REP_OK;
            rep.words[0]   = len;
            rep.words[1]   = g_rx_count;
            rep.words[2]   = off;
            rep.word_count = 3u;
            if (len) {
                uint32_t fr = (uint32_t)(off / 4096u);
                (void)iris_invoke0((long)NET_SLOT_RXBUF(fr), INV_CSPACE_REVOKE);
                rep.words[2]   = off % 4096u;   /* offset WITHIN that frame */
                rep.cap        = (long)NET_SLOT_RXBUF(fr);
                rep.cap_rights = RIGHT_READ;
            }
        }

        (void)iris_msg_reply((long)NET_SLOT_REPLY, &rep);
    }
}
