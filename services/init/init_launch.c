/*
 * How long init waits for `iris_test` before calling it hung.
 *
 * A CEILING, not a budget: the suite finishing is what normally ends the wait,
 * and this only decides how long a genuinely stuck run takes to say so.  It
 * was 25 s, sized when the suite was a one-processor suite whose waits were
 * counted in yields.  Its waits are counted in TIME now (SMP roadmap §9.3), and
 * on four processors it legitimately spends more of it — enough that the old
 * ceiling reported a healthy run as a hang.  QEMU's own timeout, which the
 * smoke script scales by the core count, is the outer bound.
 */
#define INIT_TEST_WATCHDOG_NS 120000000000ull

/*
 * init_launch.c — service launch for init (Phase 14 extraction).
 *
 * Everything here is MOVED VERBATIM from main.c (no functional change): the
 * fb / console / svcmgr / iris_test spawns — initrd loads via
 * svc_load_minted, the pre-start CSpace mint tables each child receives, and
 * the spawn error paths.  Boot order and every log string are unchanged;
 * main.c remains the orchestrator that calls these in sequence.
 */

#include "init.h"
#include "../timer/timer_proto.h"
#include <iris/pci_ep_proto.h>
#include <iris/blk_ep_proto.h>
#include <iris/net_ep_proto.h>
#include <iris/fs_ep_proto.h>
#include "../common/iris_map.h"
#include "../common/iris_timer.h"
#include <iris/endpoint_proto.h>
#include "../common/svc_loader.h"

/*
 * Say what happened to a spawn's pre-start mints.
 *
 * They are non-fatal by design — a child with an empty slot is better than no
 * child — but they used to be non-fatal AND silent, on the argument that a
 * consumer's smoke gate would catch anything missing.  That holds only for
 * capabilities some marker covers.  The domain authority (A-34) had none: its
 * destination slot was occupied, the exclusive mint refused, and the child
 * started without it with nothing anywhere saying so, until a test three
 * hundred cases later asked and got ACCESS_DENIED.
 *
 * ALREADY_EXISTS is the interesting one and almost always means two entries of
 * the same table name the same slot — a bug in the table, not in the system.
 * That is why the slot is named: with it the mistake is one grep away, and
 * without it somebody re-derives it from a failing test.
 */
static void init_report_mints(const char *who, const struct svc_mint *m,
                              uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (m[i].result == SVC_MINT_OK || m[i].result == SVC_MINT_SKIPPED)
            continue;
        {
            static char line[80];
            uint32_t p = 0, sl = (uint32_t)m[i].slot;
            const char *t = "[INIT] MINT FAILED ";
            while (*t) line[p++] = *t++;
            t = who; while (*t) line[p++] = *t++;
            line[p++] = ' '; line[p++] = 's'; line[p++] = 'l';
            line[p++] = 'o'; line[p++] = 't'; line[p++] = '=';
            line[p++] = (char)('0' + (sl / 100u) % 10u);
            line[p++] = (char)('0' + (sl / 10u) % 10u);
            line[p++] = (char)('0' + sl % 10u);
            line[p++] = ' '; line[p++] = 'e'; line[p++] = '=';
            {
                uint32_t e = (uint32_t)(m[i].result < 0 ? -m[i].result
                                                        : m[i].result);
                line[p++] = (char)('0' + (e / 10u) % 10u);
                line[p++] = (char)('0' + e % 10u);
            }
            line[p++] = '\r'; line[p++] = '\n'; line[p] = 0;
            init_log(line);
        }
    }
}

static const char init_console_load_fail[] = "[INIT] console load FAILED\r\n";
static const char init_console_ioport_fail[] = "[INIT] console ioport FAILED\r\n";
static const char init_console_chan_fail[] = "[INIT] console ep FAILED\r\n";
static const char init_fb_load_fail[] = "[INIT] fb load FAILED\r\n";

/* ── fb spawn (Phase 30: ring-3 framebuffer painter) ────────────────────── */

/* Slot 42 held fb's FRAMEBUFFER-restricted clone between its construction and
 * the pre-start mint.  Stage 5 Step 2 deleted the construction: fb is minted
 * the framebuffer control capability from init's own slot. */

void init_spawn_fb(void) {
    iris_cptr_t fb_proc_h  = IRIS_CPTR_NULL;
    iris_cptr_t fb_boot_h  = IRIS_CPTR_NULL;
    long r;

    /* Stage 5 Step 2: fb receives the FRAMEBUFFER CONTROL capability — the
     * whole of what it is allowed to do — as a pre-start mint from init's own
     * slot, so the grant is an MDB child of init's and stays revocable.
     *
     * It used to be a SYS_BOOTCAP_RESTRICT of init's monolithic boot
     * capability into a scratch slot, minted on, then deleted: three steps
     * that existed only because the authority fb needed was one bit of an
     * object that also carried spawn and debug authority.  Delegating a
     * capability that means exactly one thing needs none of them. */
    {
        struct svc_mint fb_mints[2] = { 0 };
        fb_mints[0].slot     = IRIS_CPTR_FB_CONTROL;
        fb_mints[0].src_cptr = IRIS_CPTR_FB_CONTROL;
        fb_mints[0].rights   = RIGHT_READ;
        fb_mints[0].badge    = 0;
        /* Ledger D-5/D-9: the framebuffer REGION, as a device Untyped fb
         * retypes its own frame from.  It used to come as a KVMO the kernel
         * fabricated inside SYS_FRAMEBUFFER_VMO — the last memory object in
         * the system nobody retyped.  The control capability above is now what
         * it says it is: the authority to ask where the framebuffer IS. */
        fb_mints[1].slot     = IRIS_CPTR_DEVICE_UNTYPED;
        fb_mints[1].src_cptr = IRIS_CPTR_DEVICE_UNTYPED;
        fb_mints[1].rights   = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE;
        fb_mints[1].badge    = 0;
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "fb", &fb_proc_h, &fb_boot_h, fb_mints, 2u,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /* fb maps the framebuffer into a window nothing
                                * has touched, so it owes every level under it. */
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED, /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
        init_report_mints("fb", fb_mints, 2u);
    }
    if (r < 0)
        init_early_serial_write(init_fb_load_fail);

    init_close(&fb_proc_h);
    init_close(&fb_boot_h);
}


/* ── pci spawn (Stage 10: the bus is a service) ─────────────────────────── */

/*
 * The one task that may reach PCI configuration space.
 *
 * 0xCF8/0xCFC is a single pair of ports through which any device on the
 * machine can be reprogrammed, so a capability for it is a capability over the
 * whole bus.  Handing that to each driver would undo, one port range at a
 * time, the thing Stage 10-dma just established: that a driver reaches only
 * what its capabilities name.  So init claims those eight ports ONCE, gives
 * them to this service, and gives them to nothing else ever again.
 *
 * It also gets the PCI hole (`IRIS_CPTR_MMIO_UNTYPED`), which is what makes
 * the restriction hold rather than merely being observed: a driver holds no
 * device Untyped, so there is no frame it could retype over a window it was
 * not handed.
 *
 * What init keeps is the endpoint, which is what it hands on to whoever needs
 * to find a device.
 *
 * Returns 1 on success, 0 on failure.
 */
int init_spawn_pci(void) {
    iris_cptr_t pc_proc_h = IRIS_CPTR_NULL;
    iris_cptr_t pc_boot_h = IRIS_CPTR_NULL;
    long r;

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_PCI_EP, 0) < 0) { init_log("[USER] pci: ep\n"); return 0; }

    /* The configuration ports, as a capability, out of the port AUTHORITY.
     * Eight ports: 0xCF8..0xCFF is the address register, the data register and
     * the two aliases between them, and nothing else is in the range. */
    if (iris_invoke((long)IRIS_CPTR_IOPORT_CONTROL, INV_BOOT_CREATE_IOPORT,
                    (long)(0xCF8u | (8u << 16)), (long)IRIS_CPTR_INIT_UNTYPED,
                    (long)((uint64_t)INIT_SLOT_PCI_IOPORT << 32)) != 0) {
        init_log("[USER] pci: ioport\n"); return 0;
    }

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                         INIT_SLOT_PCI_REPLY, 0) < 0) { init_log("[USER] pci: reply\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_UNTYPED,
                         INIT_SLOT_PCI_UT, 1 << 20) < 0) { init_log("[USER] pci: ut\n"); return 0; }

    {
        struct svc_mint pc[5] = { 0 };
        uint32_t n = 0;
        pc[n].slot = PCI_SLOT_CTRL_EP;  pc[n].src_cptr = INIT_SLOT_PCI_EP;
        pc[n].rights = RIGHT_READ;      pc[n].badge = 0; n++;
        pc[n].slot = PCI_SLOT_IOPORT;   pc[n].src_cptr = INIT_SLOT_PCI_IOPORT;
        pc[n].rights = RIGHT_READ | RIGHT_WRITE; pc[n].badge = 0; n++;
        pc[n].slot = PCI_SLOT_REPLY;    pc[n].src_cptr = INIT_SLOT_PCI_REPLY;
        pc[n].rights = RIGHT_READ | RIGHT_WRITE; pc[n].badge = 0; n++;
        pc[n].slot = PCI_SLOT_MMIO_UT;  pc[n].src_cptr = IRIS_CPTR_MMIO_UNTYPED;
        pc[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        pc[n].badge = 0; n++;
        pc[n].slot = IRIS_CPTR_OWN_UNTYPED; pc[n].src_cptr = INIT_SLOT_PCI_UT;
        pc[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        pc[n].badge = 0; n++;

        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "pci", &pc_proc_h, &pc_boot_h, pc, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED,
                               /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
        init_report_mints("pci", pc, n);
    }
    /* The service holds the mints now.  init keeps the ENDPOINT and drops its
     * own copy of everything else — including the port capability, which is
     * the whole point: after this returns, exactly one task in the system can
     * reach configuration space. */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_PCI_REPLY);
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_PCI_IOPORT);
    init_close(&pc_proc_h);
    init_close(&pc_boot_h);
    if (r < 0) return 0;

    /*
     * Ask it what it found, which is both the readiness check and the only
     * boot-time evidence that the scan happened at all.
     *
     * A CALL blocks until the service receives, and the service does not
     * receive until it has walked the bus and carved its windows — so this
     * returning is the signal that the machine has been described.  A bus
     * driver that started and then silently found nothing would otherwise be
     * indistinguishable from one that started.
     */
    {
        struct iris_msg m;
        { uint8_t *z = (uint8_t *)&m;
          for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
        m.label = PCI_OP_COUNT;
        if (iris_msg_call((long)INIT_SLOT_PCI_EP, &m) == 0 &&
            m.label == PCI_REP_OK) {
            char b[64] = "[USER][INIT] pci: functions ";
            uint32_t k = 0; while (b[k]) k++;
            uint32_t n2 = (uint32_t)m.words[0];
            if (n2 >= 10u) b[k++] = (char)('0' + (n2 / 10u) % 10u);
            b[k++] = (char)('0' + n2 % 10u);
            b[k++] = ' '; b[k++] = 'w'; b[k++] = 'i'; b[k++] = 'n';
            b[k++] = 'd'; b[k++] = 'o'; b[k++] = 'w'; b[k++] = 's'; b[k++] = ' ';
            uint32_t w = (uint32_t)m.words[2];
            if (w >= 10u) b[k++] = (char)('0' + (w / 10u) % 10u);
            b[k++] = (char)('0' + w % 10u);
            b[k++] = ' '; b[k++] = 'c'; b[k++] = 'a'; b[k++] = 'r';
            b[k++] = 'v'; b[k++] = 'e'; b[k++] = ' ';
            b[k++] = (char)('0' + (uint32_t)(m.words[3] % 10u));
            b[k++] = '\n'; b[k] = 0;
            init_log(b);
            /* carve 0 is "every window in range has a frame"; anything else
             * means some device on this machine cannot be handed to a driver,
             * and the reason is one of PCI_CARVE_* in the service. */
            return 1;
        }
        init_log("[USER][INIT] pci: no answer\n");
        return 0;
    }
}

