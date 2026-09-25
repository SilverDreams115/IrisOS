/*
 * pci/main.c — the PCI bus, as a service (Stage 10).
 *
 * The design and the reason for it are in `iris/pci_ep_proto.h`.  In one line:
 * one task holds the configuration ports and the PCI-hole device Untyped, so
 * that a driver can be given its own device's registers without being given
 * everybody's.
 *
 * ── What this service does at startup, and why all of it is at startup ─────
 *
 * It walks bus 0, measures every memory BAR, and carves a frame over each
 * window that falls inside the region it owns.  Then it answers questions.
 *
 * The carving cannot be lazy, and that is a property of an Untyped rather than
 * a choice: an Untyped hands out its region IN ORDER and never goes back, so
 * frames over windows at 0x81000000 and 0x81110000 can only both exist if the
 * lower one is taken first.  A service that carved on demand would satisfy the
 * first driver to ask and then be unable to satisfy a driver whose device sits
 * below it — a failure that depends on the order clients happen to start in,
 * which is the worst kind.
 *
 * Doing it at startup also means the machine is DESCRIBED before anything acts
 * on it: PCI_OP_COUNT is a complete answer, not a snapshot of what has been
 * looked at so far.
 */
#include <stdint.h>
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/pci_ep_proto.h>

static void pci_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}

/* ── configuration space, through the one capability that reaches it ─────── */

/*
 * 0xCF8 takes the address and 0xCFC gives the data, and the address register
 * IGNORES anything narrower than a dword — which is why INV_IOPORT_OUT32
 * exists (Stage 10-dma §10.2 step 6 found that the port ABI had no width above
 * a byte and therefore could not host a PCI driver at all).
 */
/*
 * Configuration space, addressed by BDF: bus in the high eight bits, device
 * and function in the low eight.  That is the same sixteen-bit shape
 * `struct pci_fn` already stored, so a caller holding one of those needs no
 * conversion -- which is why the bus could be threaded through here without
 * touching the BAR measurement at all.
 */
static uint32_t cfg_read(uint32_t bdf, uint32_t off) {
    uint32_t addr = 0x80000000u | ((bdf >> 8) << 16) |
                    ((bdf & 0xFFu) << 8) | (off & 0xFCu);
    (void)iris_invoke2((long)PCI_SLOT_IOPORT, INV_IOPORT_OUT32, 0, (long)addr);
    return (uint32_t)iris_invoke1((long)PCI_SLOT_IOPORT, INV_IOPORT_IN32, 4);
}
static void cfg_write(uint32_t bdf, uint32_t off, uint32_t v) {
    uint32_t addr = 0x80000000u | ((bdf >> 8) << 16) |
                    ((bdf & 0xFFu) << 8) | (off & 0xFCu);
    (void)iris_invoke2((long)PCI_SLOT_IOPORT, INV_IOPORT_OUT32, 0, (long)addr);
    (void)iris_invoke2((long)PCI_SLOT_IOPORT, INV_IOPORT_OUT32, 4, (long)v);
}

#define CFG_VENDOR   0x00u
#define CFG_COMMAND  0x04u
#define CFG_CLASS    0x08u
#define CFG_HEADER   0x0Cu   /* byte 2 = header type; bit 7 = multifunction */
#define CFG_BAR(n)   (0x10u + 4u * (n))
#define PCI_BAR_COUNT 6u

/* ── what the scan found ─────────────────────────────────────────────────── */

struct pci_fn {
    uint32_t vendor_device;
    uint32_t class_code;
    uint16_t devfn;
    uint8_t  bar_mmio;      /* bitmask: this BAR decodes memory */
    uint8_t  _pad;
    uint64_t bar_base[PCI_BAR_COUNT];
    uint64_t bar_size[PCI_BAR_COUNT];
    uint8_t  bar_flags[PCI_BAR_COUNT];
    int8_t   bar_window[PCI_BAR_COUNT];  /* index into g_window, or -1 */
};

static struct pci_fn g_fn[PCI_MAX_FUNCTIONS];
static uint32_t      g_fn_count;
/* Set when the table filled and the walk stopped early.  Without it
 * "functions 96" reads like a census when it is a ceiling -- the same shape
 * that made a sixteen-thread machine report eight processors. */
