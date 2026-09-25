/*
 * console/main.c — ring-3 serial console service.
 *
 * Bootstrap deliveries from svc_loader:
 *   recv SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP → ioport_h (KIoPort for 0x3F8..0x3FF)
 *   recv SVCMGR_BOOTSTRAP_KIND_SERVICE_EP → ep_h (KEndpoint recv, Phase 7.3)
 *
 * Main loop: endpoint-only. Drain EP requests (CONSOLE_EP_OP_WRITE / SYNC /
 * PING — iris/console_ep_proto.h). EP WRITE replies only after the bytes hit
 * the UART; EP SYNC is an explicit flush barrier. The legacy KChannel write
 * path (CONSOLE_MSG_WRITE/SYNC) is retired, header deleted, and no longer
 * served — every writer, including svcmgr's klog drain, uses console.ep
 * (Phase 13/Track G).
 */

#include <stdint.h>
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/console_ep_proto.h>
#include <iris/ipc_msg.h>
#include <iris/nc/error.h>
#include <iris/font8x8.h>
#include <iris/fb_info.h>
#include <iris/paging.h>
#include "../common/iris_map.h"
#include "../common/iris_vspace.h"
#include "../common/iris_ipc_buffer.h"

/*
 * Poll the UART Line Status Register (offset 5) until bit 5 (THRE) is set,
 * then write one byte to the Transmit Holding Register (offset 0).
 *
 * ── Why the wait is bounded ────────────────────────────────────────────────
 *
 * It was `do { } while (!THRE)`, which is correct against a UART that is
 * there and fatal against one that is not.  Under QEMU there is always a
 * working 16550 at 0x3F8; on a real machine there may be nothing, or a port
 * that decodes and never drains.  A floating ISA bus reads back 0xFF, and
 * 0xFF happens to have bit 5 set, so the common case of "no serial port at
 * all" escapes the loop by luck rather than by design -- and the case that
 * does not escape wedges this service forever, which wedges every task
 * waiting on it, which is the whole boot.
 *
 * The bound is generous: a 16550 at 115200 baud empties its holding register
 * in under a hundred microseconds, so a spin this long means the port is not
 * coming back.  A byte is then DROPPED rather than waited on, because a log
 * that loses a character is a diagnostic and a log that hangs is an outage.
 */
#define CON_UART_SPIN 200000u
#define CON_UART_MS      250u   /* the real deadline under the count */

static void con_uart_write_byte(iris_cptr_t ioport_h, uint8_t byte) {
    long v;
    long t0 = 0;
    for (uint32_t spin = 0; spin < CON_UART_SPIN; spin++) {
        v = iris_invoke1((long)ioport_h, INV_IOPORT_IN, 5);
        if (v >= 0 && ((uint8_t)v & 0x20u)) {
            (void)iris_invoke2((long)ioport_h, INV_IOPORT_OUT, 0, (long)byte);
            return;
        }
        /*
         * A real deadline underneath the count, checked rarely.
         *
         * The count alone is machine-dependent, like every other count this
         * sweep has replaced -- but this one is on the path EVERY ring-3 line
         * is logged through, and a clock reading per byte would make the whole
         * boot pay for it.  Every four thousand-odd reads is free by
         * comparison and still bounds the wait in real time.
         *
         * The margin is deliberate: a 16550 at 115200 baud empties its holding
         * register in under a hundred microseconds, so a wait this long means
         * the port is not coming back.
         */
        if ((spin & 0xFFFu) == 0xFFFu) {
            long now = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
            if (now > 0) {
                if (t0 == 0) t0 = now;
                else if ((uint64_t)(now - t0) > (uint64_t)CON_UART_MS * 1000000ull)
                    return;                     /* the byte is dropped */
            }
        }
    }
    /* Gone.  Say nothing and keep serving: there is nowhere to report it to. */
}

/* Phase 13 (Track I): the legacy KChannel write path (con_serve_chan_msg /
 * con_drain_chan, CONSOLE_MSG_WRITE/SYNC) is retired — console is endpoint-only.
 * Its sole writers (svcmgr klog drain, init logging) now use console.ep. */

