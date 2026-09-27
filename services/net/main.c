/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net/main.c — an Intel e1000 driver in ring 3.
 *
 * The interface and the buffer-ownership rules are in `iris/net_ep_proto.h`.
 * This file is the driver: find the NIC through `pci`, build descriptor rings
 * in memory it owns, contain the NIC's DMA when the machine can, and move
 * Ethernet frames.
 *
 * ── Why a network card is the clearest case ───────────────
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
#include "netdev.h"

static void net_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}


/* ── e1000 registers, as much as moving a frame needs ────────────────────── */
#define RXD_STAT_EOP  (1u << 1)

/* How long a transmit may take before the card is called unresponsive.
 * Generous on purpose: the cost of being wrong the other way is a frame
 * reported lost that the card was about to acknowledge. */


/* ── state ───────────────────────────────────────────────────────────────── */
static uint32_t g_ready;
static uint16_t g_source_id;
static uint32_t g_contained;
/* How far bring-up got, so that a machine that reports no link says why. */
static uint32_t g_step = NET_STEP_NONE;
/* Everything the chosen backend is given.  The service fills it in; the
 * backend reads it and owns the cursors inside it. */
static struct net_hw g_hw;

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

/*
 * The two questions a backend may ask before anything is claimed.  Both go
 * through the same `pci` endpoint as everything else here; neither can reach
 * a function the scan did not find, and neither can write.
 */
uint32_t net_bus_cfg(uint32_t index, uint32_t off) {
    struct iris_msg r;
    if (net_pci(PCI_OP_CFG_READ, index, off, 0, &r) != 0) return 0xFFFFFFFFu;
    return (uint32_t)r.words[0];
}

