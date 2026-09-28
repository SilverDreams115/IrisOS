/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net/virtio.c — virtio-net, modern (VIRTIO 1.0) transport.
 *
 * One backend that covers QEMU, KVM, VirtualBox told to present one, and
 * essentially every cloud machine.  That breadth is the reason it was written
 * before any second piece of real silicon: it is the only Ethernet device
 * this tree can exercise on every commit without owning the card.
 *
 * ── What makes it different from the e1000 ─────────────────
 *
 * Every other card in this directory has its registers at fixed offsets from
 * a fixed BAR, and a driver that knows the family knows the addresses.  A
 * virtio device DESCRIBES ITSELF: the PCI capability list says which BAR each
 * of its structures is in and at what offset, and two machines presenting the
 * same device can answer differently.  So this file reads config space before
 * anything is claimed — which is what `probe` in netdev.h exists for, and why
 * `PCI_OP_CFG_READ` exists at all.
 *
 * Modern only.  A transitional device (0x1000) also carries the modern
 * capability structures, so it is driven through those and its legacy I/O
 * BAR is never touched.  A device presenting NEITHER is refused rather than
 * half-driven: `probe` returns -1 and the service reports an Ethernet
 * controller nobody speaks to, which is the truth.
 *
 * Polled, like every other backend here: no interrupt is enabled, because a
 * device raising one nobody has routed is a line that stays asserted.
 */
#include <stdint.h>
#include "netdev.h"
#include <iris/syscall.h>
#include <iris/pci_ep_proto.h>

#define VIRTIO_VENDOR      0x1AF4u
#define VIRTIO_NET_XITION  0x1000u   /* transitional: modern caps, legacy id */
#define VIRTIO_NET_MODERN  0x1041u   /* 0x1040 + the network device type     */

/* virtio_pci_cap.cfg_type — which structure this capability points at. */
#define VIRTIO_CAP_COMMON  1u
#define VIRTIO_CAP_NOTIFY  2u
#define VIRTIO_CAP_DEVICE  4u

/* virtio_pci_common_cfg, offsets from the start of that structure. */
#define VCOM_DFEAT_SEL     0x00u
#define VCOM_DFEAT         0x04u
#define VCOM_GFEAT_SEL     0x08u
#define VCOM_GFEAT         0x0Cu
#define VCOM_MSIX_CFG      0x10u
#define VCOM_NUM_QUEUES    0x12u
#define VCOM_STATUS        0x14u
#define VCOM_Q_SELECT      0x16u
#define VCOM_Q_SIZE        0x18u
#define VCOM_Q_MSIX        0x1Au
#define VCOM_Q_ENABLE      0x1Cu
#define VCOM_Q_NOTIFY_OFF  0x1Eu
#define VCOM_Q_DESC        0x20u
#define VCOM_Q_DRIVER      0x28u
#define VCOM_Q_DEVICE      0x30u
#define VCOM_BYTES         0x38u

#define VSTAT_ACK          1u
#define VSTAT_DRIVER       2u
#define VSTAT_DRIVER_OK    4u
#define VSTAT_FEATURES_OK  8u
#define VSTAT_FAILED       0x80u

#define VF_NET_MAC         (1u << 5)    /* feature bit 5, in the low word  */
#define VF_VERSION_1       (1u << 0)    /* feature bit 32, in the high one */

#define VRING_NEXT         1u
#define VRING_WRITE        2u           /* the DEVICE writes this buffer   */

#define VNET_HDR_BYTES     12u          /* with VERSION_1, always twelve   */
#define MSIX_NO_VECTOR     0xFFFFu

/*
 * The rings, laid out by hand in the one page the service retyped.
 *
 * Sized for sixteen descriptors whatever the device settles on, so the
 * offsets below are constants: a device that hands back a shorter queue
 * simply leaves the tail of each ring unused, and nothing has to move.
 * Eight buffers of two descriptors each is what sixteen buys, which is
 * exactly the eight receive slots the service provides.
 */