/*
 * The request buffer, and the two forms it can take (ledger D-4).
 *
 * `g_con_ep_buf` is the fallback: 256 bytes of BSS matched to the kernel's own
 * staging, which is what a thread with no registered IPC buffer gets.  At
 * startup console retypes a page from the Untyped it owns and registers it,
 * and `g_con_buf` points there instead — the kernel then writes incoming
 * payloads straight into that page and no user pointer is named on either
 * side.
 *
 * The pointer indirection is the whole migration: everything below reads
 * `g_con_buf` and `g_con_buf_cap`, so the two paths differ in one assignment
 * at startup rather than in every use.
 */
static uint8_t  g_con_ep_buf[IRIS_IPC_BUF_SIZE];
static uint8_t *g_con_buf     = g_con_ep_buf;
static uint32_t g_con_buf_cap = IRIS_IPC_BUF_SIZE;

/* Spare slots from the per-service range (22..29 are unassigned). */
#define CON_SLOT_IPCBUF_FRAME  22u
#define CON_SLOT_IPCBUF_PT     23u
#define CON_SLOT_FB_FRAME      24u
#define CON_SLOT_FB_PT         25u

static void con_ipc_buffer_init(void) {
    void *b = iris_ipc_buffer_init(CON_SLOT_IPCBUF_FRAME, CON_SLOT_IPCBUF_PT,
                                   IRIS_IPC_BUFFER_VA);
    if (!b) return;                 /* keep the staging path; not fatal */
    g_con_buf     = (uint8_t *)b;
    g_con_buf_cap = 4096u;
}