int net_bus_bar(uint32_t index, uint32_t bar,
                uint64_t *base, uint64_t *size, uint32_t *flags) {
    struct iris_msg r;
    if (net_pci(PCI_OP_BAR, index, bar, 0, &r) != 0) return 0;
    if (base)  *base  = r.words[0];
    if (size)  *size  = r.words[1];
    if (flags) *flags = (uint32_t)r.words[2];
    return 1;
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

/*
 * The table.  One row per family somebody has actually run this against, and
 * adding hardware is adding a row and a file -- not an edit to anything here.
 */
static const struct net_backend *const NET_BACKENDS[] = {
    &net_backend_e1000,
    &net_backend_virtio,
};
#define NET_BACKEND_COUNT (sizeof(NET_BACKENDS) / sizeof(NET_BACKENDS[0]))

/* What was seen and refused, so the machine can say so. */
static uint32_t g_seen_eth;
static uint32_t g_seen_vd;
static const struct net_backend *g_drv;
uint32_t net_seen_eth(void);
uint32_t net_seen_eth(void) { return g_seen_eth; }

static const struct net_backend *net_pick(uint32_t vd) {
    for (uint32_t b = 0; b < NET_BACKEND_COUNT; b++) {
        const struct net_backend *d = NET_BACKENDS[b];
        if ((vd & 0xFFFFu) != d->vendor) continue;
        if (d->matches && !d->matches((vd >> 16) & 0xFFFFu)) continue;
        return d;
    }
    return 0;
}

static int net_find(uint32_t *out_index) {
    struct iris_msg r;
    g_seen_eth = 0u; g_seen_vd = 0u; g_drv = 0;
    if (net_pci(PCI_OP_COUNT, 0, 0, 0, &r) != 0) return 0;
    uint32_t n = (uint32_t)r.words[0];
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (net_pci(PCI_OP_INFO, i, 0, 0, &r) != 0) continue;
        if (((uint32_t)r.words[1] & 0xFFFF0000u) != CLASS_ETHERNET) continue;
        uint32_t vd = (uint32_t)r.words[0];
        /* Counted whether or not anything drives it: the count and the first
         * vendor:device are what the report uses to say "there is a card here
         * and no backend speaks to it", which is a different fact from "there
         * is no card" and the one that says what to write next. */
        g_seen_eth++;
        if (!g_seen_vd) g_seen_vd = vd;
        if (found) continue;
        const struct net_backend *d = net_pick(vd);
        if (!d) continue;
        *out_index  = i;
        g_source_id = (uint16_t)r.words[2];
        g_drv       = d;
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
    at[0] = g_hw.ring_phys; slot[0] = NET_SLOT_RING;
    at[1] = g_hw.tx_phys;   slot[1] = NET_SLOT_TXBUF;
    for (uint32_t i = 0; i < NET_RX_FRAMES; i++) {
        at[2u + i] = g_hw.rx_phys[i]; slot[2u + i] = NET_SLOT_RXBUF(i);
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

    /*
     * WHICH BAR, asked before it is claimed.  The e1000's registers are in
     * BAR 0 and this service used to write that number down as though it were
     * a property of Ethernet cards.  It is a property of one family: a virtio
     * device puts its registers wherever it likes and says where in its own
     * capability list, which is a thing only the backend can read.
     */
    int bar = 0;
    if (g_drv->probe) {
        bar = g_drv->probe(dev, &g_hw);
        if (bar < 0) { g_step = NET_STEP_PROBE; g_drv = 0; return; }
    }

    struct iris_msg r;
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)NET_SLOT_BAR);
    g_step = NET_STEP_CLAIM;
    if (net_pci(PCI_OP_CLAIM, dev, (uint64_t)(uint32_t)bar,
                (long)NET_SLOT_BAR, &r) != 0) return;
    if (r.got_caps == 0u) return;
    g_step = NET_STEP_ENABLE;
    if (net_pci(PCI_OP_ENABLE, dev,
                PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0, &r) != 0) return;
    g_step = NET_STEP_MAP_BAR;
    if (iris_map_frame(NET_SLOT_BAR, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, NET_SLOT_PT,
                       NET_VA_BAR, r.words[1], 1ull | 4ull) != 0) return;

    g_step = NET_STEP_FRAMES;
    if (net_frame(NET_SLOT_RING,  &g_hw.ring_phys) != 0) return;
    for (uint32_t i = 0; i < NET_RX_FRAMES; i++)
        if (net_frame(NET_SLOT_RXBUF(i), &g_hw.rx_phys[i]) != 0) return;
    if (net_frame(NET_SLOT_TXBUF, &g_hw.tx_phys)   != 0) return;

    /* Decide what the card may reach BEFORE telling it any address. */
    g_contained = (uint32_t)net_contain();

    g_step = NET_STEP_MAP_BUF;
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

    /* Everything above is the same for any card.  Everything the card's own
     * registers need is the backend's, and it is only reached once its memory
     * is retyped, mapped and contained. */
    g_step = NET_STEP_BACKEND;
    g_hw.bar_len = r.words[1];
    g_ready = (uint32_t)(g_drv && g_drv->bring_up(&g_hw));
    if (g_ready) g_step = NET_STEP_UP;
}

/* ── moving a frame ──────────────────────────────────────────────────────── */

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
            rep.words[1]   = g_hw.mac;
            rep.words[2]   = g_source_id;
            rep.words[3]   = (g_contained ? 1u : 0u) |
                             ((uint64_t)(g_seen_eth & 0xFFu) << 8) |
                             ((uint64_t)(g_step & 0xFFu) << 16) |
                             ((uint64_t)(g_hw.step & 0xFFu) << 24) |
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
            uint32_t sent = g_drv->send(&g_hw, (uint32_t)m.words[0]);
            if (sent) {
                rep.label      = NET_REP_OK;
                rep.words[0]   = sent;
                rep.word_count = 1u;
            }
        } else if (m.label == NET_OP_RECV && g_ready) {
            uint32_t off = 0;
            uint32_t len = g_drv->recv(&g_hw, &off);
            rep.label      = NET_REP_OK;
            rep.words[0]   = len;
            rep.words[1]   = g_hw.rx_count;
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
