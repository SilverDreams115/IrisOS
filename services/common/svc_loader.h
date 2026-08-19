#pragma once
#include <stdint.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>

/* Number of entries in the ring-3 name→index catalog (must match the FIRST
 * SL_CATALOG_COUNT entries of initrd.c).  Index 9 = lifecycle_probe, a
 * TEST-only child spawned by iris_test.  The kernel initrd may hold ADDITIONAL
 * images beyond this at indices >= SL_CATALOG_COUNT (new services, backing
 * blobs, etc.); those are not named by the ring-3 catalog and are loaded by
 * index/other means.  The boot invariant is therefore that the kernel has AT
 * LEAST this many images (so every named index resolves), not exactly this
 * many — see the Fase 28 boot-growth fix in userboot. */
#define SL_CATALOG_COUNT 11u   /* index 10 = pager (own binary, Fase 28) */

/*
 * Stage 5 Etapa 2: spawning needs TWO authorities, and they are two
 * capabilities.  `initrd_c` authorises reading boot images
 * (SYS_INITRD_COUNT / SYS_INITRD_VMO) and `proc_c` authorises creating a
 * process (SYS_PROCESS_CREATE).  They used to be one bit on one object, so a
 * service that only needed to read an image — vfs — also held the authority to
 * create processes.  Both are CPtrs into the CALLER's CSpace.
 */

/*
 * svc_initrd_count — query the kernel initrd catalog entry count.
 * Requires the initrd control capability.
 * Returns the count (≥0) or a negative iris_error_t on failure.
 * Boot code must assert the result is >= SL_CATALOG_COUNT (every named image
 * present), NOT == : the initrd is allowed to grow beyond the named catalog.
 */
long svc_initrd_count(uint64_t initrd_c);

/*
 * svc_load — load a named ET_DYN (static PIE) ELF from the initrd into a
 * fresh process, returning the parent end of the bootstrap IPC channel and
 * the process handle.
 *
 * Protocol:
 *   - proc_c / initrd_c: the process-control and initrd capabilities
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
long svc_load(uint64_t proc_c, uint64_t initrd_c, const char *name,
              handle_id_t *out_proc_h, handle_id_t *out_chan_h);

/*
 * Fase 8: pre-start CSpace mint table.
 *
 * svc_load_minted behaves like svc_load but additionally mints the given
 * capabilities into the child's root CNode (SYS_PROC_CSPACE_MINT) BEFORE
 * the first thread starts — the child observes its well-known slots fully
 * populated from its first instruction, with no bootstrap-message barrier
 * and no retry loops.  Mint failures are non-fatal (slots stay empty and
 * the consumer's smoke gates fail loudly); src_h == HANDLE_INVALID entries
 * are skipped.
 */
struct svc_mint {
    uint64_t      slot;    /* destination CPtr slot in the child root CNode */
    handle_id_t   src_h;   /* source cap in the CALLER's handle table
                            * (legacy path: SYS_PROC_CSPACE_MINT) */
    uint64_t      src_cptr;/* Fase S4: source cap in the CALLER's CSpace.  When
                            * non-zero it WINS over src_h and the mint goes
                            * through SYS_CSPACE_MINT_INTO, so the child's cap
                            * becomes an MDB child of OUR slot — the delegation
                            * stays revocable from the supervisor.  The handle
                            * path is legacy and retires with the dual
                            * namespace (Stage 4). */
    iris_rights_t rights;  /* rights mask (reduced against src rights) */
    uint64_t      badge;   /* Fase 9: sender badge for the minted cap
                            * (0 = inherit source badge / unbadged).
                            * Packed into SYS_PROC_CSPACE_MINT arg3 high
                            * bits; subject to the kernel's no-re-badge
                            * rule. */
};

/* Etapa 4: spawn publishing every created capability into CSpace.
 *
 * `ws` packs the untyped to carve a second-level CNode from (low 32 bits) and
 * the root slot that holds it (high 32).  A spawn needs eleven capabilities
 * alive at once and no spawning service has eleven free root slots; one slot
 * plus 256 leaves does.  ws == 0 keeps the legacy handle path.
 *
 * It is a parameter and not loader state on purpose: userboot is a flat binary
 * with no writable .data, so anything the loader remembered between calls
 * would fault on its first caller. */
#define SVC_LOADER_WS(untyped, slot) \
    ((uint64_t)(uint32_t)(untyped) | ((uint64_t)(uint32_t)(slot) << 32))

/* `child_budget` (bytes, 0 = the default) is the Untyped the child's KERNEL
 * memory is carved from: its address space, its process state, and the segment
 * and stack VMOs the loader charges to it.  It is recycled when the child dies
 * (Stage 6 Etapa 5), so it bounds concurrent cost rather than accumulating —
 * which is why the spawner, who knows what it is launching, chooses the size. */
long svc_load_minted_ws(uint64_t proc_c, uint64_t initrd_c, const char *name,
                        handle_id_t *out_proc_h, handle_id_t *out_chan_h,
                        const struct svc_mint *mints, uint32_t mint_count,
                        uint64_t ws, uint64_t child_budget);

long svc_load_minted(uint64_t proc_c, uint64_t initrd_c, const char *name,
                     handle_id_t *out_proc_h, handle_id_t *out_chan_h,
                     const struct svc_mint *mints, uint32_t mint_count);
