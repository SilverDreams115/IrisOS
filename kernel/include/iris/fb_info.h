#ifndef IRIS_FB_INFO_H
#define IRIS_FB_INFO_H

#include <stdint.h>

/* Framebuffer parameters shared between the kernel and the ring-3 fb service.
 * Filled by kernel_main from UEFI GOP data; exposed via SYS_FRAMEBUFFER_VMO. */
struct iris_fb_params {
    uint64_t phys;    /* physical base address of the linear framebuffer */
    uint64_t size;    /* byte size of the framebuffer region */
    uint32_t width;   /* horizontal resolution in pixels */
    uint32_t height;  /* vertical resolution in pixels */
    uint32_t stride;  /* pixels per scanline (may be wider than width) */
    uint32_t bpp;     /* bytes per pixel (4 for UEFI GOP ARGB32) */
};

#ifdef __KERNEL__
extern struct iris_fb_params g_iris_fb_params;
extern int                   g_iris_fb_params_valid;
#endif

#endif