#define VQ_LEN         16u
#define RXQ_DESC       0x0000u   /* 16 * 16 = 256 bytes */
#define RXQ_AVAIL      0x0100u   /* 6 + 2*16 =  38      */
#define RXQ_USED       0x0140u   /* 6 + 8*16 = 134      */
#define TXQ_DESC       0x0240u
#define TXQ_AVAIL      0x0340u
#define TXQ_USED       0x0380u
#define RX_HDR         0x0480u   /* 16 * 12 = 192       */
#define TX_HDR         0x0540u   /* 12, ending at 0x54C */

#define NET_RX_SLOTS   (NET_RX_FRAMES * NET_RX_PER_FRAME)

/* How long a transmit may take before the device is called unresponsive.
 * The same budget, and the same reasoning, as the e1000 backend. */
#define NET_TX_MS      1000u

/*
 * Where bring-up stopped, in `hw->step`.  Reported verbatim through
 * NET_OP_INFO: on a machine whose virtio device this driver cannot drive,
 * "it refused at the capability walk" and "it refused at feature
 * negotiation" are different problems and only one of them is this file's.
 *   1 no capability list      2 no common/notify capability
 *   3 a structure outside the BAR   4 probe accepted the device
 *   5 the device would not reset    6 it does not offer VERSION_1
 *   7 it rejected our features      8 no receive queue
 *   9 no transmit queue            10 the queue was too short to post into
 *  11 it refused DRIVER_OK
 */
struct virtio_priv {
    uint32_t bar;          /* the one BAR every structure turned out to be in */
    uint32_t common;       /* byte offsets into it */
    uint32_t notify;
    uint32_t notify_mul;
    uint32_t device;
    uint32_t qsize;        /* what the device agreed to */
    uint32_t rx_bufs;      /* chains posted, qsize/2 capped at the slots */
    uint32_t rx_notify;    /* the two doorbells, already resolved to offsets */
    uint32_t tx_notify;
    uint16_t rx_used;      /* how far we have consumed each used ring */
    uint16_t tx_used;
    uint16_t tx_avail;     /* our own idx, because the ring's is device-read */
};
_Static_assert(sizeof(struct virtio_priv) <= sizeof(((struct net_hw *)0)->priv),
               "virtio state does not fit in the backend's private words");

/* ── the mapped BAR ──────────────────────────────────────────────────────── */

static volatile uint8_t *bar_at(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(NET_VA_BAR + off);
}
static uint8_t  cr8 (struct virtio_priv *p, uint32_t o) {
    return *(volatile uint8_t  *)bar_at(p->common + o); }
static uint16_t cr16(struct virtio_priv *p, uint32_t o) {
    return *(volatile uint16_t *)bar_at(p->common + o); }
static uint32_t cr32(struct virtio_priv *p, uint32_t o) {
    return *(volatile uint32_t *)bar_at(p->common + o); }
static void cw8 (struct virtio_priv *p, uint32_t o, uint8_t v) {
    *(volatile uint8_t  *)bar_at(p->common + o) = v; }
static void cw16(struct virtio_priv *p, uint32_t o, uint16_t v) {
    *(volatile uint16_t *)bar_at(p->common + o) = v; }
static void cw32(struct virtio_priv *p, uint32_t o, uint32_t v) {
    *(volatile uint32_t *)bar_at(p->common + o) = v; }
static void cw64(struct virtio_priv *p, uint32_t o, uint64_t v) {
    /* Two dwords, low first.  The structure is naturally aligned and a single
     * 64-bit store would be legal, but the low-then-high order is what the
     * specification writes and what every device is tested against. */
    cw32(p, o,      (uint32_t)v);
    cw32(p, o + 4u, (uint32_t)(v >> 32));
}

/*
 * The device reads memory this driver wrote, and writes memory this driver is
 * about to read.  A compiler free to sink the descriptor stores past the
 * doorbell would hand the device a ring it has not finished filling, and the
 * fence makes the stores visible before the write that tells it to look.
 */
static void vfence(void) { __asm__ volatile ("mfence" ::: "memory"); }