static void con_imsg_zero(struct iris_msg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

/*
 * ---- The screen -----------------------------------------------------------
 *
 * This service already receives every byte anything in ring 3 logs, and it
 * used to have exactly one place to put them: the UART at 0x3F8.  Most real
 * machines have no such port, so on one of those everything from here up --
 * which disk the block driver found, whether the filesystem mounted, whether a
 * frame ever left the network card -- went nowhere at all.
 *
 * The kernel paints its own boot log (`fbcon`), but it stops at ring 3 by
 * design: two writers on one screen produce a screen that describes neither.
 * So the handover lands here, in the service whose job was always output, and
 * the glyphs come from the same <iris/font8x8.h> the kernel used, because a
 * second copy of that table is the one way the two halves of one screen could
 * start disagreeing.
 *
 * Asking where the framebuffer is IS the handover: the kernel goes quiet when
 * somebody with FB_CONTROL asks, so by the time anything is painted here the
 * other writer has stopped.
 */
#define CON_FB_VA  USER_VMO_BASE
#define CON_FG     0xFFFFFFFFu
#define CON_BG     0x00000000u

static volatile uint32_t *g_scr;
static uint32_t g_scr_stride, g_scr_w, g_scr_h, g_scr_cols, g_scr_rows;
static uint32_t g_scr_col, g_scr_row;

/*
 * The top line belongs to the kernel and this service does not touch it.
 *
 * `fbcon` paints the boot markers there -- K F S B P G g, one per point where
 * the boot can die -- on a line that never scrolls, because what is on that
 * line when a machine stops IS the diagnosis.  Clearing it on the way in threw
 * away the only record of the half of the boot that happens before this
 * service exists.
 */
#define CON_SCR_ROW0 1u

static void con_scr_fill_row(uint32_t row, uint32_t colour) {
    uint32_t y0 = row * FONT8X8_H;
    for (uint32_t y = y0; y < y0 + FONT8X8_H && y < g_scr_h; y++)
        for (uint32_t x = 0; x < g_scr_w; x++)
            g_scr[(uint64_t)y * g_scr_stride + x] = colour;
}

/*
 * At the bottom, start a new page instead of scrolling.
 *
 * Scrolling READS the framebuffer, and reading a framebuffer is the most
 * expensive thing that can be done to one: it is write-combining memory, where
 * a read costs orders of magnitude more than a write.  A scroll moves the whole
 * text area -- some megabytes at any resolution worth having -- and this
 * service paints every line anything in ring 3 logs, which on a boot with a
 * test suite in it is hundreds of lines.
 *
 * It was implemented as a scroll first, and the cost was not theoretical: the
 * boot slowed enough that the runner killed the machine with the suite still
 * running.  A page flip is writes only, and there is one per screenful instead
 * of one per line.
 *
 * What it costs is the lines above the flip, which is why a caller that has
 * something it wants read in one piece sends a form feed first (see below).
 */
static void con_scr_page(void) {
    for (uint32_t r = CON_SCR_ROW0; r < g_scr_rows; r++) con_scr_fill_row(r, CON_BG);
    g_scr_row = CON_SCR_ROW0;
    g_scr_col = 0;
}

static void con_scr_putc(char c) {
    if (!g_scr) return;
    if (c == '\r') { g_scr_col = 0; return; }
    /* Form feed: a clean page.  It is how a caller says "this next block is
     * meant to be read whole" -- without it a seven-line report can straddle
     * a page flip and lose its first half. */
    if (c == '\f') { con_scr_page(); return; }
    if (c == '\n') {
        g_scr_col = 0;
        if (++g_scr_row >= g_scr_rows) con_scr_page();
        return;
    }
    if (g_scr_col < g_scr_cols && g_scr_row < g_scr_rows) {
        const uint8_t *gl = font8x8_glyph((uint8_t)c);
        uint32_t x0 = g_scr_col * FONT8X8_W, y0 = g_scr_row * FONT8X8_H;
        for (uint32_t gy = 0; gy < FONT8X8_H; gy++) {
            uint32_t y = y0 + gy;
            if (y >= g_scr_h) break;
            uint8_t bits = gl[gy];
            for (uint32_t gx = 0; gx < FONT8X8_W; gx++) {
                uint32_t x = x0 + gx;
                if (x >= g_scr_w) break;
                g_scr[(uint64_t)y * g_scr_stride + x] =
                    ((bits >> gx) & 1u) ? CON_FG : CON_BG;
            }
        }
    }
    if (++g_scr_col >= g_scr_cols) {
        g_scr_col = 0;
        if (++g_scr_row >= g_scr_rows) con_scr_page();
    }
}

/*
 * Claim the framebuffer and map it.  Best effort, and silent on failure: a
 * machine with no framebuffer is one where the UART is the only output, which
 * is the arrangement this service had before and still works.
 */
static void con_screen_init(void) {
    struct iris_fb_params p;
    { uint8_t *raw = (uint8_t *)&p;
      for (uint32_t i = 0; i < (uint32_t)sizeof(p); i++) raw[i] = 0; }

    if (iris_invoke2((long)IRIS_CPTR_FB_CONTROL, INV_BOOT_FRAMEBUFFER_INFO,
                     (long)(uintptr_t)&p, 0) != 0) return;
    if (p.width == 0 || p.height == 0 || p.size == 0 || p.bpp != 4u) return;
    if (p.stride < p.width) return;

    /* The headers of what comes out of MMIO are charged to this service's own
     * budget: a device region cannot pay for the objects describing it. */
    (void)iris_invoke2((long)IRIS_CPTR_DEVICE_UNTYPED,
                       INV_UNTYPED_SET_DEVICE_BUDGET, (long)IRIS_CPTR_OWN_UNTYPED, 0);

    uint64_t bytes = (p.size + 0xFFFu) & ~0xFFFULL;
    if (iris_invoke((long)IRIS_CPTR_DEVICE_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)CON_SLOT_FB_FRAME << 32), (long)bytes) != 0)
        return;
    if (iris_map_frame(CON_SLOT_FB_FRAME, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, CON_SLOT_FB_PT,
                       CON_FB_VA, bytes, 1ull) != 0)
        return;

    g_scr        = (volatile uint32_t *)(uintptr_t)CON_FB_VA;
    g_scr_stride = p.stride;
    g_scr_w      = p.width;
    g_scr_h      = p.height;
    g_scr_cols   = g_scr_w / FONT8X8_W;
    g_scr_rows   = g_scr_h / FONT8X8_H;
    g_scr_col    = 0;
    g_scr_row    = CON_SCR_ROW0;

    /* Clear everything BELOW the marker line: the kernel's boot log has been
     * read by anyone watching, and starting from a blank area is what makes
     * the ring-3 log readable rather than interleaved with what came before.
     * Row 0 survives, because it is the record of how far the boot got before
     * this service existed to say anything. */
    for (uint32_t r = CON_SCR_ROW0; r < g_scr_rows; r++) con_scr_fill_row(r, CON_BG);
}

