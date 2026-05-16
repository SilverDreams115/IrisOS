#pragma once
#include <stdint.h>
#include <iris/nc/handle.h>

/* Number of entries in the ring-3 name→index catalog (must match initrd.c). */
#define SL_CATALOG_COUNT 8u

/*
 * svc_initrd_count — query the kernel initrd catalog entry count.
 * Requires a KBootstrapCap with IRIS_BOOTCAP_SPAWN_SERVICE.
 * Returns the count (≥0) or a negative iris_error_t on failure.
 * Callers should assert the result equals SL_CATALOG_COUNT at startup.
 */
long svc_initrd_count(handle_id_t spawn_cap_h);

/*
 * svc_load — load a named ET_DYN (static PIE) ELF from the initrd into a
 * fresh process, returning the parent end of the bootstrap IPC channel and
 * the process handle.
 *
 * Protocol:
 *   - spawn_cap_h: KBootstrapCap handle with IRIS_BOOTCAP_SPAWN_SERVICE
 *   - name: NUL-terminated service name (≤ 31 chars)
 *   - *out_proc_h: receives process handle (RIGHT_READ|WRITE|MANAGE|DUP|XFER|ROUTE)
 *   - *out_chan_h: receives parent end of bootstrap channel (RIGHT_READ|WRITE|DUP|XFER)
 *
 * On success returns 0; on failure returns a negative iris_error_t cast to long.
 * Both *out_proc_h and *out_chan_h are HANDLE_INVALID on failure.
 *
 * The child process starts with RBX = handle_id of its own end of the
 * bootstrap channel, consistent with the entry.S bootstrap convention.
 */
long svc_load(handle_id_t spawn_cap_h, const char *name,
              handle_id_t *out_proc_h, handle_id_t *out_chan_h);