/* ── fs spawn (Stage 10: a filesystem that survives the power going off) ─── */

/*
 * The persistent filesystem.
 *
 * It gets the LEAST of any service here: an endpoint to serve on, a reply
 * object, an endpoint to the BLOCK service, and memory.  No disk, no
 * controller, no device Untyped, no IOSpaceControl — it cannot find storage,
 * only ask for it.  That is the shape the whole stack was built to make
 * possible: the thing that owns your data holds no hardware at all.
 *
 * Returns 1 when a filesystem is mounted.
 */
int init_spawn_fs(void) {
    iris_cptr_t fs_proc_h = IRIS_CPTR_NULL;
    iris_cptr_t fs_boot_h = IRIS_CPTR_NULL;
    long r;

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_FS_EP, 0) < 0) { init_log("[USER] fs: ep\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                         INIT_SLOT_FS_REPLY, 0) < 0) { init_log("[USER] fs: reply\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_UNTYPED,
                         INIT_SLOT_FS_UT, 2 << 20) < 0) { init_log("[USER] fs: ut\n"); return 0; }

    {
        struct svc_mint fm[4] = { 0 };
        uint32_t n = 0;
        fm[n].slot = FS_SLOT_CTRL_EP;  fm[n].src_cptr = INIT_SLOT_FS_EP;
        fm[n].rights = RIGHT_READ;     fm[n].badge = 0; n++;
        fm[n].slot = FS_SLOT_REPLY;    fm[n].src_cptr = INIT_SLOT_FS_REPLY;
        fm[n].rights = RIGHT_READ | RIGHT_WRITE; fm[n].badge = 0; n++;
        fm[n].slot = FS_SLOT_BLK_EP;   fm[n].src_cptr = INIT_SLOT_BLK_EP;
        fm[n].rights = RIGHT_WRITE;    fm[n].badge = 0; n++;
        fm[n].slot = IRIS_CPTR_OWN_UNTYPED; fm[n].src_cptr = INIT_SLOT_FS_UT;
        fm[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        fm[n].badge = 0; n++;

        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "fs", &fs_proc_h, &fs_boot_h, fm, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED,
                               /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
        init_report_mints("fs", fm, n);
    }
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_FS_REPLY);
    init_close(&fs_proc_h);
    init_close(&fs_boot_h);
    if (r < 0) return 0;

    /*
     * Mount, then prove the mount MEANS something.
     *
     * `gen` is the number of times a boot has mounted this disk, read from the
     * medium and written back.  A second boot over the same image reports a
     * higher number than the first, which is the only way "persistent" can be
     * demonstrated from inside the machine — everything else a filesystem can
     * tell you about itself is equally true of one that forgets.
     *
     * Then a file: written with this boot's generation in it and read back.
     * A round trip through the directory, the data sector, the block driver
     * and the controller, and the NEXT boot reads what this one wrote.
     */
    {
        struct iris_msg m;
        { uint8_t *z = (uint8_t *)&m;
          for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
        m.label = FS_OP_STAT;
        if (iris_msg_call((long)INIT_SLOT_FS_EP, &m) != 0 ||
            m.label != FS_REP_OK || m.words[0] == 0u) {
            init_log("[USER][INIT] fs: not mounted\n");
            return 0;
        }
        uint32_t gen = (uint32_t)m.words[1];
        uint32_t formatted = (uint32_t)m.words[3];

        /* A file whose contents are this boot's generation, so the next boot
         * can tell whose bytes it is reading. */
        int wrote = 0, read_back = 0;
        { uint8_t *z = (uint8_t *)&m;
          for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
        m.label = FS_OP_BUF;
        m.recv_slot = (long)INIT_SLOT_FS_BUF;
        (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_FS_BUF);
        if (iris_msg_call((long)INIT_SLOT_FS_EP, &m) == 0 &&
            m.label == FS_REP_OK && m.got_caps != 0u &&
            iris_map_frame(INIT_SLOT_FS_BUF, IRIS_CPTR_OWN_VSPACE,
                           g_init_untyped_c, INIT_SLOT_NET_PT,
                           0x80B2000000ULL, 4096u, 1ull) == 0) {
            volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)0x80B2000000ULL;
            w[0] = 0x53495249u;           /* "IRIS" */
            w[1] = gen;
            (void)iris_invoke2((long)INIT_SLOT_FS_BUF, INV_FRAME_UNMAP,
                               (long)IRIS_CPTR_OWN_VSPACE, (long)0x80B2000000ULL);

            { uint8_t *z = (uint8_t *)&m;
              for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
            m.label = FS_OP_WRITE;
            m.words[0] = 0x676F6C2E746F6F62ULL;   /* "boot.log", packed little-endian */
            m.words[1] = 0;
            m.words[2] = 8u;
            m.word_count = 3u;
            wrote = (iris_msg_call((long)INIT_SLOT_FS_EP, &m) == 0 &&
                     m.label == FS_REP_OK);
        }
        if (wrote) {
            { uint8_t *z = (uint8_t *)&m;
              for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
            m.label = FS_OP_READ;
            m.words[0] = 0x676F6C2E746F6F62ULL;   /* the same name, read back */
            m.words[1] = 0;
            m.word_count = 2u;
            m.recv_slot = (long)INIT_SLOT_FS_BUF;
            (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_FS_BUF);
            if (iris_msg_call((long)INIT_SLOT_FS_EP, &m) == 0 &&
                m.label == FS_REP_OK && m.got_caps != 0u &&
                iris_map_frame(INIT_SLOT_FS_BUF, IRIS_CPTR_OWN_VSPACE,
                               g_init_untyped_c, INIT_SLOT_NET_PT,
                               0x80B3000000ULL, 4096u, 0ull) == 0) {
                const volatile uint32_t *w =
                    (const volatile uint32_t *)(uintptr_t)0x80B3000000ULL;
                read_back = (w[0] == 0x53495249u && w[1] == gen);
                (void)iris_invoke2((long)INIT_SLOT_FS_BUF, INV_FRAME_UNMAP,
                                   (long)IRIS_CPTR_OWN_VSPACE, (long)0x80B3000000ULL);
            }
        }

        {
            char b[80] = "[USER][INIT] fs: mounted gen ";
            uint32_t k = 0; while (b[k]) k++;
            if (gen >= 100u) b[k++] = (char)('0' + (gen / 100u) % 10u);
            if (gen >= 10u)  b[k++] = (char)('0' + (gen / 10u) % 10u);
            b[k++] = (char)('0' + gen % 10u);
            b[k++] = ' ';
            if (formatted) { b[k++]='f'; b[k++]='o'; b[k++]='r'; b[k++]='m';
                             b[k++]='a'; b[k++]='t'; b[k++]='t'; b[k++]='e';
                             b[k++]='d'; }
            else           { b[k++]='e'; b[k++]='x'; b[k++]='i'; b[k++]='s';
                             b[k++]='t'; b[k++]='i'; b[k++]='n'; b[k++]='g'; }
            b[k++] = ' '; b[k++] = 'f'; b[k++] = 'i'; b[k++] = 'l'; b[k++] = 'e';
            b[k++] = ' ';
            b[k++] = (char)('0' + (read_back ? 1u : 0u));
            b[k++] = '\n'; b[k] = 0;
            init_log(b);
        }
        return 1;
    }
}

/* ── net spawn (Stage 10: a network card is a driver too) ───────────────── */

/* Defined below: the boot check that asks whether the network actually works,
 * rather than whether a card came up.  Declared here because the spawn calls
 * it and the ARP it builds is long enough to want to be out of the way. */
static int init_net_arp_probe(void);

/*
 * The e1000 network service.
 *
 * The same manifest as the disk service, and the same reason for each entry —
 * which is the point worth noticing: two drivers for completely different
 * hardware need exactly the same five capabilities, because "drive a PCI
 * device with DMA" is one shape and this system has a name for each part of it.
 *
 * A network card is the clearest case for IOSPACE_CONTROL in the whole tree.
 * A disk controller reads a command table when you tell it to; a NIC reads a
 * RING of descriptors continuously, at addresses the driver wrote into two
 * registers, and writes received packets into addresses it finds there.
 * Nothing tells it to stop.  A driver that got those wrong would have the card
 * scribbling asynchronously with no call to attribute it to.
 *
 * Returns 1 on success, 0 on failure.
 */
int init_spawn_net(void) {
    iris_cptr_t nt_proc_h = IRIS_CPTR_NULL;
    iris_cptr_t nt_boot_h = IRIS_CPTR_NULL;
    long r;

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_NET_EP, 0) < 0) { init_log("[USER] net: ep\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                         INIT_SLOT_NET_REPLY, 0) < 0) { init_log("[USER] net: reply\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_UNTYPED,
                         INIT_SLOT_NET_UT, 2 << 20) < 0) { init_log("[USER] net: ut\n"); return 0; }

    {
        struct svc_mint nt[5] = { 0 };
        uint32_t n = 0;
        nt[n].slot = NET_SLOT_CTRL_EP;   nt[n].src_cptr = INIT_SLOT_NET_EP;
        nt[n].rights = RIGHT_READ;       nt[n].badge = 0; n++;
        nt[n].slot = NET_SLOT_REPLY;     nt[n].src_cptr = INIT_SLOT_NET_REPLY;
        nt[n].rights = RIGHT_READ | RIGHT_WRITE; nt[n].badge = 0; n++;
        nt[n].slot = NET_SLOT_PCI_EP;    nt[n].src_cptr = INIT_SLOT_PCI_EP;
        nt[n].rights = RIGHT_WRITE;      nt[n].badge = 0; n++;
        nt[n].slot = NET_SLOT_IOSPACE_C; nt[n].src_cptr = IRIS_CPTR_IOSPACE_CONTROL;
        nt[n].rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        nt[n].badge = 0; n++;
        nt[n].slot = IRIS_CPTR_OWN_UNTYPED; nt[n].src_cptr = INIT_SLOT_NET_UT;
        nt[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        nt[n].badge = 0; n++;

        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "net", &nt_proc_h, &nt_boot_h, nt, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED,
                               /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
        init_report_mints("net", nt, n);
    }
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_NET_REPLY);
    init_close(&nt_proc_h);
    init_close(&nt_boot_h);
    if (r < 0) return 0;

    {
        struct iris_msg m;
        { uint8_t *z = (uint8_t *)&m;
          for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
        m.label = NET_OP_INFO;
        if (iris_msg_call((long)INIT_SLOT_NET_EP, &m) == 0 &&
            m.label == NET_REP_OK) {
            static const char hx[] = "0123456789abcdef";
            char b[72] = "[USER][INIT] net: link ";
            uint32_t k = 0; while (b[k]) k++;
            b[k++] = (char)('0' + (uint32_t)(m.words[0] & 1u));
            b[k++] = ' '; b[k++] = 'm'; b[k++] = 'a'; b[k++] = 'c'; b[k++] = ' ';
            for (int byte = 0; byte < 6; byte++) {
                uint32_t v = (uint32_t)((m.words[1] >> (8 * byte)) & 0xFFu);
                b[k++] = hx[(v >> 4) & 0xFu]; b[k++] = hx[v & 0xFu];
            }
            b[k++] = ' '; b[k++] = 'd'; b[k++] = 'm'; b[k++] = 'a'; b[k++] = ' ';
            if (m.words[3]) { b[k++]='c'; b[k++]='o'; b[k++]='n'; b[k++]='t';
                              b[k++]='a'; b[k++]='i'; b[k++]='n'; b[k++]='e';
                              b[k++]='d'; }
            else            { b[k++]='o'; b[k++]='p'; b[k++]='e'; b[k++]='n'; }
            b[k++] = '\n'; b[k] = 0;
            init_log(b);
            if (!(m.words[0] & 1u)) return 0;
            /* A card that is up is not a network that works. */
            if (!init_net_arp_probe())
                init_log("[USER][INIT] net: the gateway did not answer\n");
            return 1;
        }
        init_log("[USER][INIT] net: no answer\n");
        return 0;
    }
}

/*
 * Does the network actually WORK?
 *
 * `net: link 1` says the driver brought a card up.  It does not say a frame
 * ever left the machine, and a transmit-only check proves nothing at all: the
 * card reports a descriptor done whether or not anything was listening.  A
 * loopback test is not much better — it proves the card talks to itself.
 *
 * So this sends an ARP request for the gateway and waits for the answer.  What
 * comes back exercises the transmit path, the receive ring, the card's receive
 * filter and a PEER that is not this driver, in one round trip; and it is the
 * smallest thing that does.
 *
 * The ARP lives HERE rather than in the driver on purpose.  A driver that
 * parsed ARP would be policy inside a driver — the thing this whole system is
 * arranged to avoid — so `net` moves frames and a client understands them.
 * init is that client for the same reason it is the one that checks the
 * filesystem at boot: a supervisor's job includes knowing whether the things
 * it started work.
 *
 * Returns 1 if the gateway answered.
 */
static int init_net_arp_probe(void) {
    struct iris_msg m;
    const uint64_t TX_VA = 0x80B0000000ULL, RX_VA = 0x80B1000000ULL;

    /* The transmit buffer, as a capability we may write. */
    { uint8_t *z = (uint8_t *)&m;
      for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
    m.label     = NET_OP_TXBUF;
    m.recv_slot = (long)INIT_SLOT_NET_TX;
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_NET_TX);
    if (iris_msg_call((long)INIT_SLOT_NET_EP, &m) != 0 ||
        m.label != NET_REP_OK || m.got_caps == 0u) return 0;
    if (iris_map_frame(INIT_SLOT_NET_TX, IRIS_CPTR_OWN_VSPACE,
                       g_init_untyped_c, INIT_SLOT_NET_PT,
                       TX_VA, 4096u, 1ull) != 0) return 0;

    /* Our own MAC, which the request has to carry as the sender. */
    uint64_t mac = 0;
    { uint8_t *z = (uint8_t *)&m;
      for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
    m.label = NET_OP_INFO;
    if (iris_msg_call((long)INIT_SLOT_NET_EP, &m) != 0 ||
        m.label != NET_REP_OK) return 0;
    mac = m.words[1];

    /*
     * An ARP request, 42 bytes, built by hand.
     *
     * 10.0.2.15 and 10.0.2.2 are what QEMU's userspace network offers: the
     * address it hands out and the gateway it answers for.  A request from
     * outside that subnet gets no reply, which would look exactly like a
     * driver that does not work — so the addresses are part of the test setup
     * and are named in the runner beside the `-netdev user` that provides
     * them.
     */
    {
        volatile uint8_t *f = (volatile uint8_t *)(uintptr_t)TX_VA;
        for (uint32_t i = 0; i < 64u; i++) f[i] = 0;
        for (uint32_t i = 0; i < 6u; i++) f[i] = 0xFFu;              /* broadcast */
        for (uint32_t i = 0; i < 6u; i++) f[6 + i] = (uint8_t)(mac >> (8 * i));
        f[12] = 0x08; f[13] = 0x06;                                  /* ARP */
        f[14] = 0x00; f[15] = 0x01;                                  /* Ethernet */
        f[16] = 0x08; f[17] = 0x00;                                  /* IPv4 */
        f[18] = 6;    f[19] = 4;
        f[20] = 0x00; f[21] = 0x01;                                  /* request */
        for (uint32_t i = 0; i < 6u; i++) f[22 + i] = (uint8_t)(mac >> (8 * i));
        f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;                /* 10.0.2.15 */
        f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;                 /* 10.0.2.2  */
    }

    { uint8_t *z = (uint8_t *)&m;
      for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
    m.label = NET_OP_SEND; m.words[0] = 42u; m.word_count = 1u;
    if (iris_msg_call((long)INIT_SLOT_NET_EP, &m) != 0 ||
        m.label != NET_REP_OK) return 0;

    /*
     * Wait for the answer, bounded in TIME.
     *
     * The first two drafts bounded it by a POLL COUNT, and both were wrong in
     * the same way: a poll is an IPC round trip through the driver, so what a
     * count buys depends entirely on how fast the machine is.  Twenty thousand
     * was generous enough on the success path and pushed the boot past the
     * gate's deadline on a machine with no network; two hundred and fifty
     * fixed that and started missing REAL replies under an IOMMU, where every
     * round trip costs more.  A count cannot be both.
     *
     * Time can.  Two seconds: far shorter than anything the gate's deadline
     * cares about, and long enough for a peer whose stack runs in the
     * EMULATOR's main loop rather than on the virtual wire — which is the
     * detail a quarter of a second got wrong.  A guest spinning in yields
     * gives that loop very little, so the reply can take hundreds of
     * milliseconds of wall clock to appear even though nothing is far away.
     *
     * `SYS_CLOCK_GET` is one of the four syscalls that survived the ABI freeze
     * (A-27: the counter is unprivileged on this architecture anyway).
     */
    long t0 = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
    for (;;) {
        long now = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
        if (t0 > 0 && now > 0 && (uint64_t)(now - t0) > 2000000000ull) break;

        { uint8_t *z = (uint8_t *)&m;
          for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
        m.label     = NET_OP_RECV;
        m.recv_slot = (long)INIT_SLOT_NET_RX;
        (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_NET_RX);
        if (iris_msg_call((long)INIT_SLOT_NET_EP, &m) != 0) return 0;
        if (m.label != NET_REP_OK || m.words[0] == 0u) {
            (void)iris_syscall4(SYS_YIELD, 0, 0, 0, 0);
            continue;
        }

        uint64_t off = m.words[2];
        if (iris_map_frame(INIT_SLOT_NET_RX, IRIS_CPTR_OWN_VSPACE,
                           g_init_untyped_c, INIT_SLOT_NET_PT,
                           RX_VA, 4096u, 0ull) != 0) return 0;

        const volatile uint8_t *f = (const volatile uint8_t *)(uintptr_t)(RX_VA + off);
        int is_arp_reply = (f[12] == 0x08 && f[13] == 0x06 &&
                            f[20] == 0x00 && f[21] == 0x02);
        int from_gateway = (f[28] == 10 && f[29] == 0 &&
                            f[30] == 2  && f[31] == 2);
        if (is_arp_reply && from_gateway) {
            static const char hx[] = "0123456789abcdef";
            char b[72] = "[USER][INIT] net: gateway answered, mac ";
            uint32_t k = 0; while (b[k]) k++;
            for (uint32_t i = 0; i < 6u; i++) {
                uint8_t v = f[22 + i];
                b[k++] = hx[(v >> 4) & 0xFu]; b[k++] = hx[v & 0xFu];
            }
            b[k++] = '\n'; b[k] = 0;
            init_log(b);
            (void)iris_invoke2((long)INIT_SLOT_NET_RX, INV_FRAME_UNMAP,
                               (long)IRIS_CPTR_OWN_VSPACE, (long)RX_VA);
            return 1;
        }
        /* Some other frame — QEMU's stack sends a few.  Unmap and keep
         * waiting; the next RECV takes this buffer back anyway. */
        (void)iris_invoke2((long)INIT_SLOT_NET_RX, INV_FRAME_UNMAP,
                           (long)IRIS_CPTR_OWN_VSPACE, (long)RX_VA);
    }
    return 0;
}

/* ── blk spawn (Stage 10: storage is a driver, and the driver is in ring 3) ── */

/*
 * The AHCI disk service.
 *
 * What it gets is worth reading as a list, because the list IS the claim: an
 * endpoint to serve on, a reply object, an endpoint to the BUS service, the
 * authority to contain its own controller's DMA, and memory.  It gets no I/O
 * ports, no interrupt, no spawn capability and no filesystem — it cannot even
 * find its own controller without asking somebody else, and the somebody else
 * hands back one device's registers and nothing more.
 *
 * IOSPACE_CONTROL is the interesting one.  AHCI is a bus master: the driver
 * writes physical addresses into a command table and the controller reads and
 * writes them itself, which is exactly the reach Stage 10-dma made
 * containable.  Giving the driver the authority to contain ITSELF is what lets
 * it bind an IOSpace to its controller and map only its own buffers — a
 * driver that is trusted to say what its hardware may touch, and able to say
 * "only this".
 *
 * Returns 1 on success, 0 on failure.
 */
int init_spawn_blk(void) {
    iris_cptr_t bk_proc_h = IRIS_CPTR_NULL;
    iris_cptr_t bk_boot_h = IRIS_CPTR_NULL;
    long r;

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_BLK_EP, 0) < 0) { init_log("[USER] blk: ep\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                         INIT_SLOT_BLK_REPLY, 0) < 0) { init_log("[USER] blk: reply\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_UNTYPED,
                         INIT_SLOT_BLK_UT, 2 << 20) < 0) { init_log("[USER] blk: ut\n"); return 0; }

    {
        struct svc_mint bk[5] = { 0 };
        uint32_t n = 0;
        bk[n].slot = BLK_SLOT_CTRL_EP;   bk[n].src_cptr = INIT_SLOT_BLK_EP;
        bk[n].rights = RIGHT_READ;       bk[n].badge = 0; n++;
        bk[n].slot = BLK_SLOT_REPLY;     bk[n].src_cptr = INIT_SLOT_BLK_REPLY;
        bk[n].rights = RIGHT_READ | RIGHT_WRITE; bk[n].badge = 0; n++;
        bk[n].slot = BLK_SLOT_PCI_EP;    bk[n].src_cptr = INIT_SLOT_PCI_EP;
        bk[n].rights = RIGHT_WRITE;      bk[n].badge = 0; n++;
        bk[n].slot = BLK_SLOT_IOSPACE_C; bk[n].src_cptr = IRIS_CPTR_IOSPACE_CONTROL;
        bk[n].rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        bk[n].badge = 0; n++;
        bk[n].slot = IRIS_CPTR_OWN_UNTYPED; bk[n].src_cptr = INIT_SLOT_BLK_UT;
        bk[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        bk[n].badge = 0; n++;

        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "blk", &bk_proc_h, &bk_boot_h, bk, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED,
                               /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
        init_report_mints("blk", bk, n);
    }
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_BLK_REPLY);
    init_close(&bk_proc_h);
    init_close(&bk_boot_h);
    if (r < 0) return 0;

    /*
     * Ask it what it found.  The CALL blocks until the service receives, and
     * the service does not receive until it has claimed its controller, built
     * its command structures, contained its DMA if it can, and READ A SECTOR —
     * so this returning is the signal that there is a working disk behind the
     * endpoint, not merely a service that started.
     */
    {
        struct iris_msg m;
        { uint8_t *z = (uint8_t *)&m;
          for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) z[i] = 0; }
        m.label = BLK_OP_INFO;
        if (iris_msg_call((long)INIT_SLOT_BLK_EP, &m) == 0 &&
            m.label == BLK_REP_OK) {
            char b[96] = "[USER][INIT] blk: disk ";
            uint32_t k = 0; while (b[k]) k++;
            /* A COUNT, not a flag: the driver reports how many disks it
             * brought up, because a machine with a boot disk and a data disk
             * has two and the client has to know that.  This printed
             * `words[0] & 1` while it was a flag and went on printing it after
             * it became a count — so two disks read as "disk 0", which is the
             * same thing as none. */
            b[k++] = (char)('0' + (uint32_t)(m.words[0] % 10u));
            b[k++] = ' '; b[k++] = 's'; b[k++] = 'i'; b[k++] = 'd'; b[k++] = ' ';
            { uint32_t sid = (uint32_t)m.words[2];
              static const char hx[] = "0123456789abcdef";
              b[k++] = '0'; b[k++] = 'x';
              b[k++] = hx[(sid >> 12) & 0xFu]; b[k++] = hx[(sid >> 8) & 0xFu];
              b[k++] = hx[(sid >> 4) & 0xFu];  b[k++] = hx[sid & 0xFu]; }
            b[k++] = ' '; b[k++] = 'd'; b[k++] = 'm'; b[k++] = 'a'; b[k++] = ' ';
            /* "contained" or "open": the difference is the whole of
             * Stage 10-dma, seen from the one driver that most needs it. */
            if (m.words[3]) { b[k++]='c'; b[k++]='o'; b[k++]='n'; b[k++]='t';
                              b[k++]='a'; b[k++]='i'; b[k++]='n'; b[k++]='e';
                              b[k++]='d'; }
            else            { b[k++]='o'; b[k++]='p'; b[k++]='e'; b[k++]='n'; }
            b[k++] = '\n'; b[k] = 0;
            init_log(b);
            return (m.words[0] != 0u) ? 1 : 0;
        }
        init_log("[USER][INIT] blk: no answer\n");
        return 0;
    }
}

/* ── timer spawn (ledger A-24: waiting is a service, not a syscall) ──────── */

/*
 * The kernel cannot block a thread on TIME any more, so somebody in ring 3 has
 * to be able to.  init builds that somebody here, before svcmgr and before
 * anything that waits: a control endpoint it keeps a copy of, the timer IRQ
 * claimed out of the IRQ control capability, the notification that interrupt
 * is routed into, a reply object, a CNode for the client notifications it will
 * be handed, and a budget.
 *
 * Nothing else.  A timer service that can only measure time is the whole
 * point: what a task can wait on is now a capability somebody granted it,
 * where it used to be three syscall numbers every task could reach for.
 *
 * Returns 1 on success, 0 on failure.
 */
int init_spawn_timer(void) {
    iris_cptr_t tm_proc_h = IRIS_CPTR_NULL;
    iris_cptr_t tm_boot_h = IRIS_CPTR_NULL;
    long r;

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_TIMER_EP, 0) < 0) { init_log("[USER] timer: ep\n"); return 0; }
    g_init_timer_ep_h = (iris_cptr_t)INIT_SLOT_TIMER_EP;

    /* The timer INTERRUPT, claimed as a capability out of the IRQ control one
     * — the same path svcmgr uses for every other line.  Line 0 is the tick
     * the kernel also uses for preemption; being told about it is not being
     * given it, which is why the kernel does not mask it for this holder. */
    {
        long ir = iris_invoke((long)IRIS_CPTR_IRQ_CONTROL, INV_BOOT_CREATE_IRQCAP, 0, (long)IRIS_CPTR_INIT_UNTYPED, (long)((uint64_t)INIT_SLOT_TIMER_IRQCAP << 32));
        if (ir != 0) {
            char m[40] = "[USER] timer: irqcap err ";
            uint32_t k = 0; while (m[k]) k++;
            long e = -ir; if (e < 0) e = 0; if (e > 99) e = 99;
            m[k++] = (char)('0' + e / 10); m[k++] = (char)('0' + e % 10);
            m[k++] = '\n'; m[k] = 0;
            init_log(m);
            return 0;
        }
    }

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                         INIT_SLOT_TIMER_NOTIF, 0) < 0) { init_log("[USER] timer: notif\n"); return 0; }
    if (iris_invoke2((long)INIT_SLOT_TIMER_IRQCAP, INV_IRQ_SET_NOTIFICATION, (long)INIT_SLOT_TIMER_NOTIF, 0) != 0) { init_log("[USER] timer: route\n"); return 0; }

    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                         INIT_SLOT_TIMER_REPLY, 0) < 0) { init_log("[USER] timer: reply\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_CNODE,
                         INIT_SLOT_TIMER_CN, (long)TMR_CN_SLOTS) < 0) { init_log("[USER] timer: cn\n"); return 0; }
    if (init_retype_slot(g_init_untyped_c, IRIS_KOBJ_UNTYPED,
                         INIT_SLOT_TIMER_UT, 1 << 20) < 0) { init_log("[USER] timer: ut\n"); return 0; }

    {
        struct svc_mint tm[6] = { 0 };
        uint32_t n = 0;
        tm[n].slot = TMR_SLOT_CTRL_EP;   tm[n].src_cptr = INIT_SLOT_TIMER_EP;
        tm[n].rights = RIGHT_READ;       tm[n].badge = 0; n++;
        tm[n].slot = TMR_SLOT_IRQ_CAP;   tm[n].src_cptr = INIT_SLOT_TIMER_IRQCAP;
        tm[n].rights = RIGHT_READ | RIGHT_ROUTE; tm[n].badge = 0; n++;
        tm[n].slot = TMR_SLOT_IRQ_NOTIF; tm[n].src_cptr = INIT_SLOT_TIMER_NOTIF;
        tm[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT; tm[n].badge = 0; n++;
        tm[n].slot = TMR_SLOT_REPLY;     tm[n].src_cptr = INIT_SLOT_TIMER_REPLY;
        tm[n].rights = RIGHT_READ | RIGHT_WRITE; tm[n].badge = 0; n++;
        tm[n].slot = TMR_SLOT_CN;        tm[n].src_cptr = INIT_SLOT_TIMER_CN;
        tm[n].rights = RIGHT_READ | RIGHT_WRITE; tm[n].badge = 0; n++;
        tm[n].slot = IRIS_CPTR_OWN_UNTYPED; tm[n].src_cptr = INIT_SLOT_TIMER_UT;
        tm[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        tm[n].badge = 0; n++;

        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "timer", &tm_proc_h, &tm_boot_h, tm, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED,
                               /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
    }
    /* The service holds the mints now; init keeps only the control endpoint,
     * which is what it hands on to whoever needs to wait. */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_TIMER_REPLY);
    init_close(&tm_proc_h);
    init_close(&tm_boot_h);
    return r >= 0;
}

