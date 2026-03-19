#ifndef IRIS_BOOT_INFO_H
#define IRIS_BOOT_INFO_H

#include <stdint.h>

#define IRIS_BOOTINFO_MAGIC 0x49524953424F4F54ULL
#define IRIS_BOOTINFO_VERSION 1ULL

struct iris_framebuffer_info {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t reserved;
};

struct iris_boot_info {
    uint64_t magic;
    uint64_t version;
    struct iris_framebuffer_info framebuffer;
};

#endif