static uint32_t      g_fn_truncated;

/* A window the service carved a frame over. */
struct pci_window {
    uint64_t base;
    uint64_t size;
    uint32_t leaf;          /* PCI_SLOT_BAR_BASE + i, in this service's CSpace */
};
static struct pci_window g_window[PCI_MAX_WINDOWS];
static uint32_t          g_window_count;

/*
 * WHY the carve stopped, reported with the counts.
 *
 * Not debug residue: a bus service that found devices and carved nothing will
 * refuse every claim, and from the outside that is indistinguishable from an
 * empty machine.  This is the one number that tells the two apart, and it
 * earned its place — the first working version of this service reported six
 * functions and zero windows for exactly one reason, and finding it took a
 * round trip through the boot log because there was nothing to ask.
 */
#define PCI_CARVE_DONE      0u   /* every window in range has a frame        */
#define PCI_CARVE_NO_REGION 1u   /* the device Untyped did not answer        */
#define PCI_CARVE_FULL      2u   /* out of window slots, or nothing left     */
#define PCI_CARVE_NO_SKIP   3u   /* could not skip the gap below a window    */
#define PCI_CARVE_NO_FRAME  4u   /* could not carve the window itself        */
static uint32_t g_carve_state = PCI_CARVE_NO_REGION;
/* The gap the carve stopped on, and what the retype said about it.  A state
 * code names the STEP that failed; these name the reason, which for a skip is
 * always a number -- how far ahead the next window was, and whether the region
 * had that much left. */
static uint64_t g_carve_gap;
static long     g_carve_err;
static uint64_t g_carve_mark, g_carve_end;

/* ── BAR measurement ─────────────────────────────────────────────────────── */

/*
 * A BAR reports its size by telling you which of its bits are writable, and
 * the only way to ask is to write ones and read back.  While that is happening
 * the BAR holds a different address, so the device would decode a window that
 * is not its own — which is why DECODE IS TURNED OFF first and restored after.
 *
 * That is not a precaution, it is the documented sequence, and skipping it on
 * a machine where the firmware has already enabled a display controller means
 * a few microseconds in which writes meant for the framebuffer land somewhere
 * else.  It costs two extra config writes per function.
 */
/*
 * Measure the memory BARs of a function -- and ONLY of a function that has
 * six of them.
 *
 * `hdr_type` is the header type, masked of its multifunction bit.  Type 0 is
 * an ordinary device with BARs at 0x10..0x24.  Type 1 is a PCI-to-PCI BRIDGE,
 * which has exactly TWO, and whose 0x18..0x24 are its primary/secondary/
 * subordinate BUS NUMBERS and its memory and prefetchable windows.
 *
 * Sizing a BAR means writing all-ones to it and reading back which bits stuck.
 * Doing that to 0x18 on a bridge writes garbage into the bus numbers that
 * every configuration cycle beyond it is routed by -- for the moment before
 * they are written back, the machine behind that bridge is unreachable, and
 * whether it comes back is a question about timing rather than about code.
 *
 * This did not exist until the bridge walk did: before it, nothing of type 1
 * was ever measured.  A walk that reaches more of the machine has to be
 * careful with more of it.
 */