static void notify(uint32_t off, uint16_t q) {
    *(volatile uint16_t *)bar_at(off) = q;
}

/* ── the rings, as they sit in the page ──────────────────────────────────── */

static volatile uint8_t *ring_at(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(NET_VA_RING + off);
}

/* One descriptor: addr, len, flags, next. */
static void desc_set(uint32_t base, uint32_t i, uint64_t addr,
                     uint32_t len, uint16_t flags, uint16_t next) {
    volatile uint8_t *d = ring_at(base + i * 16u);
    *(volatile uint64_t *)(d)        = addr;
    *(volatile uint32_t *)(d + 8u)   = len;
    *(volatile uint16_t *)(d + 12u)  = flags;
    *(volatile uint16_t *)(d + 14u)  = next;
}
static uint16_t avail_idx(uint32_t base) {
    return *(volatile uint16_t *)ring_at(base + 2u); }
static void avail_set(uint32_t base, uint16_t slot, uint16_t desc) {
    *(volatile uint16_t *)ring_at(base + 4u + (slot % VQ_LEN) * 2u) = desc; }
static void avail_publish(uint32_t base, uint16_t idx) {
    vfence();
    *(volatile uint16_t *)ring_at(base + 2u) = idx;
}
static uint16_t used_idx(uint32_t base) {
    return *(volatile uint16_t *)ring_at(base + 2u); }
static void used_entry(uint32_t base, uint16_t slot,
                       uint32_t *id, uint32_t *len) {
    volatile uint8_t *e = ring_at(base + 4u + (slot % VQ_LEN) * 8u);
    *id  = *(volatile uint32_t *)(e);
    *len = *(volatile uint32_t *)(e + 4u);
}

/* ── finding the device's own description of itself ──────────────────────── */

static int virtio_matches(uint32_t device) {
    return device == VIRTIO_NET_XITION || device == VIRTIO_NET_MODERN;
}

/*
 * Walk the capability list and record where the three structures this driver
 * needs actually are.  Refuses anything it cannot drive rather than guessing:
 * a missing common configuration means the device is legacy-only, and a
 * structure in a DIFFERENT BAR from the others means one mapping cannot reach
 * them all — the service maps one window, and a backend that pretended
 * otherwise would read whatever happened to be at that offset.
 */
static int virtio_probe(uint32_t index, struct net_hw *hw) {
    struct virtio_priv *p = (struct virtio_priv *)hw->priv;
    for (uint32_t i = 0; i < sizeof(hw->priv) / sizeof(hw->priv[0]); i++)
        hw->priv[i] = 0;

    hw->step = 1u;
    uint32_t status = net_bus_cfg(index, PCI_CFG_COMMAND);
    if (status == 0xFFFFFFFFu || !(status & PCI_CFG_STATUS_CAPS)) return -1;

    uint32_t off = net_bus_cfg(index, PCI_CFG_CAP_PTR) & 0xFCu;
    int have_common = 0, have_notify = 0, bar_known = 0;

    /* Bounded: a malformed list can point at itself, and 256 bytes of config
     * space holds no more than sixty-four dword-aligned entries. */
    for (uint32_t step = 0; step < 64u && off >= 0x40u && off < 0x100u; step++) {
        uint32_t w0 = net_bus_cfg(index, off);
        if (w0 == 0xFFFFFFFFu) break;
        uint32_t id   = w0 & 0xFFu;
        uint32_t next = (w0 >> 8) & 0xFCu;

        if (id == PCI_CAP_ID_VENDOR) {
            uint32_t type = (w0 >> 24) & 0xFFu;
            uint32_t w1   = net_bus_cfg(index, off + 4u);
            uint32_t cbar = w1 & 0xFFu;
            uint32_t coff = net_bus_cfg(index, off + 8u);

            if (type == VIRTIO_CAP_COMMON || type == VIRTIO_CAP_NOTIFY ||
                type == VIRTIO_CAP_DEVICE) {
                if (cbar >= 6u) return -1;
                if (!bar_known) { p->bar = cbar; bar_known = 1; }
                else if (cbar != p->bar) return -1;
            }
            if (type == VIRTIO_CAP_COMMON) { p->common = coff; have_common = 1; }
            else if (type == VIRTIO_CAP_NOTIFY) {
                p->notify     = coff;
                p->notify_mul = net_bus_cfg(index, off + 16u);
                have_notify   = 1;
            } else if (type == VIRTIO_CAP_DEVICE) {
                p->device = coff;
            }
        }
        if (next == 0u || next == off) break;
        off = next;
    }

    hw->step = 2u;
    if (!have_common || !have_notify) return -1;

    /*
     * The structures have to be inside the window the service will map, and
     * it maps the whole BAR.  Checked here, where the numbers came from,
     * rather than trusted at the first register read.
     */
    hw->step = 3u;
    uint64_t size = 0;
    uint32_t flags = 0;
    if (!net_bus_bar(index, p->bar, 0, &size, &flags)) return -1;
    if (!(flags & PCI_BAR_MMIO)) return -1;
    if ((uint64_t)p->common + VCOM_BYTES > size) return -1;
    if ((uint64_t)p->notify + 2u > size) return -1;
    if (p->device && (uint64_t)p->device + 8u > size) return -1;

    hw->step = 4u;
    return (int)p->bar;
}

