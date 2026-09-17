/*
 * ip/main.c — ARP, IPv4 and UDP, above a driver that parses nothing.
 *
 * The split and what is deliberately absent are in `iris/ip_ep_proto.h`.
 * This file is the stack.
 *
 * ── How it gets frames, and why it polls ───────────────────────────────────
 *
 * `net` has no interrupt: a client asks for the oldest unread frame and gets
 * one or gets nothing.  So this service polls, and it polls only while a
 * request is outstanding — an ARP that is waiting for its reply, or a receive
 * that is waiting for a datagram.  Between requests nothing runs, which means
 * a frame that arrives while nobody is asking sits in the card's ring until
 * somebody does.
 *
 * That is a real limitation and it is the honest shape for a service with no
 * thread of its own.  The ring is eight frames deep, so what it costs is that
 * a burst longer than eight frames loses the oldest.  A stack that wanted more
 * would take an interrupt, which needs `net` to route one and a thread here to
 * wait on it.
 *
 * ── Every frame that arrives is examined, not just the one being waited for ─
 *
 * A poll that dropped everything except the frame it wanted would make this
 * host unreachable: an ARP request for our address that arrives while we are
 * waiting for a datagram has to be ANSWERED, or the peer never learns where we
 * are and never sends the datagram.  So the poll loop is a dispatcher, and the
 * thing being waited for is one of its outcomes.
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
#include <iris/net_ep_proto.h>
#include <iris/ip_ep_proto.h>

static void ip_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}

#define IP_VA_PAYLOAD 0x80D0000000ULL
#define IP_VA_NETBUF  0x80D1000000ULL

/* ── wire formats, by offset ─────────────────────────────────────────────── */
#define ETH_DST   0u
#define ETH_SRC   6u
#define ETH_TYPE  12u
#define ETH_HDR   14u
#define ETHERTYPE_IP  0x0800u
#define ETHERTYPE_ARP 0x0806u

#define ARP_HTYPE 0u
#define ARP_PTYPE 2u
#define ARP_HLEN  4u
#define ARP_PLEN  5u
#define ARP_OPER  6u
#define ARP_SHA   8u
#define ARP_SPA   14u
#define ARP_THA   18u
#define ARP_TPA   24u
#define ARP_BYTES 28u

#define IP4_VER_IHL 0u
#define IP4_TOS     1u
#define IP4_LEN     2u
#define IP4_ID      4u
#define IP4_FRAG    6u
#define IP4_TTL     8u
#define IP4_PROTO   9u
#define IP4_CSUM    10u
#define IP4_SRC     12u
#define IP4_DST     16u
#define IP4_HDR     20u
#define IP4_PROTO_UDP 17u

#define UDP_SRC   0u
#define UDP_DST   2u
#define UDP_LEN   4u
#define UDP_CSUM  6u
#define UDP_HDR   8u

/* ── state ───────────────────────────────────────────────────────────────── */
static uint32_t g_up;
static uint64_t g_mac;
static uint32_t g_addr    = IP_DEFAULT_ADDR;
static uint32_t g_mask    = IP_DEFAULT_MASK;
static uint32_t g_gateway = IP_DEFAULT_GATEWAY;
static uint64_t g_rx_datagrams;

#define ARP_CACHE 8u
static struct { uint32_t ip; uint64_t mac; uint8_t used; } g_arp[ARP_CACHE];

/*
 * What this service is currently waiting for, and what arrived.
 *
 * The KIND is separate from the port because the dispatcher sees frames it did
 * not ask for.  An ARP reply for some other host arriving mid-wait must not end
 * a wait for a datagram — it is learned and the wait continues — and without a
 * kind the only way to tell the two apart was a port number of zero, which an
 * ARP reply and a datagram addressed to port zero share.
 */
#define IP_WAIT_ARP 0u
#define IP_WAIT_UDP 1u
static uint32_t g_wait_kind;
static uint32_t g_want_port;
static uint32_t g_got_len, g_got_port;
static uint32_t g_got_addr;