static void bar_measure(struct pci_fn *f, uint32_t hdr_type) {
    /* Anything that is not an ordinary device is left alone.  A bridge's two
     * BARs are all but unused, and a driver claims devices rather than the
     * bridges between them. */
    if (hdr_type != 0u) return;

    uint32_t cmd = cfg_read(f->devfn, CFG_COMMAND);
    cfg_write(f->devfn, CFG_COMMAND, cmd & ~(uint32_t)(PCI_CMD_IO | PCI_CMD_MEMORY));

    for (uint32_t b = 0; b < PCI_BAR_COUNT; b++) {
        uint32_t lo = cfg_read(f->devfn, CFG_BAR(b));
        if (lo == 0u) continue;
        if (lo & 1u) continue;                       /* I/O space: not ours */

        uint32_t type  = (lo >> 1) & 3u;
        uint32_t flags = PCI_BAR_MMIO;
        if (lo & (1u << 3)) flags |= PCI_BAR_PREFETCH;

        uint64_t base = (uint64_t)(lo & ~0xFu);
        uint64_t mask;
        cfg_write(f->devfn, CFG_BAR(b), 0xFFFFFFFFu);
        uint32_t probe = cfg_read(f->devfn, CFG_BAR(b));
        cfg_write(f->devfn, CFG_BAR(b), lo);

        if (type == 2u) {
            /* A 64-bit BAR: the high half lives in the NEXT BAR slot, and that
             * slot is not a BAR of its own — reporting it as one would offer a
             * window at whatever the high dword happens to be.
             *
             * A 64-bit BAR declared in the LAST slot has no next slot: the
             * read would land on 0x28, which is not a BAR at all.  Malformed,
             * and the answer to malformed is to skip it. */
            if (b + 1u >= PCI_BAR_COUNT) continue;
            flags |= PCI_BAR_64;
            uint32_t hi = cfg_read(f->devfn, CFG_BAR(b + 1u));
            base |= (uint64_t)hi << 32;
            cfg_write(f->devfn, CFG_BAR(b + 1u), 0xFFFFFFFFu);
            uint32_t probe_hi = cfg_read(f->devfn, CFG_BAR(b + 1u));
            cfg_write(f->devfn, CFG_BAR(b + 1u), hi);
            mask = ((uint64_t)probe_hi << 32) | (uint64_t)(probe & ~0xFu);
        } else {
            mask = 0xFFFFFFFF00000000ull | (uint64_t)(probe & ~0xFu);
        }
        if (mask == 0u) continue;                    /* the BAR is not wired */

        f->bar_base[b]  = base;
        f->bar_size[b]  = (~mask) + 1u;
        f->bar_flags[b] = (uint8_t)flags;
        f->bar_mmio    |= (uint8_t)(1u << b);
        if (flags & PCI_BAR_64) b++;                 /* skip the high half */
    }

    cfg_write(f->devfn, CFG_COMMAND, cmd);
}

/* ── the scan ────────────────────────────────────────────────────────────── */

/*
 * Which buses have been walked, so a malformed bridge cannot make this loop
 * forever.  A bus number is eight bits, so the whole space is 32 bytes.
 */
static uint8_t g_bus_done[PCI_MAX_BUSES / 8u];

static int bus_seen(uint32_t bus) {
    return (g_bus_done[(bus & 0xFFu) >> 3] >> (bus & 7u)) & 1u;
}
static void bus_mark(uint32_t bus) {
    g_bus_done[(bus & 0xFFu) >> 3] |= (uint8_t)(1u << (bus & 7u));
}

/*
 * Walk one bus, recording every function and queueing every bridge.
 *
 * A header type of 1 is a PCI-to-PCI bridge, and its SECONDARY bus number --
 * byte 1 of the dword at 0x18 -- is the bus on its far side.  Devices there
 * are invisible from bus 0, which is how a machine with two SATA drives and an
 * NVMe reported no mass-storage controller at all.
 *
 * Bridges are recorded like anything else.  They are not interesting to a
 * driver, but leaving them out would make the function list disagree with what
 * is actually on the machine, and this service is the only thing that can see
 * it.
 */
