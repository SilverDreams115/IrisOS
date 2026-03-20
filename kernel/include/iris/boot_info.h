#ifndef IRIS_BOOT_INFO_H
#define IRIS_BOOT_INFO_H

#include <stdint.h>

#define IRIS_BOOTINFO_MAGIC   0x49524953424F4F54ULL
#define IRIS_BOOTINFO_VERSION 2ULL

#define IRIS_MMAP_MAX_ENTRIES 256

/* tipos de región de memoria — espejo de EFI_MEMORY_TYPE */
#define IRIS_MEM_USABLE          1
#define IRIS_MEM_RESERVED        2
#define IRIS_MEM_ACPI_RECLAIMABLE 3
#define IRIS_MEM_ACPI_NVS        4
#define IRIS_MEM_BAD             5
#define IRIS_MEM_BOOTLOADER      6
#define IRIS_MEM_KERNEL          7
#define IRIS_MEM_FRAMEBUFFER     8

struct iris_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

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
    uint64_t mmap_entry_count;
    struct iris_mmap_entry mmap[IRIS_MMAP_MAX_ENTRIES];
};

#endif
