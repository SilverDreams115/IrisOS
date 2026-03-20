#include <iris/fb.h>

static uint32_t  fb_width;
static uint32_t  fb_height;
static uint32_t  fb_stride;
static uint32_t *fb_base;

void fb_init(struct iris_boot_info *bi) {
    fb_width  = bi->framebuffer.width;
    fb_height = bi->framebuffer.height;
    fb_stride = bi->framebuffer.pixels_per_scanline;
    fb_base   = (uint32_t *)0x80000000UL;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    uint32_t offset = y * (fb_stride) + x;
    fb_base[offset] = color;
}

void fb_fill(uint32_t color) {
    uint32_t stride = fb_stride;
    for (uint32_t y = 0; y < fb_height; y++)
        for (uint32_t x = 0; x < fb_width; x++)
            fb_base[y * stride + x] = color;
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t stride = fb_stride;
    for (uint32_t row = y; row < y + h && row < fb_height; row++)
        for (uint32_t col = x; col < x + w && col < fb_width; col++)
            fb_base[row * stride + col] = color;
}