static void pci_scan_bus(uint32_t bus) {
    if (bus_seen(bus)) return;
    bus_mark(bus);

    for (uint32_t dev = 0; dev < 32u; dev++) {
        if (g_fn_count >= PCI_MAX_FUNCTIONS) { g_fn_truncated = 1u; return; }
        uint32_t fn_max = 1u;
        for (uint32_t fn = 0; fn < fn_max; fn++) {
            if (g_fn_count >= PCI_MAX_FUNCTIONS) { g_fn_truncated = 1u; return; }
            uint32_t bdf = (bus << 8) | (dev << 3) | fn;
            uint32_t vd  = cfg_read(bdf, CFG_VENDOR);
            if (vd == 0xFFFFFFFFu || vd == 0u) continue;
            uint32_t hdr = cfg_read(bdf, CFG_HEADER);
            if (fn == 0u && (hdr & 0x00800000u))
                fn_max = 8u;                          /* multifunction */

            struct pci_fn *f = &g_fn[g_fn_count];
            f->vendor_device = vd;
            f->class_code    = cfg_read(bdf, CFG_CLASS);
            f->devfn         = (uint16_t)bdf;
            for (uint32_t b = 0; b < PCI_BAR_COUNT; b++) f->bar_window[b] = -1;
            bar_measure(f, (hdr >> 16) & 0x7Fu);
            g_fn_count++;

            if (((hdr >> 16) & 0x7Fu) == 1u) {
                uint32_t secondary = (cfg_read(bdf, 0x18u) >> 8) & 0xFFu;
                if (secondary != 0u && secondary != bus)
                    pci_scan_bus(secondary);
            }
        }
    }
}

static void pci_scan(void) {
    /* Bus 0, and everything reachable from it.  The recursion is bounded by
     * the visited bitmap above, not by trusting the topology. */
    pci_scan_bus(0u);
}

/* ── carving the windows, in address order ───────────────────────────────── */

/*
 * Where the region IS and how much of it is left.
 *
 * `INV_UNTYPED_QUERY` and not `INV_UNTYPED_INFO`: the second answers one
 * number (bytes free) into a u64, and passing it a struct pointer writes eight
 * bytes into the front of the struct and returns success.  That is what the
 * first draft of this function did, and the result was a region whose base and
 * size both read as zero, so no window was ever inside it and every claim was
 * refused — with nothing failing anywhere near the mistake.
 *
 * The argument shape is the versioned-struct convention of the 1.0 ABI: the
 * capability word carries kind, version and the buffer size the CALLER can
 * accept, so the kernel writes the smaller of the two.
 */
struct pci_ut_query {
    uint32_t version, struct_size;
    uint64_t phys_base, total_bytes, used_bytes, generation;
    uint32_t child_count, is_device;
};
#define PCI_UTQ_ONE 2u
#define PCI_QARG(kind, sz) ((long)((uint64_t)(kind) | ((uint64_t)1u << 16) | \
                                   ((uint64_t)(uint32_t)(sz) << 32)))

static long ut_info(long ut, uint64_t *base, uint64_t *used, uint64_t *total) {
    struct pci_ut_query q;
    uint8_t *raw = (uint8_t *)&q;
    for (uint32_t i = 0; i < (uint32_t)sizeof(q); i++) raw[i] = 0;
    long r = iris_invoke2(PCI_QARG(PCI_UTQ_ONE, sizeof(q)), INV_UNTYPED_QUERY,
                          (long)(uintptr_t)&q, ut);
    if (r != 0) return r;
    if (!q.is_device) return -1;      /* not the PCI hole; refuse to carve */
    *base = q.phys_base; *used = q.used_bytes; *total = q.total_bytes;
    return 0;
}

/*
 * One frame per window, lowest address first.
 *
 * `skip` is the part of the region between the watermark and the next window,
 * retyped as a frame nobody maps.  That is what consuming an Untyped in order
 * means — the alternative would be a "retype at this address" operation, which
 * would make the watermark a suggestion and let two holders carve the same
 * bytes.
 */