/* ── bring-up ────────────────────────────────────────────────────────────── */

/*
 * Point one virtqueue at memory and turn it on.  Returns the size the device
 * agreed to, or 0 — including for a queue the device does not have, which is
 * how a virtio device with no receive queue is refused instead of driven.
 */
static uint32_t vq_setup(struct virtio_priv *p, uint16_t q, uint64_t ring_phys,
                         uint32_t desc, uint32_t avail, uint32_t used,
                         uint32_t *out_notify) {
    cw16(p, VCOM_Q_SELECT, q);
    uint32_t n = cr16(p, VCOM_Q_SIZE);
    if (n == 0u) return 0u;
    if (n > VQ_LEN) {
        /* Legal and expected: the size a device reports on reset is its
         * MAXIMUM, and a driver reduces it to what it has memory for. */
        cw16(p, VCOM_Q_SIZE, (uint16_t)VQ_LEN);
        n = cr16(p, VCOM_Q_SIZE);
        if (n > VQ_LEN) return 0u;
    }
    if (n & (n - 1u)) return 0u;               /* must be a power of two */

    cw16(p, VCOM_Q_MSIX, MSIX_NO_VECTOR);      /* polled; no vector wanted */
    cw64(p, VCOM_Q_DESC,   ring_phys + desc);
    cw64(p, VCOM_Q_DRIVER, ring_phys + avail);
    cw64(p, VCOM_Q_DEVICE, ring_phys + used);
    *out_notify = p->notify + (uint32_t)cr16(p, VCOM_Q_NOTIFY_OFF) * p->notify_mul;
    cw16(p, VCOM_Q_ENABLE, 1u);
    return n;
}

