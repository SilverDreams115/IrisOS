#ifndef IRIS_INITRD_H
#define IRIS_INITRD_H

/*
 * initrd.h — kernel initrd catalog API.
 *
 * The initrd is a read-only table of ELF images embedded into the kernel
 * binary via `objcopy -I binary`. Userland-visible lookup remains named
 * because SYS_INITRD_LOOKUP exposes a generic catalog service to bootstrap
 * code.
 *
 * The kernel boot path must not depend on service names. It consumes one
 * opaque bootstrap image selected by initrd_bootstrap_image().
 */

#include <stdint.h>

/*
 * initrd_bootstrap_image — return the kernel-selected bootstrap ELF image.
 *
 * This is the only initrd API the kernel boot path should consume directly.
 * The returned image is opaque to the caller; boot sequencing policy beyond
 * "spawn the bootstrap image" lives in userland.
 */
int initrd_bootstrap_image(const void **out_data, uint32_t *out_size);

/*
 * initrd_find — look up a named ELF image in the embedded initrd catalog.
 *
 * @name      NUL-terminated image name (case-sensitive, max 31 chars).
 * @out_data  Set to a pointer to the first byte of the ELF image.
 * @out_size  Set to the byte length of the ELF image.
 *
 * Returns 1 if found, 0 if not found or if out pointers are NULL.
 * The returned data pointer is valid for the lifetime of the kernel.
 */
int initrd_find(const char *name, const void **out_data, uint32_t *out_size);

#endif /* IRIS_INITRD_H */