static void pci_carve(void) {
    uint64_t base = 0, used = 0, total = 0;
    if (ut_info((long)PCI_SLOT_MMIO_UT, &base, &used, &total) != 0) {
        g_carve_state = PCI_CARVE_NO_REGION;
        return;
    }

    /* The region pays for its object headers out of this service's own RAM;
     * MMIO is not storage.  Set once, so a second call is not an error. */
    (void)iris_invoke1((long)PCI_SLOT_MMIO_UT, INV_UNTYPED_SET_DEVICE_BUDGET,
                       (long)IRIS_CPTR_OWN_UNTYPED);

    uint64_t mark = base + used;
    uint64_t end  = base + total;

    for (;;) {
        /* The lowest un-carved window at or above the watermark. */
        uint64_t best = 0; uint32_t bf = 0, bb = 0; int found = 0;
        for (uint32_t i = 0; i < g_fn_count; i++) {
            for (uint32_t b = 0; b < PCI_BAR_COUNT; b++) {
                if (!(g_fn[i].bar_mmio & (1u << b))) continue;
                if (g_fn[i].bar_window[b] >= 0) continue;
                uint64_t wb = g_fn[i].bar_base[b];
                uint64_t ws = g_fn[i].bar_size[b];
                if (wb < mark || wb + ws > end) continue;
                /*
                 * A window has to be expressible as a FRAME, and a frame is
                 * page-aligned and a whole number of pages.  A PCI memory BAR
                 * is aligned to its own SIZE, which for a 256-byte register
                 * block is 256 bytes -- so a real machine has windows that
                 * simply cannot be handed out this way, and the right answer
                 * is to leave them alone rather than to fail the whole carve.
                 */
                if ((wb & 0xFFFULL) != 0u || ws < 4096u) continue;
                if (!found || wb < best) { best = wb; bf = i; bb = b; found = 1; }
            }
        }
        if (!found) { g_carve_state = PCI_CARVE_DONE;  break; }
        if (g_window_count >= PCI_MAX_WINDOWS)
                     { g_carve_state = PCI_CARVE_FULL;  break; }

        /*
         * Step over the gap below this window, in WHOLE PAGES.
         *
         * The gap itself is whatever the firmware left, and there is no reason
         * for it to be a multiple of 4096 -- small BARs are aligned to their
         * own size, so a 256-byte register block leaves a gap ending 256 bytes
         * past a page boundary.  Retyping that many bytes as a frame is
         * INVALID_ARG, and the first real machine this ran on stopped the
         * carve there: gap 950528, which is 232 pages and 256 bytes.
         *
         * Rounding DOWN is safe because the allocator aligns the next
         * allocation UP to a page anyway: the remainder is skipped by the
         * same arithmetic that would have skipped it had it been asked for.
         */
        uint64_t pad_bytes = (best > mark) ? ((best - mark) & ~0xFFFULL) : 0u;
        if (pad_bytes != 0u) {
            /* Skipped, not wasted: these bytes belong to whatever the firmware
             * put below this window, and the service must not hand them out. */
            long pad = iris_invoke((long)PCI_SLOT_MMIO_UT, INV_UNTYPED_RETYPE,
                                   (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                                   (long)((uint64_t)(PCI_SLOT_BAR_BASE +
                                          PCI_MAX_WINDOWS) << 32),
                                   (long)pad_bytes);
            if (pad != 0) {
                g_carve_state = PCI_CARVE_NO_SKIP;
                g_carve_gap = pad_bytes; g_carve_err = pad;
                g_carve_mark = mark; g_carve_end = end;
                break;
            }
            (void)iris_invoke1(0, INV_CNODE_DELETE,
                               (long)(PCI_SLOT_BAR_BASE + PCI_MAX_WINDOWS));
        }

        uint32_t leaf = PCI_SLOT_BAR_BASE + g_window_count;
        uint64_t size = g_fn[bf].bar_size[bb];
        if (iris_invoke((long)PCI_SLOT_MMIO_UT, INV_UNTYPED_RETYPE,
                        (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                        (long)((uint64_t)leaf << 32), (long)size) != 0)
            { g_carve_state = PCI_CARVE_NO_FRAME; break; }

        g_window[g_window_count].base = best;
        g_window[g_window_count].size = size;
        g_window[g_window_count].leaf = leaf;
        g_fn[bf].bar_window[bb]  = (int8_t)g_window_count;
        g_fn[bf].bar_flags[bb]  |= PCI_BAR_CLAIMABLE;
        g_window_count++;
        mark = best + size;
    }
}

/* ── the service loop ────────────────────────────────────────────────────── */

static int fn_ok(uint64_t i) { return i < (uint64_t)g_fn_count; }

void pci_main(iris_cptr_t bootstrap_ch_h);
void pci_main(iris_cptr_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    pci_scan();
    pci_carve();

    for (;;) {
        struct iris_msg m;
        pci_msg_zero(&m);
        m.reply = (long)PCI_SLOT_REPLY;
        if (iris_msg_recv((long)PCI_SLOT_CTRL_EP, &m) != 0) continue;

        struct iris_msg rep;
        pci_msg_zero(&rep);
        rep.label = PCI_REP_ERR;

        switch (m.label) {
        case PCI_OP_COUNT:
            rep.label      = PCI_REP_OK;
            rep.words[0]   = g_fn_count;
            /* The high bit says the walk STOPPED because the table filled,
             * so the count beside it is a ceiling rather than a total. */
            rep.words[1]   = PCI_MAX_FUNCTIONS | (g_fn_truncated ? 0x80000000u : 0u);
            /* How many windows were CARVED.  A machine where the scan found
             * devices and the carve produced nothing is a machine where every
             * claim will be refused, and the difference between that and "no
             * devices" is the difference between a broken service and an empty
             * bus. */
            rep.words[2]   = g_window_count;
            rep.words[3]   = g_carve_state;
            rep.word_count = 4u;
            break;

        case PCI_OP_CARVE:

            /* Why the carve stopped, in numbers rather than a state code.

             * PCI_OP_COUNT already carries four words, which is all a message

             * has, and the state it carries names the STEP that failed -- not

             * the reason.  For a skip the reason is always a number. */

            rep.label      = PCI_REP_OK;

            rep.words[0]   = g_carve_gap;

            rep.words[1]   = (uint64_t)g_carve_err;

            rep.words[2]   = g_carve_mark;

            rep.words[3]   = g_carve_end;

            rep.word_count = 4u;

            break;

        case PCI_OP_INFO:
            if (fn_ok(m.words[0])) {
                const struct pci_fn *f = &g_fn[m.words[0]];
                rep.label      = PCI_REP_OK;
                rep.words[0]   = f->vendor_device;
                rep.words[1]   = f->class_code;
                rep.words[2]   = f->devfn;
                rep.words[3]   = f->bar_mmio;
                rep.word_count = 4u;
            }
            break;

        case PCI_OP_BAR:
            if (fn_ok(m.words[0]) && m.words[1] < PCI_BAR_COUNT) {
                const struct pci_fn *f = &g_fn[m.words[0]];
                uint32_t b = (uint32_t)m.words[1];
                rep.label      = PCI_REP_OK;
                rep.words[0]   = f->bar_base[b];
                rep.words[1]   = f->bar_size[b];
                rep.words[2]   = f->bar_flags[b];
                rep.word_count = 3u;
            }
            break;

        case PCI_OP_CLAIM:
            if (fn_ok(m.words[0]) && m.words[1] < PCI_BAR_COUNT) {
                const struct pci_fn *f = &g_fn[m.words[0]];
                uint32_t b = (uint32_t)m.words[1];
                int8_t   w = f->bar_window[b];
                if (w >= 0) {
                    rep.label      = PCI_REP_OK;
                    rep.words[0]   = g_window[w].base;
                    rep.words[1]   = g_window[w].size;
                    rep.word_count = 2u;
                    /*
                     * A COPY, not a move: this service keeps the frame, so a
                     * driver that dies does not take the window with it and a
                     * replacement can claim the same one.  The copy is an MDB
                     * child of the slot here, so revoking reaches it.
                     */
                    rep.cap        = (long)g_window[w].leaf;
                    rep.cap_rights = RIGHT_READ | RIGHT_WRITE;
                }
            }
            break;

        case PCI_OP_ENABLE:
            if (fn_ok(m.words[0])) {
                const struct pci_fn *f = &g_fn[m.words[0]];
                uint32_t set = (uint32_t)m.words[1] & PCI_CMD_SETTABLE;
                uint32_t cmd = cfg_read(f->devfn, CFG_COMMAND);
                cfg_write(f->devfn, CFG_COMMAND, cmd | set);
                rep.label      = PCI_REP_OK;
                rep.words[0]   = cfg_read(f->devfn, CFG_COMMAND);
                rep.word_count = 1u;
            }
            break;

        default:
            break;
        }

        (void)iris_msg_reply((long)PCI_SLOT_REPLY, &rep);
    }
}
