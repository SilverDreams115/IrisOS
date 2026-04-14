#ifndef IRIS_DIAG_H
#define IRIS_DIAG_H

/*
 * IRIS Global Diagnostics / Status Surface
 *
 * ── Overview ─────────────────────────────────────────────────────────────────
 * Provides a compact, queryable snapshot of essential kernel-side state.
 * Accessed via SYS_DIAG_SNAPSHOT (syscall 30).
 *
 * Design principles:
 *   - One syscall, one atomic snapshot; no partial reads.
 *   - Unrestricted: any task may query (observability ≠ authority).
 *   - Compact: 64 bytes, versioned, with bounded pool-pressure counters.
 *   - Bounded: summarises, does not dump raw internals.
 *   - No service IPC required for the kernel-owned portion of the picture.
 *
 * ── Access model ─────────────────────────────────────────────────────────────
 *
 *   Kernel-owned state (this header):
 *     Queried via SYS_DIAG_SNAPSHOT → iris_diag_snapshot written to user buffer.
 *     Includes: task count, KProcess/KChannel/KNotification/KVmo pools,
 *     IRQ routes, and scheduler ticks.
 *
 *   Service-owned state (per-service STATUS channels):
 *     svcmgr:  SVCMGR_MSG_STATUS → SVCMGR_MSG_STATUS_REPLY  (svcmgr_proto.h)
 *     vfs:     VFS_MSG_STATUS    → VFS_MSG_STATUS_REPLY      (vfs_proto.h)
 *     kbd:     KBD_MSG_STATUS    → KBD_MSG_STATUS_REPLY      (kbd_proto.h)
 *
 *   Together these form the full diagnostics picture.
 *
 * ── iris_diag_snapshot wire layout (64 bytes) ────────────────────────────────
 *   off  0: uint32_t magic           IRIS_DIAG_MAGIC — integrity marker
 *   off  4: uint32_t version         IRIS_DIAG_VERSION (2)
 *   off  8: uint32_t tasks_live      non-DEAD scheduler tasks
 *   off 12: uint32_t tasks_max       TASK_MAX ceiling
 *   off 16: uint32_t kproc_live      KProcess pool slots in use
 *   off 20: uint32_t kproc_max       KPROCESS_POOL_SIZE ceiling
 *   off 24: uint32_t irq_routes_active  routed hardware IRQ lines
 *   off 28: uint32_t irq_routes_max     IRQ_ROUTE_MAX ceiling
 *   off 32: uint32_t ticks_lo        scheduler_ticks low  32 bits
 *   off 36: uint32_t ticks_hi        scheduler_ticks high 32 bits
 *   off 40: uint32_t kchan_live      KChannel pool slots in use
 *   off 44: uint32_t kchan_max       KCHANNEL_POOL_SIZE ceiling
 *   off 48: uint32_t knotif_live     KNotification pool slots in use
 *   off 52: uint32_t knotif_max      KNOTIF_POOL_SIZE ceiling
 *   off 56: uint32_t kvmo_live       KVmo pool slots in use
 *   off 60: uint32_t kvmo_max        KVMO_POOL_SIZE ceiling
 *
 * ── Version ──────────────────────────────────────────────────────────────────
 *   Bump IRIS_DIAG_VERSION when any field layout changes.
 *   Clients must validate magic and version before reading other fields.
 *
 * ── Phase status ─────────────────────────────────────────────────────────────
 *   Phase 12/current: diagnostics now expose bounded pool pressure for
 *   channels, notifications, and VMOs in addition to task/process/IRQ counts.
 */

/* Integrity marker and version */
#define IRIS_DIAG_MAGIC    0xD1A60001
#define IRIS_DIAG_VERSION  2

/* Byte offsets within iris_diag_snapshot — usable from assembly without 'u' suffix */
#define IRIS_DIAG_OFF_MAGIC        0
#define IRIS_DIAG_OFF_VERSION      4
#define IRIS_DIAG_OFF_TASKS_LIVE   8
#define IRIS_DIAG_OFF_TASKS_MAX   12
#define IRIS_DIAG_OFF_KPROC_LIVE  16
#define IRIS_DIAG_OFF_KPROC_MAX   20
#define IRIS_DIAG_OFF_IRQ_ACTIVE  24
#define IRIS_DIAG_OFF_IRQ_MAX     28
#define IRIS_DIAG_OFF_TICKS_LO    32
#define IRIS_DIAG_OFF_TICKS_HI    36
#define IRIS_DIAG_OFF_KCHAN_LIVE  40
#define IRIS_DIAG_OFF_KCHAN_MAX   44
#define IRIS_DIAG_OFF_KNOTIF_LIVE 48
#define IRIS_DIAG_OFF_KNOTIF_MAX  52
#define IRIS_DIAG_OFF_KVMO_LIVE   56
#define IRIS_DIAG_OFF_KVMO_MAX    60

#define IRIS_DIAG_SNAPSHOT_SIZE   64  /* total buffer size in bytes */

#ifndef __ASSEMBLER__
#include <stdint.h>

/*
 * iris_diag_snapshot — kernel-owned global status snapshot.
 *
 * Written atomically by SYS_DIAG_SNAPSHOT into a caller-supplied user buffer.
 * All fields are little-endian uint32_t values.
 * Clients must verify magic == IRIS_DIAG_MAGIC and version == IRIS_DIAG_VERSION
 * before reading any other field.
 */
struct iris_diag_snapshot {
    uint32_t magic;               /* IRIS_DIAG_MAGIC: integrity marker         */
    uint32_t version;             /* IRIS_DIAG_VERSION (2)                     */
    uint32_t tasks_live;          /* tasks in non-DEAD scheduler states        */
    uint32_t tasks_max;           /* TASK_MAX ceiling                          */
    uint32_t kproc_live;          /* KProcess pool slots in use                */
    uint32_t kproc_max;           /* KPROCESS_POOL_SIZE ceiling                */
    uint32_t irq_routes_active;   /* hardware IRQ lines with active channel    */
    uint32_t irq_routes_max;      /* IRQ_ROUTE_MAX ceiling                     */
    uint32_t ticks_lo;            /* scheduler_ticks low  32 bits              */
    uint32_t ticks_hi;            /* scheduler_ticks high 32 bits              */
    uint32_t kchan_live;          /* KChannel pool slots in use                */
    uint32_t kchan_max;           /* KCHANNEL_POOL_SIZE ceiling                */
    uint32_t knotif_live;         /* KNotification pool slots in use           */
    uint32_t knotif_max;          /* KNOTIF_POOL_SIZE ceiling                  */
    uint32_t kvmo_live;           /* KVmo pool slots in use                    */
    uint32_t kvmo_max;            /* KVMO_POOL_SIZE ceiling                    */
};  /* 64 bytes */

#endif /* !__ASSEMBLER__ */

#endif /* IRIS_DIAG_H */
