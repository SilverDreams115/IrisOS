/*
 * fb/main.c — ring-3 framebuffer painting service.
 *
 * Bootstrap protocol (over bootstrap channel from svc_loader):
 *   recv SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP → framebuffer_cap_h (KBootstrapCap
 *       with IRIS_BOOTCAP_FRAMEBUFFER only)
 *
 * Claims the framebuffer via SYS_FRAMEBUFFER_VMO, maps it writable, draws
 * rainbow stripes, then unmaps and exits.  The MMIO mapping persists so the
 * painted pixels remain visible after this process exits.
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/endpoint_proto.h>
#include <iris/fb_info.h>
#include <iris/nc/error.h>
#include <iris/paging.h>

#define MAP_WRITABLE 1u

/* Rainbow stripe colours (ARGB32). */
#define FB_RED    0x00FF0000u
#define FB_ORANGE 0x00FF8800u
#define FB_YELLOW 0x00FFFF00u
#define FB_GREEN  0x0000FF00u
#define FB_CYAN   0x0000FFFFu
#define FB_BLUE   0x000000FFu
#define FB_IRIS   0x008800FFu

static inline long fb_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long fb_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long fb_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long fb_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}

static void fb_draw_rect(uint32_t *pixels, uint32_t stride,
                         uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t row = y; row < y + h; row++) {
        for (uint32_t col = x; col < x + w; col++) {
            pixels[row * stride + col] = color;
        }
    }
}

void fb_main_c(handle_id_t bootstrap_h) {
    /* Fase 13 (Track I): the framebuffer KBootstrapCap arrives as the
     * IRIS_CPTR_SPAWN_CAP (slot 6) pre-start mint — SYS_FRAMEBUFFER_VMO resolves
     * it by CPtr through the device-cap dual resolver.  No bootstrap KChannel. */
    handle_id_t cap_h  = (handle_id_t)IRIS_CPTR_SPAWN_CAP;
    handle_id_t vmo_h  = HANDLE_INVALID;
    struct iris_fb_params params;

    (void)fb_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_h);
    bootstrap_h = HANDLE_INVALID;

    /* ── Claim framebuffer VMO ────────────────────────────────────── */
    {
        uint8_t *raw = (uint8_t *)&params;
        uint32_t i;
        for (i = 0; i < (uint32_t)sizeof(params); i++) raw[i] = 0;
    }
    {
        long r = fb_sys4(SYS_FRAMEBUFFER_VMO, (long)cap_h,
                         (long)(uintptr_t)&params, 0, 0);
        if (r < 0) goto out;
        vmo_h = (handle_id_t)r;
    }
    /* cap_h is the IRIS_CPTR_SPAWN_CAP CSpace slot, not an owned handle —
     * reaped with the address space, nothing to close (Track I). */
    cap_h = HANDLE_INVALID;

    if (params.width == 0 || params.height == 0 || params.size == 0) goto out;

    /* ── Map framebuffer ──────────────────────────────────────────── */
    if (fb_sys3(SYS_VMO_MAP, (long)vmo_h, (long)USER_VMO_BASE,
                (long)MAP_WRITABLE) != IRIS_OK)
        goto out;

    /* ── Paint rainbow stripes ────────────────────────────────────── */
    {
        uint32_t *pixels = (uint32_t *)(uintptr_t)USER_VMO_BASE;
        uint32_t  stride = params.stride;
        uint32_t  w      = params.width;
        uint32_t  h      = params.height;
        uint32_t  stripe = h / 7u;
        if (stripe == 0) stripe = 1u;

        /* Clear to black first. */
        {
            uint32_t total = stride * h;
            uint32_t i;
            for (i = 0; i < total; i++) pixels[i] = 0u;
        }

        fb_draw_rect(pixels, stride, 0, stripe * 0u, w, stripe, FB_RED);
        fb_draw_rect(pixels, stride, 0, stripe * 1u, w, stripe, FB_ORANGE);
        fb_draw_rect(pixels, stride, 0, stripe * 2u, w, stripe, FB_YELLOW);
        fb_draw_rect(pixels, stride, 0, stripe * 3u, w, stripe, FB_GREEN);
        fb_draw_rect(pixels, stride, 0, stripe * 4u, w, stripe, FB_CYAN);
        fb_draw_rect(pixels, stride, 0, stripe * 5u, w, stripe, FB_BLUE);
        fb_draw_rect(pixels, stride, 0, stripe * 6u, w, stripe, FB_IRIS);
    }

    /* ── Unmap (physical MMIO stays painted) ─────────────────────── */
    (void)fb_sys2(SYS_VMO_UNMAP, (long)USER_VMO_BASE, (long)params.size);

out:
    if (bootstrap_h != HANDLE_INVALID)
        (void)fb_sys1(SYS_HANDLE_CLOSE, (long)bootstrap_h);
    if (cap_h != HANDLE_INVALID)
        (void)fb_sys1(SYS_HANDLE_CLOSE, (long)cap_h);
    if (vmo_h != HANDLE_INVALID)
        (void)fb_sys1(SYS_HANDLE_CLOSE, (long)vmo_h);
}
