#ifndef IRIS_INITRD_H
#define IRIS_INITRD_H

/*
 * initrd.h — kernel initrd catalog API.
 *
 * The initrd is a read-only table of binary images embedded into the kernel
 * via `objcopy -I binary`. Images are identified by index only; the kernel
 * has no knowledge of service names or file formats.
 *
 * Name→index mapping lives entirely in ring-3 (services/common/svc_loader.c).
 */

#include <stdint.h>

/* initrd_bootstrap_image — return the bootstrap image (always index 0). */
int initrd_bootstrap_image(const void **out_data, uint32_t *out_size);

/* initrd_get — return the image at the given index. Returns 1 on success. */
int initrd_get(uint32_t index, const void **out_data, uint32_t *out_size);

/* initrd_count — return the total number of embedded images. */
uint32_t initrd_count(void);

#endif /* IRIS_INITRD_H */
