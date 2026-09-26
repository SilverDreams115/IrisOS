/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net/e1000.c — the Intel 82540EM / 82545EM backend.
 *
 * Everything in this file is one family's register protocol.  It is handed
 * memory the service has already retyped, mapped and contained, and it never
 * touches a capability: see netdev.h for why the line is drawn there.
 *
 * The card is polled.  No interrupts are enabled, because a card raising one
 * nobody has routed is a line that stays asserted.
 */
#include <stdint.h>
#include "netdev.h"
#include <iris/syscall.h>

/* How long to wait for the card to report a transmit done.  Bounded by TIME,
 * not by a count of reads: two million register reads is a different amount of
 * patience on every machine, and an emulated card completes before the loop
 * begins where a real one behind a bridge does not. */
#define NET_TX_MS     1000u

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

#define NET_RING_LEN  8u           /* RDLEN must be a multiple of 128 bytes,
                                    * and a descriptor is 16 — so eight is the
                                    * smallest ring the hardware will take. */
#define RING_BYTES    (NET_RING_LEN * 16u)
#define RX_RING_OFF   0x000u
#define TX_RING_OFF   0x200u       /* clear of the RX ring, 16-byte aligned */

/* The BAR, mapped by the service at a fixed address. */
static volatile uint32_t *reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(NET_VA_BAR + off);
}
static uint32_t rd(uint32_t off)             { return *reg(off); }
static void     wr(uint32_t off, uint32_t v) { *reg(off) = v; }

/* Intel 82540EM / 82545EM: the e1000 this backend was written and tested
 * against.  A device outside this list may well be an e1000 too; it is not one
 * anybody has run this code on, and guessing is exactly what the service
 * refuses to do on the machine's behalf. */
static int e1000_matches(uint32_t device) {
    return device == 0x100Eu || device == 0x100Fu;
}

static int e1000_bring_up(struct net_hw *hw) {
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
        hw->mac = (uint64_t)lo | ((uint64_t)(hi & 0xFFFFu) << 32);
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
            rxd[i * 2u]      = hw->rx_phys[i / NET_RX_PER_FRAME] +
                               (uint64_t)(i % NET_RX_PER_FRAME) * NET_FRAME_BYTES;
            rxd[i * 2u + 1u] = 0u;
        }
        wr(E1000_RDBAL, (uint32_t)(hw->ring_phys + RX_RING_OFF));
        wr(E1000_RDBAH, (uint32_t)((hw->ring_phys + RX_RING_OFF) >> 32));
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
        wr(E1000_TDBAL, (uint32_t)(hw->ring_phys + TX_RING_OFF));
        wr(E1000_TDBAH, (uint32_t)((hw->ring_phys + TX_RING_OFF) >> 32));
        wr(E1000_TDLEN, RING_BYTES);
        wr(E1000_TDH, 0u);
        wr(E1000_TDT, 0u);
        wr(E1000_TIPG, 0x0060200Au);            /* the spec's IEEE 802.3 values */
        wr(E1000_TCTL, TCTL_EN | TCTL_PSP |
                       (0x10u << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
    }

    return 1;
}


static uint32_t e1000_send(struct net_hw *hw, uint32_t len) {
    if (len == 0u || len > NET_FRAME_BYTES) return 0;
    volatile uint32_t *txd = (volatile uint32_t *)(uintptr_t)(NET_VA_RING + TX_RING_OFF);
    uint32_t tail = rd(E1000_TDT) % NET_RING_LEN;

    ((volatile uint64_t *)txd)[tail * 2u] = hw->tx_phys;
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
 * `hw->rx_next` walks the ring rather than reading RDH, because the head is
 * where the card will write NEXT and says nothing about which descriptors the
 * driver has already consumed.
 */

static uint32_t e1000_recv(struct net_hw *hw, uint32_t *out_off) {
    volatile uint32_t *rxd = (volatile uint32_t *)(uintptr_t)(NET_VA_RING + RX_RING_OFF);
    uint32_t i = hw->rx_next;
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
    if (hw->rx_skipping) {
        if (status & RXD_STAT_EOP) hw->rx_skipping = 0u;
        len = 0;
    } else if (!(status & RXD_STAT_EOP)) {
        hw->rx_skipping = 1u;
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
    hw->rx_next = (i + 1u) % NET_RING_LEN;
    if (len) hw->rx_count++;
    return len;
}

const struct net_backend net_backend_e1000 = {
    .name     = "Intel e1000",
    .vendor   = 0x8086u,
    .matches  = e1000_matches,
    .bring_up = e1000_bring_up,
    .send     = e1000_send,
    .recv     = e1000_recv,
};