/* ── console spawn (Phase 30: ring-3 serial console service) ────────────── */

/* Phase 13 (Track I): console is endpoint-only and CPtr-provisioned — its
 * endpoint recv side (IRIS_CPTR_OWN_EP) and its 0x3F8 UART KIoPort
 * (IRIS_CPTR_IOPORT) are pre-start mints; no legacy console KChannel pair, no
 * bootstrap sends.  Returns 1 on success, 0 on failure. */
int init_spawn_console(void) {
    iris_cptr_t con_proc_h  = IRIS_CPTR_NULL;
    iris_cptr_t con_boot_h  = IRIS_CPTR_NULL;
#define INIT_CONSOLE_IOPORT_SLOT 41u
    uint32_t    ioport_c    = 0u;   /* Phase S4: CPtr slot, not a handle */
    long r;

    /* Console KEndpoint master (init owns it); recv side minted to the child.
     * Phase S1: retyped from init's untyped pool (SYS_ENDPOINT_CREATE retired). */
    r = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_CONSOLE_EP, 0);
    if (r < 0) {
        init_early_serial_write(init_console_chan_fail);
        goto fail;
    }
    g_init_console_ep_h = (iris_cptr_t)INIT_SLOT_CONSOLE_EP;

    /* KIoPort for the 8 UART registers at 0x3F8..0x3FF (IN poll LSR + OUT THR).
     * Phase S4: published into a CSpace slot as an MDB child of the authorising
     * slot, and forwarded to console by CSpace source — so the delegation is
     * revocable from init.  Slot 41 is free in init's root CNode.
     * Stage 5 Step 2: the authority is the ioport control capability. */
    if (iris_invoke((long)IRIS_CPTR_IOPORT_CONTROL, INV_BOOT_CREATE_IOPORT, (long)(0x3F8u | (8u << 16)), (long)IRIS_CPTR_INIT_UNTYPED, (long)((uint64_t)INIT_CONSOLE_IOPORT_SLOT << 32)) != 0) {
        init_early_serial_write(init_console_ioport_fail);
        goto fail;
    }
    ioport_c = INIT_CONSOLE_IOPORT_SLOT;

    /* Phase S1: console serves EP_CALLs, so it needs an explicit reply object.
     * Retype it from init's pool, mint it at IRIS_CPTR_OWN_REPLY, then DROP
     * init's handle — a retained reply cap would suppress the
     * close-wakes-caller path if console dies. */
    iris_cptr_t con_reply_h = IRIS_CPTR_NULL;
    {
        long rr = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_REPLY,
                                   INIT_SLOT_CONSOLE_RPLY, 0);
        if (rr >= 0) con_reply_h = (iris_cptr_t)INIT_SLOT_CONSOLE_RPLY;
        else init_early_serial_write("[INIT] console reply retype FAILED\r\n");
    }

    {
        struct svc_mint con_mints[3] = { 0 };
        uint32_t n = 0;
        con_mints[n].slot   = IRIS_CPTR_OWN_EP;
        con_mints[n].src_cptr = g_init_console_ep_h;
        con_mints[n].rights = RIGHT_READ;
        con_mints[n].badge  = 0;   /* server-side cap: unbadged */
        n++;
        con_mints[n].slot   = IRIS_CPTR_IOPORT;
        con_mints[n].src_cptr = ioport_c;
        con_mints[n].rights = RIGHT_READ | RIGHT_WRITE;
        con_mints[n].badge  = 0;
        n++;
        if (con_reply_h != IRIS_CPTR_NULL) {
            con_mints[n].slot   = IRIS_CPTR_OWN_REPLY;
            con_mints[n].src_cptr = con_reply_h;
            con_mints[n].rights = RIGHT_READ | RIGHT_WRITE;
            con_mints[n].badge  = 0;
            n++;
        }
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "console", &con_proc_h, &con_boot_h,
                               con_mints, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               2u << 20,
                               /* Stage 6/D-5: console owns memory.  A service
                                * holding no Untyped can create nothing — not a
                                * frame, not a page table, not an IPC buffer —
                                * and everything it needs has to be made for it
                                * by somebody else.  Naming the sub-untyped its
                                * address space is already charged to costs
                                * nothing and removes that. */
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED,
                               /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
        init_report_mints("console", con_mints, n);
    }
    /* console's slot-13 mint is the only reply cap: drop ours. */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_CONSOLE_RPLY);
    if (r < 0) {
        init_early_serial_write(init_console_load_fail);
        goto fail;
    }

    /* console holds the slot-10 mint now; init keeps its own slot so it can
     * still revoke the delegation (Phase S4). */
    init_close(&con_proc_h);
    init_close(&con_boot_h);
    return 1;