/* ── byte order ──────────────────────────────────────────────────────────── */
static uint16_t rd16(const volatile uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);          /* network order */
}
static uint32_t rd32(const volatile uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void wr16(volatile uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(volatile uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/*
 * The internet checksum: ones' complement of the ones'-complement sum of
 * sixteen-bit words.  One function, because IPv4 and UDP differ only in what
 * they feed it — and a second copy is where the two would drift.
 */
static uint16_t csum16(const volatile uint8_t *p, uint32_t len, uint32_t seed) {
    uint32_t sum = seed;
    for (uint32_t i = 0; i + 1u < len; i += 2u) sum += ((uint32_t)p[i] << 8) | p[i + 1u];
    if (len & 1u) sum += (uint32_t)p[len - 1u] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── talking to the driver ───────────────────────────────────────────────── */
static long ip_net(uint64_t op, uint64_t a0, long recv, struct iris_msg *out) {
    struct iris_msg m;
    ip_msg_zero(&m);
    m.label = op; m.words[0] = a0; m.word_count = 1u;
    m.recv_slot = recv;
    long r = iris_msg_call((long)IP_SLOT_NET_EP, &m);
    if (out) *out = m;
    if (r != 0) return r;
    return (m.label == NET_REP_OK) ? 0 : -1;
}

/* Release before replacing: the unmap names a CAPABILITY, so doing it after
 * the slot has been refilled removes nothing and leaves the old mapping in the
 * way of the new one. */
static void ip_release_netbuf(void) {
    (void)iris_invoke2((long)IP_SLOT_NETBUF, INV_FRAME_UNMAP,
                       (long)IRIS_CPTR_OWN_VSPACE, (long)IP_VA_NETBUF);
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)IP_SLOT_NETBUF);
}

/* The driver's transmit buffer, mapped, with the Ethernet header already
 * filled in for `dst` and `type`.  Returns 0 on failure. */
static volatile uint8_t *ip_tx_begin(uint64_t dst_mac, uint16_t type) {
    struct iris_msg r;
    ip_release_netbuf();
    if (ip_net(NET_OP_TXBUF, 0, (long)IP_SLOT_NETBUF, &r) != 0) return 0;
    if (r.got_caps == 0u) return 0;
    if (iris_map_frame(IP_SLOT_NETBUF, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, IP_SLOT_PT,
                       IP_VA_NETBUF, 4096u, 1ull) != 0) return 0;

    volatile uint8_t *f = (volatile uint8_t *)(uintptr_t)IP_VA_NETBUF;
    for (uint32_t i = 0; i < ETH_HDR; i++) f[i] = 0;
    for (uint32_t i = 0; i < 6u; i++) f[ETH_DST + i] = (uint8_t)(dst_mac >> (8 * i));
    for (uint32_t i = 0; i < 6u; i++) f[ETH_SRC + i] = (uint8_t)(g_mac >> (8 * i));
    wr16(f + ETH_TYPE, type);
    return f;
}
static int ip_tx_end(uint32_t len) {
    struct iris_msg r;
    return ip_net(NET_OP_SEND, len, 0, &r) == 0;
}

/* ── ARP ─────────────────────────────────────────────────────────────────── */

static void arp_learn(uint32_t ip, uint64_t mac) {
    for (uint32_t i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].used && g_arp[i].ip == ip) { g_arp[i].mac = mac; return; }
    for (uint32_t i = 0; i < ARP_CACHE; i++)
        if (!g_arp[i].used) { g_arp[i].ip = ip; g_arp[i].mac = mac; g_arp[i].used = 1; return; }
    /* Full: replace the first.  A cache with no eviction is a cache that stops
     * learning, which is worse than one that forgets the wrong entry. */
    g_arp[0].ip = ip; g_arp[0].mac = mac;
}
static int arp_lookup(uint32_t ip, uint64_t *out) {
    for (uint32_t i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].used && g_arp[i].ip == ip) { *out = g_arp[i].mac; return 1; }
    return 0;
}