static void con_ep_reply_err(struct iris_msg *reply, int32_t err) {
    con_imsg_zero(reply);
    reply->label      = IRIS_EP_REPLY_ERR;
    reply->words[0]   = (uint64_t)(uint32_t)err;
    reply->word_count = 1u;
}

/* Serve one endpoint request; exactly one reply per request. */
static void con_serve_ep_msg(iris_cptr_t ioport_h, struct iris_msg *req) {
    struct iris_msg reply;
    iris_cptr_t reply_h = (iris_cptr_t)req->got_cap;

    switch (req->label) {
    case CONSOLE_EP_OP_WRITE: {
        uint32_t len = req->buf_len;
        if (len > g_con_buf_cap) len = g_con_buf_cap;
        for (uint32_t i = 0; i < len; i++) {
            con_uart_write_byte(ioport_h, g_con_buf[i]);
            con_scr_putc((char)g_con_buf[i]);
        }
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        reply.word_count = 1u;
        break;
    }
    case CONSOLE_EP_OP_SYNC:
        if (req->buf_len > 0u) {
            con_ep_reply_err(&reply, IRIS_ERR_INVALID_ARG);
            break;
        }
        /* Phase 13 (Track I): no legacy KChannel writers remain — EP writes are
         * synchronous by construction, so SYNC is a trivial acknowledge. */
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        reply.word_count = 1u;
        break;
    case IRIS_EP_OP_PING:
        con_imsg_zero(&reply);
        reply.label      = IRIS_EP_REPLY_OK;
        /* Phase 9 PING convention: echo the kernel-stamped sender badge. */
        reply.words[1]   = req->sender_badge;
        reply.word_count = 2u;
        break;
    default:
        con_ep_reply_err(&reply, IRIS_ERR_NOT_SUPPORTED);
        break;
    }

    /* Phase S1: reply_h is the console's OWN reply-object CPtr (echoed by the
     * kernel from the recv arg2).  The object is reusable — nothing to close. */
    if (reply_h != IRIS_CPTR_NULL)
        (void)iris_msg_reply((long)reply_h, &reply);
}

void console_main_c(iris_cptr_t rbx_unused) {
    /* Phase 13 (Track I): console is endpoint-only and fully CPtr-provisioned —
     * the endpoint recv side is the IRIS_CPTR_OWN_EP mint (slot 5) and the
     * KIoPort for 0x3F8..0x3FF the IRIS_CPTR_IOPORT mint (slot 10), named by
     * CPtr like everything else (INV_IOPORT_IN/OUT).  No bootstrap KChannel
     * recv, no legacy service channel. */
    iris_cptr_t ioport_h = (iris_cptr_t)IRIS_CPTR_IOPORT;
    iris_cptr_t ep_h     = (iris_cptr_t)IRIS_CPTR_OWN_EP;

    (void)rbx_unused;   /* RBX = 0 since the KChannel bootstrap retired */

    /* D-4: a page console owns, registered as its IPC buffer.  Best-effort —
     * failing leaves the kernel staging path, which still works. */
    con_ipc_buffer_init();

    /* The screen, if this machine gave one and this service was granted it.
     * Silent when either is absent: the UART path is unchanged and a machine
     * with no framebuffer simply keeps the arrangement it had. */
    con_screen_init();

    /* Endpoint-only main loop: block on the KEndpoint, serve, reply.
     * Phase S1: the explicit reply object (init retypes it from its untyped
     * pool and mints it at IRIS_CPTR_OWN_REPLY) rides in recv arg2. */
    for (;;) {
        struct iris_msg req;
        con_imsg_zero(&req);
        if ((req.reply = (long)IRIS_CPTR_OWN_REPLY, iris_msg_recv((long)ep_h, &req)) != IRIS_OK)
            continue;
        con_serve_ep_msg(ioport_h, &req);
    }
}