fail:
    init_close(&con_proc_h);
    init_close(&con_boot_h);
    (void)ioport_c;
    return 0;
}

/* ── svcmgr spawn (Phase 29: ring-3 loader; Phase 30: also sends console) ── */

/* Phase 13 (Track I): init owns svcmgr's discovery endpoint ("svcmgr.ep").  It
 * creates the endpoint, mints the recv+mint side into svcmgr (IRIS_CPTR_OWN_EP)
 * and keeps the send side for its own EP_LOOKUP_NAME calls.  All of svcmgr's
 * bootstrap caps arrive as pre-start CSpace mints (no bootstrap KChannel):
 *   slot 3 (CONSOLE_EP)  — console.ep, WRITE|DUP|TRANSFER (re-mint to children);
 *   slot 5 (OWN_EP)      — svcmgr.ep recv side, READ|WRITE|DUP (recv + re-mint
 *                          IRIS_CPTR_SVCMGR_EP into catalog children);
 *   slot 6 (SPAWN_CAP)   — spawn/authority cap, READ|DUP|TRANSFER.
 * Returns the svcmgr.ep send side (init's discovery handle), or IRIS_CPTR_NULL. */
iris_cptr_t init_spawn_svcmgr(void) {
    iris_cptr_t svcmgr_proc_h  = IRIS_CPTR_NULL;
    iris_cptr_t svcmgr_chan_h  = IRIS_CPTR_NULL;
    iris_cptr_t svcmgr_ep_h    = IRIS_CPTR_NULL;
    long r;

    /* Phase S1: retyped from init's untyped pool (SYS_ENDPOINT_CREATE retired). */
    r = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_ENDPOINT,
                         INIT_SLOT_SVCMGR_EP, 0);
    if (r < 0) goto fail;
    svcmgr_ep_h = (iris_cptr_t)INIT_SLOT_SVCMGR_EP;

    /* Step 4: the SYS_HANDLE_DUP that used to sit here is gone.  It produced a
     * rights-reduced duplicate purely to have a HANDLE to pass as a mint
     * source — and the mint below already reduces to exactly the same rights,
     * so the duplicate never carried authority the mint did not compute
     * itself.  Minting straight from our spawn-cap SLOT drops a handle, drops
     * a syscall, and makes svcmgr's spawn cap an MDB child of that slot, so
     * the delegation is revocable by init instead of handed over outright. */

    /* Phase S1: carve svcmgr's untyped pool (a sub-untyped of init's boot
     * block) — svcmgr retypes every service endpoint / IRQ notification /
     * reply object from it.  Sized for the whole catalog plus per-service
     * reply sub-untypeds and restart churn. */
    iris_cptr_t sm_untyped_h = IRIS_CPTR_NULL;
    {
        /* Stage 6: svcmgr's pool funds everything its subtree consumes, not
         * just its own endpoints and replies — each child's address space and
         * kernel state (Etapas 2-4), the loader's segment and stack VMOs per
         * spawn AND per restart, and vfs's copies of the initrd images
         * (Step 5).  A bump allocator does not rewind, so a restart costs its
         * images again until the pool is RESET; the budget is sized for that
         * rather than pretending the memory is free. */
        static const uint64_t s1_sm_ut_sizes[] =
            { 32u<<20, 16u<<20, 4u<<20, 1u<<20 };
        for (uint32_t szi = 0; szi < 4u && sm_untyped_h == IRIS_CPTR_NULL; szi++) {
            long ur = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_UNTYPED,
                                       INIT_SLOT_SM_UNTYPED, s1_sm_ut_sizes[szi]);
            if (ur >= 0) sm_untyped_h = (iris_cptr_t)INIT_SLOT_SM_UNTYPED;
        }
        if (sm_untyped_h == IRIS_CPTR_NULL)
            init_log("[USER][INIT] svcmgr untyped carve FAILED\n");
    }

    {
        struct svc_mint sm_mints[10] = { 0 };
        uint32_t n = 0;
        sm_mints[n].slot   = IRIS_CPTR_CONSOLE_EP;
        sm_mints[n].src_cptr = g_init_console_ep_h;
        sm_mints[n].rights = RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;   /* unbadged: svcmgr re-mints per child badge */
        n++;
        sm_mints[n].slot   = IRIS_CPTR_OWN_EP;
        sm_mints[n].src_cptr = svcmgr_ep_h;
        /* TRANSFER is required so svcmgr can hand out dup'd "svcmgr.ep" caps via
         * SYS_REPLY cap-transfer (EP_LOOKUP_NAME of "svcmgr.ep"). */
        sm_mints[n].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;   /* unbadged: svcmgr re-mints per child badge */
        n++;
        sm_mints[n].slot     = IRIS_CPTR_PROC_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_PROC_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        /* svcmgr loads service images and forwards image-reading authority to
         * vfs — which, unlike before, does NOT come with the authority to
         * create processes. */
        sm_mints[n].slot     = IRIS_CPTR_INITRD_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_INITRD_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        /* Stage 5 Step 2: svcmgr claims the machine's IRQs and port ranges on
         * behalf of the catalog, so it gets the two control capabilities that
         * authorise exactly that — and, once it has claimed everything, drops
         * them.  They used to be one bit on the capability above. */
        sm_mints[n].slot     = IRIS_CPTR_IRQ_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_IRQ_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        sm_mints[n].slot     = IRIS_CPTR_IOPORT_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_IOPORT_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        /* svcmgr drains the kernel log for its diagnostics endpoint and powers
         * the machine off on request: that is the debug control capability,
         * and nothing else it holds implies it. */
        sm_mints[n].slot     = IRIS_CPTR_DEBUG_CONTROL;
        sm_mints[n].src_cptr = IRIS_CPTR_DEBUG_CONTROL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        sm_mints[n].badge  = 0;
        n++;
        /* Ledger A-24: the authority to WAIT.  svcmgr's idle loop blocks on a
         * death notification, and the kernel no longer has a timeout to hand
         * it — so waiting is a request to a server, and this is the capability
         * that lets it make one. */
        sm_mints[n].slot     = IRIS_CPTR_TIMER_EP;
        sm_mints[n].src_cptr = INIT_SLOT_TIMER_EP;
        sm_mints[n].rights   = RIGHT_WRITE | RIGHT_DUPLICATE;
        sm_mints[n].badge  = 0;
        n++;
        /* Ledger A-21: svcmgr loads services, and loading one means naming
         * its address space.  The POOL travels, the CONTROL does not: svcmgr
         * fills a namespace it was granted, it does not mint new ones. */
        sm_mints[n].slot     = IRIS_CPTR_ASID_POOL;
        sm_mints[n].src_cptr = IRIS_CPTR_ASID_POOL;
        sm_mints[n].rights   = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE;
        sm_mints[n].badge  = 0;
        n++;
        if (sm_untyped_h != IRIS_CPTR_NULL) {
            sm_mints[n].slot   = IRIS_CPTR_OWN_UNTYPED;
            sm_mints[n].src_cptr = sm_untyped_h;
            sm_mints[n].rights = RIGHT_READ | RIGHT_WRITE |
                                 RIGHT_DUPLICATE | RIGHT_TRANSFER;
            sm_mints[n].badge  = 0;
            n++;
        }
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "svcmgr", &svcmgr_proc_h,
                            &svcmgr_chan_h, sm_mints, n,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               8u << 20,
                               /* svcmgr's budget arrives as an explicit
                                * manifest mint above, which the loader's
                                * duplicate guard sees and honours.  Asking for
                                * it here anyway is what says "this child owns
                                * memory", and that is the gate on being given
                                * its own address space and thread (D-6). */
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED,
                               /*keep_cnode_dest=*/0u, /*keep_tcb_dest=*/0u, 0);
        init_report_mints("svcmgr", sm_mints, n);
    }
    /* svcmgr's slot-12 mint keeps the pool alive: drop ours. */
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_SM_UNTYPED);
    if (r < 0) goto fail;

    init_close(&svcmgr_chan_h);   /* bootstrap channel unused — svcmgr is CPtr-only */
    init_close(&svcmgr_proc_h);
    return svcmgr_ep_h;