static int arp_send(uint16_t oper, uint32_t target_ip, uint64_t target_mac,
                    uint64_t dst_mac) {
    volatile uint8_t *f = ip_tx_begin(dst_mac, ETHERTYPE_ARP);
    if (!f) return 0;
    volatile uint8_t *a = f + ETH_HDR;
    for (uint32_t i = 0; i < ARP_BYTES; i++) a[i] = 0;
    wr16(a + ARP_HTYPE, 1u);              /* Ethernet */
    wr16(a + ARP_PTYPE, ETHERTYPE_IP);
    a[ARP_HLEN] = 6u; a[ARP_PLEN] = 4u;
    wr16(a + ARP_OPER, oper);
    for (uint32_t i = 0; i < 6u; i++) a[ARP_SHA + i] = (uint8_t)(g_mac >> (8 * i));
    wr32(a + ARP_SPA, g_addr);
    for (uint32_t i = 0; i < 6u; i++) a[ARP_THA + i] = (uint8_t)(target_mac >> (8 * i));
    wr32(a + ARP_TPA, target_ip);
    return ip_tx_end(ETH_HDR + ARP_BYTES);
}

/* ── the receive dispatcher ──────────────────────────────────────────────── */

/*
 * Take one frame from the driver, if there is one, and act on it.
 *
 * Everything is examined: an ARP request for our address is answered even when
 * this service is waiting for something else, because a host that only asks is
 * a host nobody can reach.
 *
 * Returns 1 if the thing currently being waited for arrived.
 */
static int ip_poll_once(void) {
    struct iris_msg r;
    ip_release_netbuf();
    ip_msg_zero(&r);
    {
        struct iris_msg m;
        ip_msg_zero(&m);
        m.label = NET_OP_RECV;
        m.recv_slot = (long)IP_SLOT_NETBUF;
        if (iris_msg_call((long)IP_SLOT_NET_EP, &m) != 0) return 0;
        if (m.label != NET_REP_OK || m.words[0] == 0u) return 0;
        r = m;
    }
    uint32_t len = (uint32_t)r.words[0];
    uint32_t off = (uint32_t)r.words[2];
    if (r.got_caps == 0u) return 0;
    if (iris_map_frame(IP_SLOT_NETBUF, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, IP_SLOT_PT,
                       IP_VA_NETBUF, 4096u, 0ull) != 0) return 0;

    const volatile uint8_t *f =
        (const volatile uint8_t *)(uintptr_t)(IP_VA_NETBUF + off);
    if (len < ETH_HDR) return 0;
    uint16_t type = rd16(f + ETH_TYPE);

    if (type == ETHERTYPE_ARP && len >= ETH_HDR + ARP_BYTES) {
        const volatile uint8_t *a = f + ETH_HDR;
        uint16_t oper = rd16(a + ARP_OPER);
        uint32_t spa  = rd32(a + ARP_SPA);
        uint64_t sha  = 0;
        for (uint32_t i = 0; i < 6u; i++) sha |= (uint64_t)a[ARP_SHA + i] << (8 * i);
        arp_learn(spa, sha);
        if (oper == 2u) return g_wait_kind == IP_WAIT_ARP;
        if (oper == 1u && rd32(a + ARP_TPA) == g_addr) {
            /* Somebody is looking for us.  Answering is not optional. */
            (void)arp_send(2u, spa, sha, sha);
        }
        return 0;
    }

    if (type != ETHERTYPE_IP || len < ETH_HDR + IP4_HDR) return 0;
    const volatile uint8_t *ip4 = f + ETH_HDR;
    if ((ip4[IP4_VER_IHL] >> 4) != 4u) return 0;
    uint32_t ihl = (uint32_t)(ip4[IP4_VER_IHL] & 0xFu) * 4u;
    if (ihl < IP4_HDR || len < ETH_HDR + ihl) return 0;
    if (rd32(ip4 + IP4_DST) != g_addr) return 0;
    if (ip4[IP4_PROTO] != IP4_PROTO_UDP) return 0;
    /* A fragment is not a datagram.  Dropping it is honest; reassembling it is
     * a feature this stack says it does not have. */
    if (rd16(ip4 + IP4_FRAG) & 0x3FFFu) return 0;

    const volatile uint8_t *udp = ip4 + ihl;
    if (len < ETH_HDR + ihl + UDP_HDR) return 0;
    uint16_t ulen = rd16(udp + UDP_LEN);
    if (ulen < UDP_HDR || ETH_HDR + ihl + ulen > len) return 0;
    uint32_t plen = (uint32_t)ulen - UDP_HDR;
    if (plen > IP_MAX_PAYLOAD) return 0;

    if (g_wait_kind != IP_WAIT_UDP) return 0;
    if (rd16(udp + UDP_DST) != (uint16_t)g_want_port) return 0;

    /* Ours.  Copy the payload out of the driver's buffer before the next
     * receive takes that capability away. */
    {
        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)IP_VA_PAYLOAD;
        const volatile uint8_t *src = udp + UDP_HDR;
        for (uint32_t i = 0; i < plen; i++) dst[i] = src[i];
    }
    g_got_len  = plen;
    g_got_addr = rd32(ip4 + IP4_SRC);
    g_got_port = rd16(udp + UDP_SRC);
    g_rx_datagrams++;
    return 1;
}

