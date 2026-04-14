/*
 * initrd.c — kernel initrd service image registry.
 *
 * Service ELF binaries are embedded into the kernel image via the Makefile:
 *
 *   objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
 *       services/svcmgr/svcmgr.elf $(BUILD_DIR)/svcmgr_bin.o
 *
 * objcopy creates three linker symbols per input file:
 *   _binary_<mangled_path>_start  — pointer to first byte
 *   _binary_<mangled_path>_end    — pointer one past last byte
 *   _binary_<mangled_path>_size   — size in bytes (symbol value, not address)
 *
 * The path is mangled: every character that is not alphanumeric becomes '_'.
 * For a file compiled from services/svcmgr/svcmgr.elf embedded as
 * build/svcmgr_bin.o the mangled name is:
 *   _binary_services_svcmgr_svcmgr_elf_{start,end,size}
 *
 * This file declares those extern symbols and maps service names to them in a
 * static table.  initrd_find() does a linear search — acceptable given the
 * small table size.
 */

#include <iris/initrd.h>
#include <iris/service_catalog.h>
#include <stdint.h>

/* ── Embedded service symbol declarations ───────────────────────────────── */

/* svcmgr */
extern const uint8_t _binary_services_svcmgr_svcmgr_elf_start[];
extern const uint8_t _binary_services_svcmgr_svcmgr_elf_end[];

/* kbd */
extern const uint8_t _binary_services_kbd_kbd_elf_start[];
extern const uint8_t _binary_services_kbd_kbd_elf_end[];

/* vfs */
extern const uint8_t _binary_services_vfs_vfs_elf_start[];
extern const uint8_t _binary_services_vfs_vfs_elf_end[];

/* ── Initrd registry ─────────────────────────────────────────────────────── */

struct initrd_entry {
    const char    *name;
    const uint8_t *start;
    const uint8_t *end;
};

static const struct initrd_entry g_initrd[] = {
    { IRIS_BOOTSTRAP_SUPERVISOR_IMAGE, _binary_services_svcmgr_svcmgr_elf_start,
                _binary_services_svcmgr_svcmgr_elf_end },
    { IRIS_SERVICE_IMAGE_KBD, _binary_services_kbd_kbd_elf_start,
                _binary_services_kbd_kbd_elf_end },
    { IRIS_SERVICE_IMAGE_VFS, _binary_services_vfs_vfs_elf_start,
                _binary_services_vfs_vfs_elf_end },
};

#define INITRD_ENTRY_COUNT \
    ((uint32_t)(sizeof(g_initrd) / sizeof(g_initrd[0])))

/* ── Simple string equality helper ──────────────────────────────────────── */

static int initrd_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int initrd_find(const char *name, const void **out_data, uint32_t *out_size) {
    if (!name || !out_data || !out_size) return 0;

    for (uint32_t i = 0; i < INITRD_ENTRY_COUNT; i++) {
        if (initrd_streq(name, g_initrd[i].name)) {
            *out_data = (const void *)g_initrd[i].start;
            if (g_initrd[i].end > g_initrd[i].start)
                *out_size = (uint32_t)(g_initrd[i].end - g_initrd[i].start);
            else
                *out_size = 0;
            return 1;
        }
    }
    return 0;
}