static int virtio_bring_up(struct net_hw *hw) {
    struct virtio_priv *p = (struct virtio_priv *)hw->priv;

    /* Reset, and wait for the device to agree that it is reset. */
    hw->step = 5u;
    cw8(p, VCOM_STATUS, 0u);
    for (uint32_t i = 0; i < 1000000u; i++) if (cr8(p, VCOM_STATUS) == 0u) break;
    if (cr8(p, VCOM_STATUS) != 0u) return 0;

    cw8(p, VCOM_STATUS, VSTAT_ACK);
    cw8(p, VCOM_STATUS, VSTAT_ACK | VSTAT_DRIVER);

    /*
     * VERSION_1 is not optional here.  A device that does not offer it is a
     * legacy device wearing modern capabilities, and the ring layout below —
     * and the twelve-byte header — are the modern ones.
     */
    cw32(p, VCOM_DFEAT_SEL, 1u);
    uint32_t feat_hi = cr32(p, VCOM_DFEAT);
    hw->step = 6u;
    if (!(feat_hi & VF_VERSION_1)) { cw8(p, VCOM_STATUS, VSTAT_FAILED); return 0; }
    cw32(p, VCOM_DFEAT_SEL, 0u);
    uint32_t feat_lo = cr32(p, VCOM_DFEAT);

    /* Nothing is accepted that is not needed.  In particular MRG_RXBUF is
     * refused, which is what makes a received frame exactly one buffer. */
    uint32_t take_lo = feat_lo & VF_NET_MAC;
    cw32(p, VCOM_GFEAT_SEL, 0u); cw32(p, VCOM_GFEAT, take_lo);
    cw32(p, VCOM_GFEAT_SEL, 1u); cw32(p, VCOM_GFEAT, VF_VERSION_1);

    cw8(p, VCOM_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    hw->step = 7u;
    if (!(cr8(p, VCOM_STATUS) & VSTAT_FEATURES_OK)) {
        cw8(p, VCOM_STATUS, VSTAT_FAILED); return 0;
    }

    /* Queue 0 receives, queue 1 transmits: fixed by the device type. */
    uint32_t nrx = vq_setup(p, 0u, hw->ring_phys, RXQ_DESC, RXQ_AVAIL, RXQ_USED,
                            &p->rx_notify);
    uint32_t ntx = vq_setup(p, 1u, hw->ring_phys, TXQ_DESC, TXQ_AVAIL, TXQ_USED,
                            &p->tx_notify);
    hw->step = (nrx == 0u) ? 8u : 9u;
    if (nrx == 0u || ntx == 0u) { cw8(p, VCOM_STATUS, VSTAT_FAILED); return 0; }
    p->qsize = nrx < ntx ? nrx : ntx;

    /*
     * Two descriptors per received frame: the device's twelve-byte header
     * into the ring page, the Ethernet frame into a receive buffer.
     *
     * One descriptor would be simpler and would put the header at the front
     * of the buffer — where the client, which is handed that buffer and told
     * an offset, would find twelve bytes of virtio in the middle of what it
     * was told is a frame.  Splitting keeps the receive pages holding nothing
     * but Ethernet, so the contract in net_ep_proto.h means the same thing
     * whichever backend filled them.
     */
    p->rx_bufs = p->qsize / 2u;
    if (p->rx_bufs > NET_RX_SLOTS) p->rx_bufs = NET_RX_SLOTS;
    hw->step = 10u;
    if (p->rx_bufs == 0u) { cw8(p, VCOM_STATUS, VSTAT_FAILED); return 0; }

    for (uint32_t i = 0; i < p->rx_bufs; i++) {
        desc_set(RXQ_DESC, i * 2u,
                 hw->ring_phys + RX_HDR + (uint64_t)i * VNET_HDR_BYTES,
                 VNET_HDR_BYTES, (uint16_t)(VRING_NEXT | VRING_WRITE),
                 (uint16_t)(i * 2u + 1u));
        desc_set(RXQ_DESC, i * 2u + 1u,
                 hw->rx_phys[i / NET_RX_PER_FRAME] +
                     (uint64_t)(i % NET_RX_PER_FRAME) * NET_FRAME_BYTES,
                 NET_FRAME_BYTES, (uint16_t)VRING_WRITE, 0u);
        avail_set(RXQ_AVAIL, (uint16_t)i, (uint16_t)(i * 2u));
    }
    avail_publish(RXQ_AVAIL, (uint16_t)p->rx_bufs);

    /* The transmit chain never changes shape: header, then the one buffer. */
    desc_set(TXQ_DESC, 0u, hw->ring_phys + TX_HDR, VNET_HDR_BYTES,
             (uint16_t)VRING_NEXT, 1u);

    cw8(p, VCOM_STATUS,
        VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);
    hw->step = 11u;
    if (!(cr8(p, VCOM_STATUS) & VSTAT_DRIVER_OK)) return 0;

    /* The device is live now, so the receive queue is offered only here. */
    vfence();
    notify(p->rx_notify, 0u);

    /* The MAC, if the device agreed to tell us.  Six bytes, low byte first,
     * packed the way the endpoint reports it. */
    if (take_lo & VF_NET_MAC) {
        volatile uint8_t *dc = bar_at(p->device);
        uint64_t mac = 0;
        for (uint32_t i = 0; i < 6u; i++) mac |= (uint64_t)dc[i] << (8u * i);
        hw->mac = mac;
    }
    hw->step = 12u;
    return 1;
}

/* ── moving a frame ──────────────────────────────────────────────────────── */

static uint32_t virtio_send(struct net_hw *hw, uint32_t len) {
    struct virtio_priv *p = (struct virtio_priv *)hw->priv;
    if (len == 0u || len > NET_FRAME_BYTES) return 0;

    /* A zeroed header: no checksum offload, no segmentation, one buffer. */
    volatile uint8_t *h = ring_at(TX_HDR);
    for (uint32_t i = 0; i < VNET_HDR_BYTES; i++) h[i] = 0u;

    desc_set(TXQ_DESC, 1u, hw->tx_phys, len, 0u, 0u);
    avail_set(TXQ_AVAIL, p->tx_avail, 0u);
    p->tx_avail++;
    avail_publish(TXQ_AVAIL, p->tx_avail);
    vfence();
    notify(p->tx_notify, 1u);

    /*
     * Bounded by TIME and not by a count of reads, for the reason the e1000
     * backend gives at length: a count is a different amount of patience on
     * every machine.  A clock that will not answer leaves this unbounded
     * rather than instantaneous — the failure being guarded is a device that
     * never replies, and a missing clock is not that.
     */
    long t0 = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
    for (;;) {
        if (used_idx(TXQ_USED) != p->tx_used) { p->tx_used++; return len; }
        long now = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
        if (t0 > 0 && now > 0 &&
            (uint64_t)(now - t0) > (uint64_t)NET_TX_MS * 1000000ull) break;
    }
    return 0;
}

static uint32_t virtio_recv(struct net_hw *hw, uint32_t *out_off) {
    struct virtio_priv *p = (struct virtio_priv *)hw->priv;
    if (used_idx(RXQ_USED) == p->rx_used) return 0;

    uint32_t id = 0, total = 0;
    used_entry(RXQ_USED, p->rx_used, &id, &total);
    p->rx_used++;

    /*
     * `id` is the head of the chain this driver built, so it names the buffer
     * — but it arrived from the device, and a device that returned a head
     * outside the ring would have this indexing into whatever follows it.
     */
    uint32_t i = (id / 2u);
    /* `total` counts the header the device wrote as well as the frame. */
    uint32_t len = total > VNET_HDR_BYTES ? total - VNET_HDR_BYTES : 0u;
    if (len > NET_FRAME_BYTES) len = 0u;
    if (id >= p->rx_bufs * 2u || (id & 1u) != 0u) {
        /* Clamped AND emptied, not just clamped: a head the driver never
         * offered names no buffer, and handing up buffer zero's contents
         * would pass off whatever was last received there as a new frame.
         * The chain is still re-offered below, so a device that does this
         * once does not permanently cost a receive slot. */
        i = 0u; len = 0u;
    }

    *out_off = (i / NET_RX_PER_FRAME) * 4096u +
               (i % NET_RX_PER_FRAME) * NET_FRAME_BYTES;

    /* Offer the same chain back.  Its descriptors still describe the same
     * memory, so only the available ring has to move. */
    uint16_t a = avail_idx(RXQ_AVAIL);
    avail_set(RXQ_AVAIL, a, (uint16_t)(i * 2u));
    avail_publish(RXQ_AVAIL, (uint16_t)(a + 1u));
    vfence();
    notify(p->rx_notify, 0u);

    if (len) hw->rx_count++;
    return len;
}

const struct net_backend net_backend_virtio = {
    .name     = "virtio-net",
    .vendor   = VIRTIO_VENDOR,
    .matches  = virtio_matches,
    .probe    = virtio_probe,
    .bring_up = virtio_bring_up,
    .send     = virtio_send,
    .recv     = virtio_recv,
};