/* Poll until the thing being waited for arrives or the time runs out. */
static int ip_wait(uint32_t ms) {
    long t0 = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
    for (;;) {
        if (ip_poll_once()) return 1;
        long now = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
        if (t0 > 0 && now > 0 &&
            (uint64_t)(now - t0) > (uint64_t)ms * 1000000ull) return 0;
        (void)iris_syscall4(SYS_YIELD, 0, 0, 0, 0);
    }
}

/*
 * The MAC to send to, for an address.
 *
 * One routing decision: inside our subnet, ask for the address itself;
 * outside it, the gateway answers for everything.  That is the whole of the
 * routing table and it is enough for a machine with one interface.
 */
static int ip_route(uint32_t addr, uint64_t *out_mac) {
    uint32_t next = ((addr & g_mask) == (g_addr & g_mask)) ? addr : g_gateway;
    if (arp_lookup(next, out_mac)) return 1;
    /*
     * Asking changes what a reply MEANS to the dispatcher, and a route is also
     * resolved in the middle of a send that a datagram wait is waiting on — so
     * the previous kind is restored rather than cleared.
     */
    uint32_t saved_kind = g_wait_kind;
    uint32_t saved_port = g_want_port;
    g_wait_kind = IP_WAIT_ARP;
    for (uint32_t attempt = 0; attempt < 3u; attempt++) {
        if (!arp_send(1u, next, 0, 0xFFFFFFFFFFFFull)) break;
        (void)ip_wait(500u);
        if (arp_lookup(next, out_mac)) {
            g_wait_kind = saved_kind; g_want_port = saved_port;
            return 1;
        }
    }
    g_wait_kind = saved_kind; g_want_port = saved_port;
    return 0;
}

/* ── UDP ─────────────────────────────────────────────────────────────────── */

static int udp_send(uint32_t dst_addr, uint16_t dst_port, uint16_t src_port,
                    uint32_t plen) {
    if (plen > IP_MAX_PAYLOAD) return 0;
    uint64_t mac;
    if (!ip_route(dst_addr, &mac)) return 0;

    volatile uint8_t *f = ip_tx_begin(mac, ETHERTYPE_IP);
    if (!f) return 0;
    volatile uint8_t *ip4 = f + ETH_HDR;
    volatile uint8_t *udp = ip4 + IP4_HDR;

    for (uint32_t i = 0; i < IP4_HDR + UDP_HDR; i++) ip4[i] = 0;
    ip4[IP4_VER_IHL] = 0x45u;                        /* IPv4, 20-byte header */
    wr16(ip4 + IP4_LEN, (uint16_t)(IP4_HDR + UDP_HDR + plen));
    ip4[IP4_TTL]   = 64u;
    ip4[IP4_PROTO] = IP4_PROTO_UDP;
    wr32(ip4 + IP4_SRC, g_addr);
    wr32(ip4 + IP4_DST, dst_addr);
    wr16(ip4 + IP4_CSUM, csum16(ip4, IP4_HDR, 0));

    wr16(udp + UDP_SRC, src_port);
    wr16(udp + UDP_DST, dst_port);
    wr16(udp + UDP_LEN, (uint16_t)(UDP_HDR + plen));

    /* The payload, copied from the buffer the client wrote into. */
    {
        const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)IP_VA_PAYLOAD;
        volatile uint8_t *dst = udp + UDP_HDR;
        for (uint32_t i = 0; i < plen; i++) dst[i] = src[i];
    }

    /*
     * UDP's checksum covers a PSEUDO-HEADER as well as the datagram: the
     * addresses and the protocol from the IP header, which is what stops a
     * datagram delivered to the wrong host passing its own checksum.  A
     * computed value of zero is transmitted as all-ones, because zero means
     * "not computed" and the two must not be confusable.
     */
    {
        uint32_t seed = (g_addr >> 16) + (g_addr & 0xFFFFu) +
                        (dst_addr >> 16) + (dst_addr & 0xFFFFu) +
                        IP4_PROTO_UDP + (uint32_t)(UDP_HDR + plen);
        uint16_t c = csum16(udp, UDP_HDR + plen, seed);
        wr16(udp + UDP_CSUM, c ? c : 0xFFFFu);
    }

    return ip_tx_end(ETH_HDR + IP4_HDR + UDP_HDR + plen);
}