fail:
    init_close(&svcmgr_proc_h);
    init_close(&svcmgr_chan_h);
    if (svcmgr_ep_h != IRIS_CPTR_NULL) init_close(&svcmgr_ep_h);
    return IRIS_CPTR_NULL;
}

/* ── iris_test spawn + wait ──────────────────────────────────────────────── */

/*
 * Spawns iris_test using spawn_cap_h (a dup of bootstrap_h kept before it is
 * closed; consumed here).  Every capability the suite needs — spawn cap,
 * svcmgr/vfs/console/kbd endpoints, test fixtures — is delivered as a
 * pre-start CSpace mint (table below); no bootstrap-channel sends remain
 * (Phase 13/Track I).  Then waits up to 12 seconds for iris_test to exit and
 * logs the final pass/fail result.
 */
void init_spawn_iris_test(iris_cptr_t sm_h) {
    iris_cptr_t proc_h      = IRIS_CPTR_NULL;
    iris_cptr_t boot_h      = IRIS_CPTR_NULL;
    iris_cptr_t watch_base_h = IRIS_CPTR_NULL; /* death notification (Track B) */
    long r;

    /* Phase 8: the full well-known slot set is pre-start-minted into
     * iris_test (the kind-0x20 bootstrap forward is retired — slot 1 is
     * the only discovery path):
     *   slot 1  — svcmgr discovery ep, RIGHT_WRITE   → T026+/T039/T041
     *   slot 2  — vfs.ep,     RIGHT_WRITE            → T042/T045
     *   slot 3  — console.ep, RIGHT_WRITE            → T043
     *   slot 4  — kbd.ep,     RIGHT_WRITE            → T044
     *   slot 30 — KNotification, RIGHT_WRITE (wrong type) → T040 WRONG_TYPE
     *   slot 31 — svcmgr ep, RIGHT_TRANSFER only     → T040 ACCESS_DENIED
     *             (ACCESS_DENIED is a HARD stop: a resolver that kept
     *              looking after one would answer about another object).
     * Phase 13/Track I: svcmgr.ep/vfs.ep/kbd.ep come from EP_LOOKUP_NAME over
     * init's svcmgr.ep (init holds a supervisor badge → full granted rights,
     * including DUPLICATE for the mint).  Missing caps leave slots empty: the
     * tests FAIL loudly, never skip. */
    /* Step 4: declare a receive slot for every lookup.  A recv that declares
     * none takes the delivery-by-handle path, which is the last IPC producer
     * of handles in the productive tree.  Slots 54..56 are free in init. */
    iris_cptr_t lk_svcmgr = init_ep_lookup_name_slot(sm_h, "svcmgr.ep",
                                                     INIT_RSLOT_LK_SVCMGR);
    iris_cptr_t lk_vfs    = init_ep_lookup_name_slot(sm_h, "vfs.ep",
                                                     INIT_RSLOT_LK_VFS);
    iris_cptr_t lk_kbd    = init_ep_lookup_name_slot(sm_h, "kbd.ep",
                                                     INIT_RSLOT_LK_KBD);
    /* Phase 13/Track I: a KNotification serves as the slot-30 wrong-type fixture
     * for T040 (replaces the retired console KChannel cap).  It carries
     * RIGHT_WRITE so EP_CALL passes the rights check and fails on TYPE
     * (WRONG_TYPE), not ACCESS_DENIED. */
    iris_cptr_t fix_wrongtype = IRIS_CPTR_NULL;
    {
        long nr = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                                   INIT_SLOT_FIX_WRONGTY, 0);
        if (nr >= 0) fix_wrongtype = (iris_cptr_t)INIT_SLOT_FIX_WRONGTY;
    }
    if (lk_svcmgr == IRIS_CPTR_NULL)
        init_log("[USER][INIT] svcmgr.ep lookup FAILED\n");

    /* Phase 18: forward the boot KUntyped (received from userboot at
     * IRIS_CPTR_INIT_UNTYPED) on to iris_test for the ring-3 authority suite.
     * Resolve init's CSpace slot into a mint-source handle; full rights so the
     * suite can retype (WRITE) and revoke.  Absent grant → slot stays empty and
     * T125–T131 FAIL loudly. */
    iris_cptr_t lk_untyped = IRIS_CPTR_NULL;
    {
        /* Phase S1: iris_test receives its OWN sub-untyped (carved from init's
         * pool) instead of a second cap to the shared boot block — the suite
         * can retype/reset it freely without touching init/svcmgr objects.
         * Sized generously for the object-churn suites; smaller fallbacks
         * keep the authority tests alive on small boot blocks. */
        /* Stage 6 Step 2: page tables are charged to the pool of whoever
         * spawns, so the suite — which spawns a hundred-odd children across
         * the lifecycle and pager tests — needs a budget sized for their
         * address spaces (about five tables each), not just for the objects
         * it fabricates.  The ladder keeps the authority tests alive on a
         * small boot block. */
        static const uint64_t s1_test_ut_sizes[] =
            { 96u<<20, 32u<<20, 8u<<20, 2u<<20 };
        /* Stage 6: carve the suite's budget from the SECOND boot block when
         * userboot handed one over, so the suite and svcmgr do not compete for
         * the same block now that every address space, process and VMO page
         * is charged to somebody's budget. */
        uint64_t test_ut_src = IRIS_CPTR_INIT_UNTYPED2;
        if (iris_invoke2((long)test_ut_src, INV_UNTYPED_INFO, 0, 0) != 0)
            test_ut_src = g_init_untyped_c;
        for (uint32_t szi = 0; szi < 4u && lk_untyped == IRIS_CPTR_NULL; szi++) {
            long ur = init_retype_slot(test_ut_src, IRIS_KOBJ_UNTYPED,
                                       INIT_SLOT_TEST_UNTYPED, s1_test_ut_sizes[szi]);
            if (ur >= 0) lk_untyped = (iris_cptr_t)INIT_SLOT_TEST_UNTYPED;
        }
        if (lk_untyped == IRIS_CPTR_NULL)
            init_log("[USER][INIT] test untyped carve FAILED\n");
    }

    {
        /* Phase 9: slots 1-4 carry IRIS_BADGE_IRIS_TEST so every server can
         * verify who is calling; slot 28 is a SECOND cap to the svcmgr
         * endpoint with a different badge (T053: two caps, same endpoint,
         * different identities). */
        struct svc_mint it_mints[26] = { 0 };
        it_mints[0].slot = IRIS_CPTR_SVCMGR_EP;
        it_mints[0].src_h = lk_svcmgr;
        it_mints[0].rights = RIGHT_WRITE;
        it_mints[0].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[1].slot = IRIS_CPTR_VFS_EP;
        it_mints[1].src_h = lk_vfs;
        it_mints[1].rights = RIGHT_WRITE;
        it_mints[1].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[2].slot = IRIS_CPTR_CONSOLE_EP;
        it_mints[2].src_cptr = g_init_console_ep_h;
        it_mints[2].rights = RIGHT_WRITE;
        it_mints[2].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[3].slot = IRIS_CPTR_KBD_EP;
        it_mints[3].src_h = lk_kbd;
        it_mints[3].rights = RIGHT_WRITE;
        it_mints[3].badge = IRIS_BADGE_IRIS_TEST;
        it_mints[4].slot = IRIS_CPTR_TEST_FIX_A;
        it_mints[4].src_cptr = fix_wrongtype;          /* wrong type (KNotification, not endpoint) */
        /* READ|WRITE so the probes that use it fail on TYPE, not rights —
         * which is the whole reason this fixture exists.  It carried WRITE
         * alone, chosen when the only probe was EP_CALL; SYS_FRAME_MAP asks
         * for READ as well, so a "wrong type" assertion was being answered
         * ACCESS_DENIED and passing for the wrong reason elsewhere. */
        it_mints[4].rights = RIGHT_READ | RIGHT_WRITE;
        it_mints[4].badge = 0;
        it_mints[5].slot = IRIS_CPTR_TEST_FIX_B;
        it_mints[5].src_h = lk_svcmgr;                 /* TRANSFER only */
        it_mints[5].rights = RIGHT_TRANSFER;
        it_mints[5].badge = 0;
        it_mints[6].slot = IRIS_CPTR_TEST_FIX_C;
        it_mints[6].src_h = lk_svcmgr;                 /* badge B fixture */
        it_mints[6].rights = RIGHT_WRITE;
        it_mints[6].badge = IRIS_BADGE_TEST_B;
        /* Phase 10: supervisor-badged svcmgr cap so iris_test can drive the
         * privileged RESTART path (real death→respawn E2E, T057/T060). */
        it_mints[7].slot = IRIS_CPTR_TEST_SUPER;
        it_mints[7].src_h = lk_svcmgr;
        it_mints[7].rights = RIGHT_WRITE;
        it_mints[7].badge = IRIS_BADGE_INIT;
        /* Phase 13: an authority cap in a CPtr slot — iris_test invokes it by
         * CPtr to prove device authority resolves via CSpace (T069).
         * Stage 5 Step 2: that cap is the ioport CONTROL capability now, so
         * the test names something that authorises exactly one syscall. */
        it_mints[8].slot = IRIS_CPTR_IOPORT_CONTROL;
        it_mints[8].src_cptr = IRIS_CPTR_IOPORT_CONTROL;
        /* Stage 5: DUPLICATE as well as READ, because the suite has to be able
         * to DERIVE a narrowed control capability — that is what replaced the
         * kernel's port whitelist, and a test that cannot narrow cannot check
         * that narrowing confines.  DUPLICATE is the right that governs making
         * a second capability carrying the same authority, here as everywhere
         * else; a delegate handed one without it may use its range and may not
         * subdivide it. */
        it_mints[8].rights = RIGHT_READ | RIGHT_DUPLICATE;
        it_mints[8].badge = 0;
        /* Phase 13 (Track I): the suite's operational authorities are pre-start
         * mints — no bootstrap KChannel send.  Stage 5 Step 2: they are three
         * capabilities, because the suite does three different things with
         * them (spawn children, read boot images, probe the framebuffer). */
        it_mints[9].slot = IRIS_CPTR_PROC_CONTROL;
        it_mints[9].src_cptr = IRIS_CPTR_PROC_CONTROL;
        it_mints[9].rights = RIGHT_READ;
        it_mints[9].badge = 0;
        /* Stage 5 Step 2: the suite creates IRQ capabilities in several
         * tests and asserts that each control capability authorises ONLY its
         * own syscall (T296).  The ioport half arrives at index 8 above. */
        it_mints[13].slot = IRIS_CPTR_IRQ_CONTROL;
        it_mints[13].src_cptr = IRIS_CPTR_IRQ_CONTROL;
        it_mints[13].rights = RIGHT_READ;
        it_mints[13].badge = 0;
        /* The suite reads scheduler and IPC statistics in ~20 tests; that is
         * debug authority and now says nothing about spawning. */
        it_mints[14].slot = IRIS_CPTR_DEBUG_CONTROL;
        it_mints[14].src_cptr = IRIS_CPTR_DEBUG_CONTROL;
        it_mints[14].rights = RIGHT_READ;
        it_mints[14].badge = 0;
        it_mints[15].slot = IRIS_CPTR_INITRD_CONTROL;
        it_mints[15].src_cptr = IRIS_CPTR_INITRD_CONTROL;
        it_mints[15].rights = RIGHT_READ;
        it_mints[15].badge = 0;
        /* T168 asserts the framebuffer VMO is one-shot: it needs the
         * capability that authorises the call, or it would be asserting a
         * denial instead. */
        it_mints[16].slot = IRIS_CPTR_FB_CONTROL;
        it_mints[16].src_cptr = IRIS_CPTR_FB_CONTROL;
        it_mints[16].rights = RIGHT_READ;
        it_mints[16].badge = 0;
        /* Phase 18: the boot KUntyped for the authority suite (T125–T131). */
        it_mints[10].slot = IRIS_CPTR_TEST_UNTYPED;
        it_mints[10].src_cptr = lk_untyped;   /* 0 → skipped by svc_load */
        it_mints[10].rights = RIGHT_READ | RIGHT_WRITE |
                              RIGHT_DUPLICATE | RIGHT_TRANSFER;
        it_mints[10].badge = 0;
        /* Phase 28.1: the supervisor-side file-grant caps for iris_test (the
         * pager supervisor in the runtime suite).  Two slots, because a badged
         * cap can never be re-badged:
         *   slot 58 — the grant ADMIN identity: call-only (WRITE) vfs.ep cap
         *             badged IRIS_BADGE_FILEGRANT_ADMIN.  Drives GRANT_OPEN /
         *             GRANT_REVOKE / GRANT_SESSION_RESET at the VFS.
         *   slot 59 — the session-cap MINT SOURCE: an UNBADGED vfs.ep cap with
         *             WRITE|DUPLICATE|TRANSFER.  Fresh session badges
         *             (IRIS_BADGE_FILEGRANT_S(s)) are minted from it into each
         *             pager instance; invoked directly it is an ordinary
         *             unbadged client (no grant authority).
         * lk_vfs came from init's own supervisor-badged lookup
         * (WRITE|DUPLICATE|TRANSFER); the ordinary client lookup strips
         * DUPLICATE, so this pre-mint is the only honest source.
         * IRIS_CPTR_NULL (lookup miss) → svc_load skips it, and the
         * file-backed suite gates loudly. */
        it_mints[11].slot  = IRIS_CPTR_TEST_VFS_DUP;
        it_mints[11].src_h = lk_vfs;
        it_mints[11].rights = RIGHT_WRITE;
        it_mints[11].badge = IRIS_BADGE_FILEGRANT_ADMIN;
        it_mints[12].slot  = IRIS_CPTR_TEST_VFS_MINT;
        it_mints[12].src_h = lk_vfs;
        it_mints[12].rights = RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        it_mints[12].badge = 0;
        /* Ledger D-9: the DEVICE untyped, so the suite can exercise the path
         * that hands MMIO over as a capability.  Delegated rather than
         * duplicated — init keeps the parent, so revoking reaches it. */
        /* Ledger A-20: authority over CPU time.  The suite configures
         * scheduling contexts (T083, T267, T308, T309, T315) and without this
         * it cannot — which is the point: a budget is granted, not taken. */
        it_mints[18].slot = IRIS_CPTR_SCHED_CONTROL;
        it_mints[18].src_cptr = IRIS_CPTR_SCHED_CONTROL;
        it_mints[18].rights = RIGHT_READ | RIGHT_DUPLICATE;
        it_mints[18].badge = 0;
        /* The domain authority, so T343 can drive a time partition — and so
         * that the negative half is testable too: without this mint the same
         * invocation must answer ACCESS_DENIED.
         *
         * It lands at a DIFFERENT slot than everyone else gets it at.  The
         * service-wide constant is 97, which is free in init and in every
         * service — and is the last of iris_test's fixed reply-object range
         * (88..97), which T113 deletes.  The suite's slot map is the crowded
         * one, so the suite names its own; 99 is on its documented free list. */
        it_mints[21].slot = IRIS_CPTR_DOMAIN_CONTROL_TEST;
        it_mints[21].src_cptr = IRIS_CPTR_DOMAIN_CONTROL;
        it_mints[21].rights = RIGHT_READ | RIGHT_DUPLICATE;
        it_mints[21].badge = 0;
        /*
         * And the authority over what a DEVICE may reach (Stage 10-dma), at a
         * slot of the suite's own for the reason the one above has one.
         *
         * The suite is given it deliberately: Stage 10-dma's claim is that a
         * device reaches only what somebody mapped for it, and a test that
         * cannot bind an IOSpace can only check that the hardware is switched
         * on — a statement about the kernel's boot, not its capability model.
         */
        it_mints[22].slot = IRIS_CPTR_IOSPACE_CONTROL_TEST;
        it_mints[22].src_cptr = IRIS_CPTR_IOSPACE_CONTROL;
        it_mints[22].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE;
        it_mints[22].badge = 0;
        /* Ledger A-21: the suite builds address spaces (T079, T328) and has
         * to be able to name them.  It receives the POOL and not the CONTROL,
         * so T328 can also assert that carving a pool without ASIDControl is
         * refused — the negative half of the same grant. */
        /* Ledger A-24: the suite's bounded waits are requests to the timer
         * service now — every one of them, which is why this mint is not
         * optional for it. */
        it_mints[20].slot = IRIS_CPTR_TIMER_EP;
        it_mints[20].src_cptr = INIT_SLOT_TIMER_EP;
        it_mints[20].rights = RIGHT_WRITE | RIGHT_DUPLICATE;
        it_mints[20].badge = 0;
        it_mints[19].slot = IRIS_CPTR_ASID_POOL;
        it_mints[19].src_cptr = IRIS_CPTR_ASID_POOL;
        it_mints[19].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE;
        it_mints[19].badge = 0;
        it_mints[17].slot = IRIS_CPTR_DEVICE_UNTYPED;
        it_mints[17].src_cptr = IRIS_CPTR_DEVICE_UNTYPED;
        it_mints[17].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE;
        it_mints[17].badge = 0;
        /* Stage 10: the bus service, so the suite's driver test can ask for
         * its device's window instead of carving one out of a region it would
         * then be sharing with `pci`.  WRITE because a client of an endpoint
         * sends on it; DUPLICATE so the test can derive a narrowed copy and
         * check that a narrowed one is refused. */
        it_mints[23].slot = IRIS_CPTR_PCI_EP;
        it_mints[23].src_cptr = INIT_SLOT_PCI_EP;
        it_mints[23].rights = RIGHT_WRITE | RIGHT_DUPLICATE;
        it_mints[23].badge = 0;
        /* Stage 10: the firmware's tables, so the suite can prove ring 3 can
         * read them.  A device Untyped like any other — it pays for its object
         * headers out of RAM the holder names. */
        it_mints[24].slot = IRIS_CPTR_ACPI_UNTYPED_TEST;
        it_mints[24].src_cptr = IRIS_CPTR_ACPI_UNTYPED;
        it_mints[24].rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE;
        it_mints[24].badge = 0;
        /* Stage 10: the disk, so the suite can prove the bytes a ring-3 driver
         * read are the bytes that are on it. */
        it_mints[25].slot = IRIS_CPTR_BLK_EP_TEST;
        it_mints[25].src_cptr = INIT_SLOT_BLK_EP;
        it_mints[25].rights = RIGHT_WRITE;
        it_mints[25].badge = 0;
        /* Step 4: the loader authority is our spawn-cap SLOT.  SYS_INITRD_VMO
         * and SYS_PROCESS_CREATE both resolve it either way, and the slot
         * outlives bootstrap_h by construction — which is the only reason the
         * retired duplicate had to exist. */
        r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL,
                               "iris_test",
                            &proc_h, &boot_h, it_mints, 26u,
                               SVC_LOADER_WS(g_init_untyped_c, INIT_SLOT_LOADER_WS),
                               16u << 20, /*own_budget_slot=*/0, /* has TEST_UNTYPED */
                               /* Stage 7 Step 9: keep the suite's CSpace root
                                * long enough for the self-proc mint below. */
                               (uint64_t)INIT_SLOT_TEST_CNODE << 32,
                               (uint64_t)INIT_SLOT_TEST_TCB << 32, 0);
        init_report_mints("iris_test", it_mints, 26u);
    }
    init_close(&lk_svcmgr);
    init_close(&lk_vfs);
    init_close(&lk_kbd);
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_TEST_UNTYPED);
    if (fix_wrongtype != 0u)
        (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_FIX_WRONGTY);
    if (r < 0) {
        init_log("[USER][INIT] iris_test load FAILED\n");
        goto out;
    }

    /* A1 Increment 1: mint iris_test's OWN process cap (RIGHT_WRITE) into its
     * CSpace (slot 25).  The source only exists after the load, hence
     * post-start.
     *
     * Stage 7 Step 9: through the child's ROOT CSPACE, which the spawn kept at
     * INIT_SLOT_TEST_CNODE.  It used to name the child's PROCESS and let the
     * kernel read `child->cspace_root` out of it — reaching a CSpace init did
     * not hold by naming something it did.  init drops the root right after,
     * because keeping it is standing authority over the suite's namespace and
     * this is the only thing it needed it for. */
    if (iris_invoke2((long)proc_h, INV_CSPACE_MINT, (long)((uint64_t)INIT_SLOT_TEST_CNODE |
                         ((uint64_t)IRIS_CPTR_TEST_PROC << 32)), (long)RIGHT_WRITE) != 0)
        init_log("[USER][INIT] iris_test self-proc mint FAILED\n");
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_TEST_CNODE);

    /* Phase 13 (Track I): the iris_test spawn cap is delivered as the
     * IRIS_CPTR_SPAWN_CAP pre-start mint above — no KChannel SPAWN_CAP send. */
    init_close(&boot_h);

    /* Phase 13 (Track B): process-exit watch is delivered as a KNotification
     * signal.  One notification (full rights) serves both the watch arm
     * (RIGHT_WRITE) and our own wait (RIGHT_WAIT); bit 0 marks iris_test. */
    /* Phase S1: retyped from init's untyped pool (SYS_NOTIFY_CREATE retired). */
    r = init_retype_slot(g_init_untyped_c, IRIS_KOBJ_NOTIFICATION,
                         INIT_SLOT_WATCH_NOTIF, 0);
    if (r < 0) goto out;
    watch_base_h = (iris_cptr_t)INIT_SLOT_WATCH_NOTIF;

    /* Stage 7 Step 10: wait on the THREAD iris_test was started with. */
    r = iris_invoke2((long)INIT_SLOT_TEST_TCB, INV_TCB_WATCH, (long)watch_base_h, 1);
    if (r < 0) {
        init_log("[USER][INIT] iris_test watch FAILED\n");
        goto out;
    }

    /*
     * Wait for iris_test to exit, with a bound.
     *
     * Ledger A-24: the bound is a request to the TIMER SERVICE, because the
     * kernel cannot block on time any more.  A derived copy of the watch
     * notification is handed over and the timeout arrives on it as a reserved
     * bit, told apart from the exit signal the watch raises.
     *
     * Ledger A-29: transfer is a COPY, so INIT_SLOT_TIMER_GIVE still holds
     * init's own capability afterwards.  It is left there deliberately — this
     * is init's last wait before it parks, the slot is reserved for exactly
     * this, and holding the parent of the grant is what would let init revoke
     * the timer's reach if it ever needed to.
     */
    {
        uint64_t bits = 0;
        long give = iris_invoke2((long)watch_base_h, INV_CSPACE_MINT, (long)(((uint64_t)INIT_SLOT_TIMER_GIVE << 32) | 0u), (long)(RIGHT_WRITE | RIGHT_TRANSFER));
        uint64_t tok = 0;
        if (give == 0)
            (void)iris_timer_arm((long)INIT_SLOT_TIMER_EP,
                                 (long)INIT_SLOT_TIMER_GIVE, IRIS_TIMER_BIT,
                                 INIT_TEST_WATCHDOG_NS, &tok);
        for (;;) {
            r = iris_invoke1((long)watch_base_h, INV_NOTIFY_WAIT, (long)&bits);
            if (r != 0) break;
            if (bits & ~IRIS_TIMER_BIT) { r = 0; break; }
            if (bits & IRIS_TIMER_BIT)  { r = (long)IRIS_ERR_TIMED_OUT; break; }
        }
        if (r == 0 && tok) (void)iris_timer_cancel((long)INIT_SLOT_TIMER_EP, tok);
    }
    if (r < 0) {
        init_log("[USER][INIT] iris_test wait TIMEOUT\n");
    } else {
        long ec = iris_invoke0((long)INIT_SLOT_TEST_TCB, INV_TCB_EXIT_CODE);
        if (ec == 0)
            init_log("[USER][INIT] iris_test PASS\n");
        else
            init_log("[USER][INIT] iris_test FAIL\n");
    }

out:
    init_close(&proc_h);
    init_close(&boot_h);
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)INIT_SLOT_WATCH_NOTIF);
    /* Step 4: nothing to close — the loader authority was our own CSpace slot,
     * not a duplicate this function owned and had to release. */
}
