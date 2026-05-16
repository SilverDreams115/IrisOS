/*
 * initrd.c — kernel initrd image catalog.
 *
 * Service binaries are embedded into the kernel image via the Makefile:
 *
 *   objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
 *       services/svcmgr/svcmgr.elf $(BUILD_DIR)/svcmgr_bin.o
 *
 * objcopy creates three linker symbols per input file:
 *   _binary_<mangled_path>_start  — pointer to first byte
 *   _binary_<mangled_path>_end    — pointer one past last byte
 *
 * The kernel exposes images by index only. Name→index mapping is a ring-3
 * concern (services/common/svc_loader.c). The kernel has zero knowledge of
 * service names or file formats — it is a pure image catalog.
 *
 * Index layout (must match the ring-3 catalog in svc_loader.c):
 *   [0] userboot — bootstrap stub (flat binary, loaded directly by kernel)
 *   [1] init
 *   [2] svcmgr
 *   [3] kbd
 *   [4] vfs
 *   [5] console
 *   [6] fb
 *   [7] sh
 */

#include <iris/initrd.h>
#include <stdint.h>

/* ── Embedded image symbol declarations ─────────────────────────────────── */

extern const uint8_t _binary_services_userboot_userboot_bin_start[];
extern const uint8_t _binary_services_userboot_userboot_bin_end[];

extern const uint8_t _binary_services_init_init_elf_start[];
extern const uint8_t _binary_services_init_init_elf_end[];

extern const uint8_t _binary_services_svcmgr_svcmgr_elf_start[];
extern const uint8_t _binary_services_svcmgr_svcmgr_elf_end[];

extern const uint8_t _binary_services_kbd_kbd_elf_start[];
extern const uint8_t _binary_services_kbd_kbd_elf_end[];

extern const uint8_t _binary_services_vfs_vfs_elf_start[];
extern const uint8_t _binary_services_vfs_vfs_elf_end[];

extern const uint8_t _binary_services_console_console_elf_start[];
extern const uint8_t _binary_services_console_console_elf_end[];

extern const uint8_t _binary_services_fb_fb_elf_start[];
extern const uint8_t _binary_services_fb_fb_elf_end[];

extern const uint8_t _binary_services_sh_sh_elf_start[];
extern const uint8_t _binary_services_sh_sh_elf_end[];

/* ── Initrd catalog (index-only, no names) ──────────────────────────────── */

struct initrd_entry {
    const uint8_t *start;
    const uint8_t *end;
};

static const struct initrd_entry g_initrd[] = {
    /* [0] */ { _binary_services_userboot_userboot_bin_start,
                _binary_services_userboot_userboot_bin_end  },
    /* [1] */ { _binary_services_init_init_elf_start,
                _binary_services_init_init_elf_end          },
    /* [2] */ { _binary_services_svcmgr_svcmgr_elf_start,
                _binary_services_svcmgr_svcmgr_elf_end      },
    /* [3] */ { _binary_services_kbd_kbd_elf_start,
                _binary_services_kbd_kbd_elf_end             },
    /* [4] */ { _binary_services_vfs_vfs_elf_start,
                _binary_services_vfs_vfs_elf_end             },
    /* [5] */ { _binary_services_console_console_elf_start,
                _binary_services_console_console_elf_end     },
    /* [6] */ { _binary_services_fb_fb_elf_start,
                _binary_services_fb_fb_elf_end               },
    /* [7] */ { _binary_services_sh_sh_elf_start,
                _binary_services_sh_sh_elf_end               },
};

#define INITRD_ENTRY_COUNT \
    ((uint32_t)(sizeof(g_initrd) / sizeof(g_initrd[0])))

/* ── Public API ──────────────────────────────────────────────────────────── */

int initrd_bootstrap_image(const void **out_data, uint32_t *out_size) {
    return initrd_get(0, out_data, out_size);
}

int initrd_get(uint32_t index, const void **out_data, uint32_t *out_size) {
    if (!out_data || !out_size || index >= INITRD_ENTRY_COUNT) return 0;
    *out_data = (const void *)g_initrd[index].start;
    if (g_initrd[index].end > g_initrd[index].start)
        *out_size = (uint32_t)(g_initrd[index].end - g_initrd[index].start);
    else
        *out_size = 0;
    return 1;
}

uint32_t initrd_count(void) {
    return INITRD_ENTRY_COUNT;
}
