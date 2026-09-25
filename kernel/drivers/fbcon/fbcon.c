/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fbcon.c — the kernel's console of last resort, on the screen.
 *
 * The contract, and the reason a kernel in this tree is allowed to paint at
 * all, is in `iris/fbcon.h`.  This file is the implementation.
 *
 * ── Addressing ────────────────────────────────────────────────────────────
 *
 * The framebuffer is written through its PHYSICAL address, and that is
 * correct at every point this code can run.  Before `paging_init` the
 * firmware's identity map is still loaded; after it, `paging_init` identity
 * maps the framebuffer region itself (writable, NX).  So one pointer is valid
 * across the switch, which is what lets a marker be painted before paging
 * exists and a panic be painted long after.
 *
 * ── Cost ──────────────────────────────────────────────────────────────────
 *
 * Scrolling reads the framebuffer, and framebuffer reads are slow — this is
 * write-combining memory on real hardware, where a read costs far more than a
 * write.  It is implemented anyway, and left simple, because everything this
 * kernel prints before ring 3 exists fits on one screen at every resolution
 * worth having: sixty-odd lines against the seventy-five an 800x600 screen
 * holds.  Scrolling is the path that should not run, not the path that is
 * tuned.
 */
#include <iris/fbcon.h>
#include <iris/font8x8.h>

#define GLYPH_W FONT8X8_W
#define GLYPH_H FONT8X8_H

/* The glyphs live in <iris/font8x8.h>, shared with the ring-3 console. */

/* White on black: the two values that mean the same thing whether the
 * firmware reports RGBA or BGRA, which this boot protocol does not record. */
#define FG 0xFFFFFFFFu
#define BG 0x00000000u

static volatile uint32_t *g_fb;
static uint32_t g_stride;          /* pixels per scanline */
static uint32_t g_w, g_h;          /* pixels                            */
static uint32_t g_cols, g_rows;    /* characters                        */
static uint32_t g_col, g_row;      /* cursor, in characters             */
static uint32_t g_mark_col;        /* next marker column on the top line */
static int      g_on;
static int      g_yielded;         /* ring 3 owns the screen */

/*
 * The top line is reserved for boot markers and never scrolls, so text starts
 * one line down.  A marker is the last thing a dead boot leaves behind and it
 * must not be pushed off the screen by whatever managed to print afterwards.
 */
#define TEXT_ROW0 1u

static void fb_fill_row(uint32_t row, uint32_t colour) {
    if (!g_on || row >= g_rows) return;
    uint32_t y0 = row * GLYPH_H;
    for (uint32_t y = y0; y < y0 + GLYPH_H && y < g_h; y++)
        for (uint32_t x = 0; x < g_w; x++)
            g_fb[(uint64_t)y * g_stride + x] = colour;
}

static void fb_glyph(uint32_t col, uint32_t row, char c) {
    if (!g_on || col >= g_cols || row >= g_rows) return;
    const uint8_t *gl = font8x8_glyph((uint8_t)c);
    uint32_t x0 = col * GLYPH_W, y0 = row * GLYPH_H;
    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint32_t y = y0 + gy;
        if (y >= g_h) break;
        uint8_t bits = gl[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            uint32_t x = x0 + gx;
            if (x >= g_w) break;
            g_fb[(uint64_t)y * g_stride + x] = (bits >> gx) & 1u ? FG : BG;
        }
    }
}

static void fb_scroll(void) {
    if (!g_on) return;
    uint32_t top = TEXT_ROW0 * GLYPH_H;
    uint32_t n   = (g_rows - TEXT_ROW0 - 1u) * GLYPH_H;
    for (uint32_t y = 0; y < n; y++) {
        volatile uint32_t *dst = g_fb + (uint64_t)(top + y) * g_stride;
        volatile uint32_t *src = dst + (uint64_t)GLYPH_H * g_stride;
        for (uint32_t x = 0; x < g_w; x++) dst[x] = src[x];
    }
    fb_fill_row(g_rows - 1u, BG);
    g_row = g_rows - 1u;
}

int fbcon_init(const struct iris_fb_params *p) {
    g_on = 0;
    if (!p) return 0;
    /* Refused rather than guessed at: a mode this code cannot address is a
     * screen it would corrupt, and a corrupted screen is worse than none. */
    if (p->phys == 0 || p->size == 0) return 0;
    if (p->bpp != 4u) return 0;
    if (p->width == 0 || p->height == 0) return 0;
    if (p->stride < p->width) return 0;
    if ((uint64_t)p->stride * p->height * 4ull > p->size) return 0;
    if (p->width < GLYPH_W * 16u || p->height < GLYPH_H * 4u) return 0;

    g_fb     = (volatile uint32_t *)(uintptr_t)p->phys;
    g_stride = p->stride;
    g_w      = p->width;
    g_h      = p->height;
    g_cols   = g_w / GLYPH_W;
    g_rows   = g_h / GLYPH_H;
    g_col    = 0;
    g_row    = TEXT_ROW0;
    g_mark_col = 0;
    g_on     = 1;

    for (uint32_t r = 0; r < g_rows; r++) fb_fill_row(r, BG);
    return 1;
}

int fbcon_active(void) { return g_on && !g_yielded; }

void fbcon_yield(void) { g_yielded = 1; }

void fbcon_reclaim(void) {
    if (!g_on) return;
    g_yielded = 0;
    for (uint32_t r = TEXT_ROW0; r < g_rows; r++) fb_fill_row(r, BG);
    g_col = 0;
    g_row = TEXT_ROW0;
}

void fbcon_putc(char c) {
    if (!g_on || g_yielded) return;
    if (c == '\r') { g_col = 0; return; }
    if (c == '\n') {
        g_col = 0;
        if (++g_row >= g_rows) fb_scroll();
        return;
    }
    if (c == '\t') { g_col = (g_col + 8u) & ~7u; }
    else { fb_glyph(g_col, g_row, c); g_col++; }
    if (g_col >= g_cols) {
        g_col = 0;
        if (++g_row >= g_rows) fb_scroll();
    }
}

void fbcon_write(const char *s) {
    if (!g_on || g_yielded || !s) return;
    while (*s) fbcon_putc(*s++);
}

void fbcon_mark(char c) {
    if (!g_on || g_yielded) return;
    if (g_mark_col >= g_cols) return;    /* the line does not scroll */
    fb_glyph(g_mark_col++, 0, c);
}
