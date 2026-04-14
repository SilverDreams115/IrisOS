#ifndef IRIS_INITRD_H
#define IRIS_INITRD_H

/*
 * initrd.h — kernel initrd lookup API.
 *
 * The initrd is a read-only table of named ELF images embedded into the
 * kernel binary via `objcopy -I binary`.  Each entry maps a short name
 * (≤ 31 bytes, NUL-terminated) to a contiguous byte range in the kernel
 * image.
 *
 * Usage:
 *   const void *data; uint32_t size;
 *   if (initrd_find("svcmgr", &data, &size) == 0) { // not found }
 *   iris_error_t err = elf_loader_load(data, size, &img);
 */

#include <stdint.h>

/*
 * initrd_find — look up a named service image in the embedded initrd.
 *
 * @name   NUL-terminated service name (case-sensitive, max 31 chars).
 * @out_data  Set to a pointer to the first byte of the ELF image.
 * @out_size  Set to the byte length of the ELF image.
 *
 * Returns 1 if found, 0 if not found or if out pointers are NULL.
 * The returned data pointer is valid for the lifetime of the kernel.
 */
int initrd_find(const char *name, const void **out_data, uint32_t *out_size);

#endif /* IRIS_INITRD_H */
