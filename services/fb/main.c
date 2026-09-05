/*
 * fb/main.c — ring-3 framebuffer painting service.
 *
 * Bootstrap protocol (over bootstrap channel from svc_loader):
 *   recv SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP → framebuffer_cap_h (KBootstrapCap
 *       with the framebuffer control capability only)
 *
 * Claims the framebuffer via SYS_FRAMEBUFFER_VMO, maps it writable, draws
 * rainbow stripes, then unmaps and exits.  The MMIO mapping persists so the
 * painted pixels remain visible after this process exits.
 */

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/endpoint_proto.h>
#include "../common/iris_vspace.h"
#include <iris/fb_info.h>
#include <iris/nc/error.h>
#include "../common/iris_map.h"
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
    return iris_syscall4((long)nr, (long)a0, (long)0L, (long)0L, (long)0);
}

static inline long fb_sys2(long nr, long a0, long a1) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)0L, (long)0);
}

/* Stage 6-pure Step 2: fb maps the framebuffer into a window nothing else has
 * touched, so it owes every level under it.  IRIS_CPTR_OWN_UNTYPED is the
 * budget its own address space was built from, minted by svc_loader. */
#define FB_SLOT_SELF_VS 40u
#define FB_SLOT_PT      41u
static long fb_self_vs(void);
static inline long fb_sys3(long nr, long a0, long a1, long a2) {
    long r = iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)0);
    if (r == (long)IRIS_ERR_MISSING_TABLE)
        r = iris_vspace_fixup(nr, a0, a1, a2, 0,
                              fb_self_vs(), (long)IRIS_CPTR_OWN_UNTYPED,
                              (long)((uint64_t)FB_SLOT_PT << 32), (long)FB_SLOT_PT,
                              0, 0);
    return r;
}
static long fb_self_vs(void) {
    static int ready = 0;
    if (!ready) {
        if (iris_syscall1(SYS_VSPACE_SELF, (long)((uint64_t)FB_SLOT_SELF_VS << 32)) != 0)
            return 0;
        ready = 1;
    }
    return (long)FB_SLOT_SELF_VS;
}

static inline long fb_sys4(long nr, long a0, long a1, long a2, long a3) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)a3);
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

void fb_main_c(handle_id_t rbx_unused) {
    /* Phase 13 (Track I): the framebuffer capability arrives as a pre-start
     * mint — SYS_FRAMEBUFFER_VMO resolves it by CPtr.  No bootstrap KChannel:
     * svc_loader passes RBX = 0, so this argument is not a handle and closing
     * it was closing handle 0.
     *
     * Stage 5 Step 2: what fb holds is the FRAMEBUFFER CONTROL capability at
     * IRIS_CPTR_FB_CONTROL — the whole of its boot authority.  It used to be a
     * narrowed clone of init's monolith, which is to say: an object of the
     * same type as the one that authorises spawning and poweroff, trusted to
     * have had those bits masked off correctly. */
    (void)rbx_unused;
    /* The framebuffer capability is a CSpace slot, not a handle: it is never
     * closed, and it must never be passed to SYS_HANDLE_CLOSE — that call used
     * to sit on the error path, where cap_h had not yet been blanked, and
     * asked the handle table to close CPtr 6. */
    const long  cap_cptr = (long)IRIS_CPTR_FB_CONTROL;
    /* Stage 4: the framebuffer VMO lands in a CSpace slot of fb's own choosing
     * instead of coming back as a handle.  fb's manifest is a single mint
     * (IRIS_CPTR_FB_CONTROL), so every other slot is free; 16 keeps a clear
     * gap from the well-known service range. */
    const long  fb_frame_slot = 16;
    long        vmo_cptr = -1;
    struct iris_fb_params params;

    /*
     * ── The framebuffer, as memory fb OWNS ────────────────────────────
     *
     * This used to be one call: SYS_FRAMEBUFFER_VMO answered "where and how
     * big is it" and fabricated a KVMO over the region in the same breath, so
     * the geometry could only be learned by accepting a kernel-made object —
     * the last memory object in the system that nobody retyped (ledger D-5).
     *
     * Now the two halves are what they are.  The geometry is a fact boot
     * discovered and SYS_FRAMEBUFFER_INFO reports it, creating nothing.  The
     * REGION arrives as a DEVICE Untyped (D-9), and fb retypes a frame out of
     * it exactly as it would out of RAM — one frame for the whole thing, since
     * a frame maps as a whole (D-10).
     *
     * A device Untyped cannot hold the headers of objects carved from it —
     * MMIO is not storage — so fb names the RAM that pays, out of the budget
     * its spawner gave it.
     */
    {
        uint8_t *raw = (uint8_t *)&params;
        uint32_t i;
        for (i = 0; i < (uint32_t)sizeof(params); i++) raw[i] = 0;
    }
    if (fb_sys3(SYS_FRAMEBUFFER_INFO, cap_cptr,
                (long)(uintptr_t)&params, 0) != IRIS_OK)
        goto out;
    if (params.width == 0 || params.height == 0 || params.size == 0) goto out;

    /* The headers of what comes out of MMIO are charged to fb's own budget. */
    (void)fb_sys3(SYS_UNTYPED_SET_DEVICE_BUDGET,
                  (long)IRIS_CPTR_DEVICE_UNTYPED,
                  (long)IRIS_CPTR_OWN_UNTYPED, 0);

    {
        uint64_t bytes = (params.size + 0xFFFu) & ~0xFFFULL;
        if (fb_sys4(SYS_UNTYPED_RETYPE2, (long)IRIS_CPTR_DEVICE_UNTYPED,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)fb_frame_slot << 32),
                    (long)bytes) != 0)
            goto out;
        vmo_cptr = fb_frame_slot;
    }

    /* ── Map framebuffer ──────────────────────────────────────────────
     * The window is one nothing has touched, and the kernel creates no paging
     * levels, so the helper supplies them — several, for a region this size. */
    if (iris_map_frame((uint64_t)vmo_cptr, (uint64_t)fb_self_vs(),
                       IRIS_CPTR_OWN_UNTYPED, FB_SLOT_PT,
                       USER_VMO_BASE, (params.size + 0xFFFu) & ~0xFFFULL,
                       1u /* writable */) != 0)
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
    (void)fb_sys3(SYS_FRAME_UNMAP, vmo_cptr, fb_self_vs(),
                  (long)USER_VMO_BASE);

out:
    /* The framebuffer cap is a CSpace slot now: fb is fire-and-forget and its
     * whole CSpace is torn down with the address space, so there is nothing to
     * close.  Deleting the slot explicitly would also work and is what a
     * long-lived service would do. */
    if (vmo_cptr >= 0)
        (void)fb_sys2(SYS_CNODE_DELETE, 0, vmo_cptr);
}
