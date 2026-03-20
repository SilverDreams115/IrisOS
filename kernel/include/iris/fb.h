#ifndef IRIS_FB_H
#define IRIS_FB_H

#include <stdint.h>
#include <iris/boot_info.h>

void fb_init(struct iris_boot_info *bi);
void fb_fill(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

/* colores ARGB */
#define FB_BLACK   0x00000000
#define FB_WHITE   0x00FFFFFF
#define FB_RED     0x00FF0000
#define FB_GREEN   0x0000FF00
#define FB_BLUE    0x000000FF
#define FB_CYAN    0x0000FFFF
#define FB_MAGENTA 0x00FF00FF
#define FB_YELLOW  0x00FFFF00
#define FB_IRIS    0x006A0DAD  /* morado IrisOS */

#endif