/* ── the service loop ────────────────────────────────────────────────────── */

void ip_main(iris_cptr_t bootstrap_ch_h);
void ip_main(iris_cptr_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)IP_SLOT_PAYLOAD << 32), 4096) == 0)
        (void)iris_map_frame(IP_SLOT_PAYLOAD, IRIS_CPTR_OWN_VSPACE,
                             IRIS_CPTR_OWN_UNTYPED, IP_SLOT_PT,
                             IP_VA_PAYLOAD, 4096u, 1ull);

    {
        struct iris_msg r;
        if (ip_net(NET_OP_INFO, 0, 0, &r) == 0 && (r.words[0] & 1u)) {
            g_mac = r.words[1];
            g_up  = 1u;
        }
    }

    for (;;) {
        struct iris_msg m;
        ip_msg_zero(&m);
        m.reply = (long)IP_SLOT_REPLY;
        if (iris_msg_recv((long)IP_SLOT_CTRL_EP, &m) != 0) continue;

        struct iris_msg rep;
        ip_msg_zero(&rep);
        rep.label = IP_REP_ERR;

        if (m.label == IP_OP_INFO) {
            rep.label      = IP_REP_OK;
            rep.words[0]   = g_up;
            rep.words[1]   = g_addr;
            rep.words[2]   = g_mac;
            rep.words[3]   = g_rx_datagrams;
            rep.word_count = 4u;
        } else if (m.label == IP_OP_ARP && g_up) {
            uint64_t mac;
            if (ip_route((uint32_t)m.words[0], &mac)) {
                rep.label      = IP_REP_OK;
                rep.words[0]   = mac;
                rep.word_count = 1u;
            }
        } else if (m.label == IP_OP_BUF && g_up) {
            rep.label      = IP_REP_OK;
            rep.words[0]   = IP_MAX_PAYLOAD;
            rep.word_count = 1u;
            (void)iris_invoke0((long)IP_SLOT_PAYLOAD, INV_CSPACE_REVOKE);
            rep.cap        = (long)IP_SLOT_PAYLOAD;
            rep.cap_rights = RIGHT_READ | RIGHT_WRITE;
        } else if (m.label == IP_OP_UDP_SEND && g_up) {
            uint16_t dport = (uint16_t)(m.words[1] & 0xFFFFu);
            uint16_t sport = (uint16_t)((m.words[1] >> 16) & 0xFFFFu);
            if (udp_send((uint32_t)m.words[0], dport, sport,
                         (uint32_t)m.words[2])) {
                rep.label      = IP_REP_OK;
                rep.words[0]   = m.words[2];
                rep.word_count = 1u;
            }
        } else if (m.label == IP_OP_UDP_RECV && g_up) {
            g_want_port = (uint32_t)m.words[0];
            g_wait_kind = IP_WAIT_UDP;
            g_got_len = 0;
            (void)ip_wait((uint32_t)m.words[1]);
            rep.label      = IP_REP_OK;
            rep.words[0]   = g_got_len;
            rep.words[1]   = g_got_addr;
            rep.words[2]   = g_got_port;
            rep.word_count = 3u;
            if (g_got_len) {
                (void)iris_invoke0((long)IP_SLOT_PAYLOAD, INV_CSPACE_REVOKE);
                rep.cap        = (long)IP_SLOT_PAYLOAD;
                rep.cap_rights = RIGHT_READ;
            }
            g_want_port = 0;
            g_wait_kind = IP_WAIT_ARP;
        }

        (void)iris_msg_reply((long)IP_SLOT_REPLY, &rep);
    }
}
