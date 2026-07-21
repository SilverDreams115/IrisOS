#ifndef IRIS_SYSCALL_H
#define IRIS_SYSCALL_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/*
 * IRIS syscall ABI target v1
 *
 * External contract:
 *   - Every syscall returns a signed long value in the architecture ABI sense.
 *   - Success returns a non-negative value whose meaning is syscall-specific.
 *   - Failure returns a negative iris_error_t value.
 *
 * Current implementation note:
 *   - The kernel dispatcher still uses uint64_t internally because the return
 *     value is moved through raw register state.
 *   - Syscall implementations must nevertheless encode failures as negative
 *     iris_error_t values and must not introduce new generic -1 error returns
 *     outside explicitly transitional legacy paths.
 *
 * Surface status used in this header:
 *   - live/conforming: current supported surface on the v1 error model
 *   - live/transitional: current supported surface with compatibility notes
 *   - retired: permanently reserved; returns IRIS_ERR_NOT_SUPPORTED
 *
 * Current exported syscall number surface: 0..94.
 */

/* Syscall numbers */
/* SYS_WRITE 0 permanently retired in Phase 30 — returns IRIS_ERR_NOT_SUPPORTED.
 * Serial output is now handled by the ring-3 console service over its
 * KEndpoint ("console.ep", iris/console_ep_proto.h). */
#define SYS_WRITE   0
#define SYS_EXIT    1   /* modern/conforming: success path does not return */
#define SYS_GETPID  2   /* modern/conforming: returns pid >= 0 */
#define SYS_YIELD   3   /* modern/conforming: returns 0 on success */
/* Numbers 4, 5, 6 are permanently reserved; the dispatch returns
 * IRIS_ERR_NOT_SUPPORTED.  File I/O uses the VFS service over its KEndpoint
 * ("vfs.ep", iris/vfs_ep_proto.h). */
#define SYS_OPEN    4
#define SYS_READ    5
#define SYS_CLOSE   6
/* SYS_BRK 7 retired in Phase 20 — permanently reserved, returns IRIS_ERR_NOT_SUPPORTED.
 * All heap memory must be managed via SYS_VMO_CREATE + SYS_VMO_MAP. */
#define SYS_BRK     7
#define SYS_SLEEP   8   /* modern/conforming: returns 0 on success */
/* legacy: removed from user-space dispatch; internal use only */
/* SYS_IPC_CREATE  9  (retired) */
/* SYS_IPC_SEND   10  (retired) */
/* SYS_IPC_RECV   11  (retired) */
/* SYS_CHAN_CREATE(12)/SEND(13)/RECV(14) — retired in Fase 13/Track G with the
 * KChannel object.  Permanently reserved: the dispatch falls through to
 * IRIS_ERR_NOT_SUPPORTED.  Do not reuse 12-14.  Productive IPC is the KEndpoint
 * family (SYS_EP_SEND/RECV/CALL) + KNotification. */
#define SYS_CHAN_CREATE  12  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */
#define SYS_CHAN_SEND    13  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */
#define SYS_CHAN_RECV    14  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */
#define SYS_HANDLE_CLOSE 15  /* (handle) → 0 or negative iris_error_t */
/* modern/conforming: Virtual Memory Objects */
#define SYS_VMO_CREATE   16  /* (size) → handle_id or negative iris_error_t */
#define SYS_VMO_MAP      17  /* (handle, virt_addr, flags) → 0 or negative iris_error_t
                               * flags bit 0: MAP_WRITABLE  — map with PAGE_WRITABLE
                               * flags bit 1: MAP_EXEC      — map without PAGE_NX (executable)
                               * W^X enforced: bit 0 + bit 1 simultaneously → ERR_INVALID_ARG */
/* SYS_SPAWN 18 retired in Phase 19 — permanently reserved, returns
 * IRIS_ERR_NOT_SUPPORTED. Healthy-path process creation now uses the
 * composable primitives rooted in SYS_INITRD_VMO / SYS_PROCESS_CREATE /
 * SYS_VMO_MAP_INTO / SYS_THREAD_START / SYS_HANDLE_INSERT. */
#define SYS_SPAWN        18
/* Fase S1: SYS_NOTIFY_CREATE (19) RETIRED — returns IRIS_ERR_NOT_SUPPORTED.
 * Notifications are created via SYS_UNTYPED_RETYPE2 (Untyped storage, cap
 * directly in CSpace; no kslab, no per-process quota, no handle).  Number
 * permanently reserved. */
#define SYS_NOTIFY_CREATE 19 /* RETIRED (Fase S1) → IRIS_ERR_NOT_SUPPORTED */
#define SYS_NOTIFY_SIGNAL 20 /* (handle, bits) → 0 or negative iris_error_t */
#define SYS_NOTIFY_WAIT   21 /* (handle, *out_bits) → 0 or negative iris_error_t */
/* modern/conforming: handle management */
#define SYS_HANDLE_DUP      22  /* (src_handle, new_rights) → new_handle_id or negative iris_error_t
                                 *   Requires RIGHT_DUPLICATE on src_handle.
                                 *   new_rights must be a subset of existing rights.
                                 *   RIGHT_NONE is rejected.
                                 *   Pass RIGHT_SAME_RIGHTS to keep the same rights. */
#define SYS_HANDLE_TRANSFER 23  /* RETIRED A1.8 — permanently reserved, returns
                                 *   IRIS_ERR_NOT_SUPPORTED.  Zero in-tree
                                 *   callers; cross-process placement is
                                 *   SYS_PROC_CSPACE_MINT (CSpace-canonical)
                                 *   or an IPC receive-slot (A1.5/A1.6). */
#define SYS_PROCESS_WATCH   29  /* (proc_handle, notify_handle, signal_bits) → 0 or negative iris_error_t
                                 *   Registers one process-exit watch for proc_handle.
                                 *   On death, the kernel signals signal_bits on notify_handle
                                 *   (Fase 13 / Track B — a KNotification signal, no message).
                                 *   The watcher names the dead process by which bit is set and
                                 *   queries SYS_PROCESS_EXIT_CODE / STATUS for detail.
                                 *   Requires RIGHT_READ on proc_handle and RIGHT_WRITE on
                                 *   notify_handle. signal_bits must be non-zero. */
#define SYS_PROCESS_SELF    28  /* () → self proc_handle or negative iris_error_t
                                 *   Returns a handle to the caller's own KProcess with
                                 *   RIGHT_READ|RIGHT_DUPLICATE|RIGHT_TRANSFER.
                                 *   Intended for userland-owned lifecycle tracking such as
                                 *   service-side cleanup keyed to client process death. */
/*
 * Process lifecycle query — modern/conforming (iris_error_t).
 *
 * SYS_PROCESS_STATUS is a non-blocking query: it returns immediately.
 * Callers that do not have a process-exit watch path may still poll this.
 * Healthy-path supervision should prefer SYS_PROCESS_WATCH where practical;
 * this syscall remains the fallback compatibility query.
 */
#define SYS_PROCESS_STATUS  26  /* (proc_handle) → 1=alive, 0=dead, or negative iris_error_t
                                  *   Requires RIGHT_READ on proc_handle.
                                  *   Returns 0 when the process has called SYS_EXIT or been
                                  *   reaped; the handle itself remains valid for closing. */

/* Fase 13 (Track B): the legacy PROC_EVENT_MSG_EXIT KChannel event is retired.
 * Process death is now delivered as a KNotification signal — see
 * SYS_PROCESS_WATCH above. */
/* SYS_DIAG_SNAPSHOT 30 retired Phase 51 — permanently reserved, returns
 * IRIS_ERR_NOT_SUPPORTED.  Aggregated diagnostics are now provided entirely
 * through SVCMGR_MSG_DIAG over IPC; the kernel no longer exposes a raw
 * user-buffer snapshot path on the healthy boot surface. */
#define SYS_DIAG_SNAPSHOT 30

/*
 * I/O port access via KIoPort capability — modern/conforming (iris_error_t).
 *
 * Both syscalls resolve arg0 as a KIoPort handle and validate that
 * arg1 (port_offset) is within the authorized range (0 <= offset < cap->count).
 * The actual IN/OUT instruction executes in ring 0 on behalf of the caller.
 *
 * SYS_IOPORT_IN(ioport_handle, port_offset) → uint8_t value or negative iris_error_t
 *   Requires RIGHT_READ on ioport_handle.
 *   Returns the byte read in the low 8 bits of the result on success.
 *
 * SYS_IOPORT_OUT(ioport_handle, port_offset, value) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on ioport_handle.
 *   Writes the low byte of value to base_port + port_offset.
 */
#define SYS_IOPORT_IN  32
#define SYS_IOPORT_OUT 33

/*
 * SYS_CHAN_SEAL(37) — retired in Fase 13/Track G with the KChannel object.
 * Permanently reserved: the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.
 * Do not reuse 37.  (Service teardown now relies on KEndpoint close semantics,
 * which wake blocked peers with IRIS_ERR_CLOSED.)
 */
#define SYS_CHAN_SEAL  37  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * Synchronous channel call — modern/conforming (iris_error_t).
 *
 * SYS_CHAN_CALL(38) — retired in Fase 13/Track G (zero callers; the productive
 * request/reply path is the KEndpoint SYS_EP_CALL).  Permanently reserved:
 * the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.  Do not reuse 38.
 */
#define SYS_CHAN_CALL  38  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * Hardware capability creation — modern/conforming (iris_error_t).
 * Both syscalls require a bootstrap capability handle with IRIS_BOOTCAP_HW_ACCESS
 * permission.  Policy (which service gets which resource) lives in svcmgr; the
 * kernel only validates ranges and creates the cap object.
 *
 * SYS_CAP_CREATE_IRQCAP(auth_handle, irq_num) → handle_id_t or negative iris_error_t
 *   auth_handle: KOBJ_BOOTSTRAP_CAP with IRIS_BOOTCAP_HW_ACCESS.
 *   irq_num: hardware IRQ line (0–15).
 *   Returns a KIrqCap handle with RIGHT_ROUTE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 *
 * SYS_CAP_CREATE_IOPORT(auth_handle, base, count) → handle_id_t or negative iris_error_t
 *   auth_handle: KOBJ_BOOTSTRAP_CAP with IRIS_BOOTCAP_HW_ACCESS.
 *   base: first I/O port in the range (0–0xFFFF).
 *   count: number of consecutive ports (1–0x10000, base+count ≤ 0x10000).
 *   Returns a KIoPort handle with RIGHT_READ|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 */
#define SYS_CAP_CREATE_IRQCAP  39
#define SYS_CAP_CREATE_IOPORT  40

/* SYS_INITRD_LOOKUP 41 retired Phase 29 — permanently reserved, returns
 * IRIS_ERR_NOT_SUPPORTED.
 * SYS_SPAWN_ELF    42 retired Phase 29 — permanently reserved, returns
 * IRIS_ERR_NOT_SUPPORTED.
 * ELF loading is now performed entirely in ring-3 via the new primitives below
 * (SYS_INITRD_VMO / SYS_PROCESS_CREATE / SYS_VMO_MAP_INTO / SYS_THREAD_START /
 * SYS_HANDLE_INSERT). */
#define SYS_INITRD_LOOKUP  41
#define SYS_SPAWN_ELF      42

/*
 * Pure-microkernel process construction primitives — modern/conforming (iris_error_t).
 * These replace the kernel-side SYS_INITRD_LOOKUP + SYS_SPAWN_ELF with a set of
 * composable ring-3-usable primitives.  ELF parsing and loading happen in user
 * space; the kernel only exposes raw memory and process management operations.
 *
 * SYS_INITRD_VMO(auth_h, index) → vmo_handle or negative iris_error_t
 *   auth_h: KOBJ_BOOTSTRAP_CAP with IRIS_BOOTCAP_SPAWN_SERVICE.
 *   index: integer initrd catalog index (name→index mapping is a ring-3 concern).
 *   Returns a KOBJ_VMO (eager wrap, owned=0) covering the embedded ELF bytes.
 *   The VMO is read-only (RIGHT_READ only); map with flags=0 for read access.
 *   Physical pages are the kernel's initrd blob — never freed by this VMO.
 *
 * SYS_PROCESS_CREATE() → proc_handle or negative iris_error_t
 *   Creates an empty KProcess with a fresh user address space (new CR3).
 *   No threads are started; caller must call SYS_THREAD_START to begin execution.
 *   Returns proc handle with RIGHT_READ|RIGHT_WRITE|RIGHT_MANAGE|
 *           RIGHT_DUPLICATE|RIGHT_TRANSFER|RIGHT_ROUTE.
 *
 * SYS_VMO_MAP_INTO(vmo_h, proc_h, vaddr, flags) → 0 or negative iris_error_t
 *   vmo_h:   KOBJ_VMO with RIGHT_READ (plus RIGHT_WRITE for MAP_WRITABLE).
 *   proc_h:  KOBJ_PROCESS with RIGHT_MANAGE; must not be torn down.
 *   vaddr:   page-aligned target virtual address in proc's address space.
 *   flags:   bit 0 = MAP_WRITABLE, bit 1 = MAP_EXEC; W^X enforced.
 *   Eagerly allocates and maps all pages into proc's page table at call time.
 *   Also registers the mapping descriptor in proc's vmo_mappings for teardown.
 *   Uses 4-arg syscall ABI (arg3 = flags via r10).
 *
 * SYS_THREAD_START(proc_h, entry_vaddr, stack_top, boot_arg) → 0 or iris_error_t
 *   proc_h:      KOBJ_PROCESS with RIGHT_MANAGE; must have 0 live threads.
 *   entry_vaddr: user virtual address where the thread begins executing.
 *   stack_top:   initial RSP for the thread (caller-allocated stack).
 *   boot_arg:    value delivered to the thread in RBX on first execution.
 *   Uses 4-arg syscall ABI (arg3 = boot_arg via r10).
 *
 * SYS_HANDLE_INSERT(proc_h, obj_h, rights) → handle_id in child or iris_error_t
 *   proc_h:  KOBJ_PROCESS with RIGHT_MANAGE.
 *   obj_h:   any live handle with RIGHT_TRANSFER.
 *   rights:  effective rights in child = rights_reduce(obj_rights, rights); RIGHT_NONE rejected.
 *   Non-destructive: caller retains obj_h; child receives an independent handle slot.
 *   Returns the new handle_id as it appears in the child's handle table.
 *   DEPRECATED (A1.8): legacy compat producer — it plants persistent
 *   authority as a handle in the destination table.  No in-tree service
 *   uses it (T082 keeps it covered as the dual-resolver compat path).
 *   New code uses SYS_PROC_CSPACE_MINT or an IPC receive-slot instead.
 */
#define SYS_INITRD_VMO      55
#define SYS_PROCESS_CREATE  56
#define SYS_VMO_MAP_INTO    57
#define SYS_THREAD_START    58
#define SYS_HANDLE_INSERT   59

/*
 * Framebuffer VMO claim — modern/conforming (iris_error_t).
 *
 * SYS_FRAMEBUFFER_VMO(auth_h, info_uptr) → vmo_handle or negative iris_error_t
 *   auth_h:    KOBJ_BOOTSTRAP_CAP with IRIS_BOOTCAP_FRAMEBUFFER.
 *   info_uptr: user pointer to struct iris_fb_params (see iris/fb_info.h);
 *              filled with physical base, size, and pixel geometry on success.
 *   Returns a KOBJ_VMO (eager wrap, owned=0) covering the framebuffer region
 *   with RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 *   One-shot: the kernel clears the framebuffer-valid flag on first call so that
 *   only the first caller can claim the physical framebuffer window.
 */
#define SYS_FRAMEBUFFER_VMO 60

/*
 * Initrd catalog count — modern/conforming (iris_error_t).
 *
 * SYS_INITRD_COUNT(auth_h) → uint32_t count or negative iris_error_t
 *   auth_h: KOBJ_BOOTSTRAP_CAP with IRIS_BOOTCAP_SPAWN_SERVICE.
 *   Returns the number of entries in the kernel's initrd catalog.
 *   Ring-3 uses this at startup to verify its local name→index table is
 *   consistent with the kernel build.
 */
#define SYS_INITRD_COUNT    61

/*
 * Monotonic clock — modern/conforming (iris_error_t).
 *
 * SYS_CLOCK_GET() → uint64_t nanoseconds since boot, or negative iris_error_t.
 *   No arguments required.  Returns a monotonically increasing nanosecond
 *   timestamp.  When the TSC has been calibrated at boot (via PIT CH2 one-shot),
 *   the value is derived from RDTSC and carries sub-millisecond resolution.
 *   When calibration fails, the implementation falls back to the 100 Hz scheduler
 *   tick counter (10 ms resolution).  Safe to call from any ring-3 context; does
 *   not block.  Overflow wraps at UINT64_MAX.
 */
#define SYS_CLOCK_GET       62

/*
 * SYS_CHAN_RECV_TIMEOUT(63) — retired in Fase 13/Track G with the KChannel
 * object.  Permanently reserved: the dispatch falls through to
 * IRIS_ERR_NOT_SUPPORTED.  Do not reuse 63.  (Timed blocking now uses
 * SYS_NOTIFY_WAIT_TIMEOUT on a KNotification.)
 */
#define SYS_CHAN_RECV_TIMEOUT 63  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * Timed notification wait (Phase 43).
 *
 * SYS_NOTIFY_WAIT_TIMEOUT(notify_h, bits_uptr, timeout_ns) → 0 or negative iris_error_t.
 *   Blocks until the KNotification has at least one signal bit set, or until
 *   timeout_ns nanoseconds elapse.  Returns IRIS_ERR_TIMED_OUT if the deadline
 *   expires before any signal arrives.  bits_uptr receives the consumed bits on
 *   success (same as SYS_NOTIFY_WAIT).  Requires RIGHT_WAIT on notify_h.
 */
#define SYS_NOTIFY_WAIT_TIMEOUT 64

/*
 * Kernel boot-log drain — modern/conforming (iris_error_t).
 *
 * SYS_KLOG_DRAIN(buf_uptr, max_bytes) → bytes_copied or negative iris_error_t.
 *   Copies up to max_bytes bytes from the kernel boot-log buffer into the
 *   caller-supplied user buffer, then clears the buffer (destructive read).
 *   Returns the number of bytes actually copied (0 if the buffer is empty).
 *   Requires IRIS_BOOTCAP_KDEBUG.
 *   max_bytes must be > 0 and ≤ KLOG_BUF_SIZE (4096).
 */
#define SYS_KLOG_DRAIN 65

/*
 * I/O port sub-delegation — modern/conforming (iris_error_t).
 *
 * SYS_IOPORT_RESTRICT(ioport_h, offset, count) → new_handle_id or negative iris_error_t
 *   ioport_h: KOBJ_IOPORT with RIGHT_READ | RIGHT_DUPLICATE.
 *   offset: port offset within the parent range (0 ≤ offset < parent.count).
 *   count: ports in the sub-range (1 ≤ count; offset + count ≤ parent.count).
 *   Returns a new KOBJ_IOPORT handle for [parent.base + offset, +count) with
 *   RIGHT_READ|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 *   Use case: svcmgr delegates a narrow sub-range to a service that only needs
 *   one specific port within a wider authorized range.
 */
#define SYS_IOPORT_RESTRICT  43

/*
 * SYS_WAIT_ANY(44) — retired in Fase 13/Track G (zero callers).  Permanently
 * reserved: the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.  Do not reuse.
 */
#define SYS_WAIT_ANY  44  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * VMO unmap — modern/conforming (iris_error_t).
 *
 * SYS_VMO_UNMAP(vaddr, size) → 0 or negative iris_error_t
 *   Removes the virtual-to-physical mappings for [vaddr, vaddr+size) from the
 *   caller's address space.  Does NOT free the backing physical pages — those
 *   remain owned by the KVmo object and are released when the last handle to
 *   the VMO is closed.
 *
 *   Constraints:
 *     - vaddr and size must be page-aligned (4 KiB boundary).
 *     - [vaddr, vaddr+size) must lie entirely within [USER_VMO_BASE, USER_VMO_TOP).
 *     - Pages that are not currently mapped are silently skipped (idempotent).
 *     - No capability handle required: the caller owns their own address space.
 *
 *   Lifecycle contract:
 *     SYS_VMO_CREATE → SYS_VMO_MAP → (use) → SYS_VMO_UNMAP → SYS_HANDLE_CLOSE
 *     UNMAP removes the virtual alias; HANDLE_CLOSE triggers physical page free.
 */
#define SYS_VMO_UNMAP 36  /* (vaddr, size) → 0 or negative iris_error_t */

/*
 * SYS_CHAN_RECV_NB(34) — retired in Fase 13/Track G with the KChannel object.
 * Permanently reserved: the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.
 * Do not reuse 34.  (Non-blocking receive is now SYS_EP_NB_RECV on a KEndpoint.)
 */
#define SYS_CHAN_RECV_NB  34  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * Process termination — modern/conforming (iris_error_t).
 *
 * SYS_PROCESS_KILL(proc_handle) → 0 or negative iris_error_t
 *   Requires RIGHT_MANAGE on proc_handle.
 *   Forcibly tears down the target process: closes its handle table, frees
 *   its address space, removes it from the scheduler, and fires any registered
 *   exit watch (SYS_PROCESS_WATCH).
 *   Cannot be used for self-termination — use SYS_EXIT for that.
 *   Idempotent: returns 0 if the process is already dead.
 */
#define SYS_PROCESS_KILL  35  /* (proc_handle) → 0 or negative iris_error_t */

/* SYS_SPAWN_SERVICE 31 retired in Phase 22 — permanently reserved and returns
 * IRIS_ERR_NOT_SUPPORTED. Named image loading is now a ring-3 concern layered
 * over SYS_INITRD_VMO plus the process/VMO/thread primitives. */
#define SYS_SPAWN_SERVICE   31

#define SYS_IRQ_ROUTE_REGISTER 27 /* (irqcap_handle, chan_handle, proc_handle) → 0 or iris_error_t
                                   *   irqcap_handle: KOBJ_IRQ_CAP with RIGHT_ROUTE.
                                   *     The IRQ vector is embedded in the cap; callers cannot
                                   *     forge a different vector.
                                   *   chan_handle: KOBJ_CHANNEL with RIGHT_READ|RIGHT_WRITE.
                                   *     IRQ signals are delivered here.
                                   *   proc_handle: KOBJ_PROCESS with RIGHT_READ|RIGHT_ROUTE.
                                   *     Owns the route; kprocess_teardown auto-clears it. */

/* Numbers 24, 25 permanently reserved; dispatch returns IRIS_ERR_NOT_SUPPORTED.
 * Service discovery uses svcmgr IPC over its KEndpoint (endpoint_proto.h). */
#define SYS_NS_REGISTER     24
#define SYS_NS_LOOKUP       25

/* Bootstrap capability permission bits — used as arg1 for SYS_BOOTCAP_RESTRICT
 * and visible to userland so callers can reference symbolic constants. */
#define IRIS_BOOTCAP_NONE          0u
#define IRIS_BOOTCAP_SPAWN_SERVICE (1u << 0)
#define IRIS_BOOTCAP_HW_ACCESS     (1u << 1)
#define IRIS_BOOTCAP_KDEBUG        (1u << 2)  /* may call SYS_POWEROFF and SYS_KLOG_DRAIN */
#define IRIS_BOOTCAP_FRAMEBUFFER   (1u << 3)  /* may call SYS_FRAMEBUFFER_VMO (one-shot) */

/*
 * Bootstrap capability permission restriction — modern/conforming (iris_error_t).
 *
 * SYS_BOOTCAP_RESTRICT(cap_h, new_perms) → 0 or negative iris_error_t
 *   cap_h: KOBJ_BOOTSTRAP_CAP with RIGHT_READ.
 *   new_perms: IRIS_BOOTCAP_* bitmask; applied as: new_cap->permissions = old & new_perms.
 *   Cannot add permissions — only AND-reduces the existing set.
 *   The caller's handle is rebound to a restricted copy; aliased handles in
 *   other tables keep their original permissions.
 *   Use case: svcmgr strips IRIS_BOOTCAP_HW_ACCESS after claiming all hardware
 *   caps during bootstrap, so a compromised svcmgr cannot create new hw caps.
 */
#define SYS_BOOTCAP_RESTRICT  45

/*
 * VMO inter-process share — modern/conforming (iris_error_t).
 *
 * SYS_VMO_SHARE(vmo_h, dest_proc_h, rights) → new_handle_id or negative iris_error_t
 *   vmo_h: KOBJ_VMO with RIGHT_READ | RIGHT_DUPLICATE.
 *   dest_proc_h: KOBJ_PROCESS with RIGHT_MANAGE; target must be alive.
 *   rights: effective rights granted = rights_reduce(caller_vmo_rights, arg2).
 *   Does NOT consume vmo_h — non-destructive dup into dest's handle table.
 *   Returns the new handle_id in dest's table, visible to dest after next recv
 *   or when passed explicitly (e.g. via a channel notification from caller).
 *   RIGHT_NONE result is rejected with IRIS_ERR_INVALID_ARG.
 *   DEPRECATED (A1.8): legacy compat producer — it plants persistent
 *   authority as a handle in the destination table.  No in-tree service
 *   uses it (T080/T082/T098 keep it covered).  New code uses
 *   SYS_PROC_CSPACE_MINT into a destination CSpace slot instead.
 */
#define SYS_VMO_SHARE  46

/*
 * User-level exception handler registration — modern/conforming (iris_error_t).
 *
 * SYS_EXCEPTION_HANDLER(proc_h, notify_h, signal_bits) → 0 or negative iris_error_t
 *   proc_h: KOBJ_PROCESS with RIGHT_MANAGE, or HANDLE_INVALID for own process.
 *   notify_h: KOBJ_NOTIFICATION with RIGHT_WRITE.  signal_bits must be non-zero.
 *   Fase 13/Track I: on a ring-3 hardware exception the kernel records the fault
 *   details in the process and signals signal_bits on notify_h (no KChannel),
 *   then suspends the faulting task in TASK_BLOCKED_FAULT.  The handler reads the
 *   details with SYS_PROCESS_FAULT_INFO and resumes/kills via SYS_EXCEPTION_RESUME.
 *   If no handler is registered, the fault is logged and the task is killed.
 *   Re-registration replaces the previous handler (last registration wins).
 *   Fase 20: registering on an already-dead process fails IRIS_ERR_NOT_FOUND
 *   (nothing can fault, and the registration would leak the notification pin).
 *   Only ring-3 faults are deliverable; kernel faults are always fatal.
 */
#define SYS_EXCEPTION_HANDLER  47

/*
 * SYS_PROCESS_FAULT_INFO(proc_h, out_uptr) → 0 or negative iris_error_t
 *   proc_h: KOBJ_PROCESS with RIGHT_READ, or HANDLE_INVALID for own process.
 *   out_uptr: 32-byte user buffer filled per iris/fault_proto.h
 *             (FAULT_OFF_VECTOR/TASK_ID/RIP/ERROR/CR2).
 *   Returns IRIS_ERR_WOULD_BLOCK if no fault is pending.  Pairs with the
 *   exception-handler KNotification (SYS_EXCEPTION_HANDLER).
 *   Fase 20: a fault record lives exactly as long as the fault is pending —
 *   SYS_EXCEPTION_RESUME (either action) and process teardown clear it, so a
 *   resolved or dead-process query honestly reports WOULD_BLOCK.
 */
#define SYS_PROCESS_FAULT_INFO 105

/*
 * Exception resume — modern/conforming (iris_error_t).
 *
 * SYS_EXCEPTION_RESUME(proc_h, task_id, action) → 0 or negative iris_error_t
 *   proc_h:  KOBJ_PROCESS with RIGHT_MANAGE, or HANDLE_INVALID for own process.
 *   task_id: task ID from the FAULT_MSG_NOTIFY message.
 *   action:  0 = resume the task at the faulting RIP; 1 = kill the task.
 *   The target task must belong to the specified process and be in BLOCKED_FAULT.
 *   Returns IRIS_ERR_NOT_FOUND if no matching suspended task exists.
 *
 *   Fase 25 (additive): action 2 = resume, action 3 = kill, each with a fault
 *   generation check — bits [63:32] of the action argument must equal the
 *   fault_seq the caller read at FAULT_OFF_SEQ.  A generation of 0 is
 *   INVALID_ARG; a mismatch (the task refaulted since, or the caller replays
 *   a stale record) is NOT_FOUND with no side effect.  Values 2/3 were
 *   INVALID_ARG before Fase 25 — no existing caller changes behaviour.
 */
#define SYS_EXCEPTION_RESUME   66

/*
 * VMO size query — modern/conforming (iris_error_t).
 *
 * SYS_VMO_SIZE(vmo_h) → uint64_t byte size or negative iris_error_t
 *   vmo_h: KOBJ_VMO with RIGHT_READ.
 *   Returns the byte size of the VMO as it was created.
 *   For initrd VMOs this is the exact size of the embedded binary.
 *   For heap VMOs this is the value passed to SYS_VMO_CREATE.
 */
#define SYS_VMO_SIZE   67

/*
 * IRQ deferred ACK — modern/conforming (iris_error_t).
 *
 * SYS_IRQ_ACK(irqcap_h) → 0 or negative iris_error_t
 *   irqcap_h: KOBJ_IRQ_CAP with RIGHT_ROUTE.
 *   Unmasks the hardware IRQ line recorded in irqcap_h, re-enabling delivery
 *   to the registered KNotification.  Must be called after consuming the IRQ
 *   (reading hardware registers) to allow subsequent interrupts to fire.
 *
 *   seL4-style deferred ACK contract:
 *     1. Kernel masks the IRQ line and sends EOI to clear the PIC ISR bit.
 *     2. Kernel signals the route channel (SYS_IRQ_ACK is not needed for delivery).
 *     3. Ring-3 handler reads hardware (e.g. PS/2 port 0x60 via SYS_IOPORT_IN).
 *     4. Ring-3 calls SYS_IRQ_ACK to unmask so subsequent IRQs can fire.
 *
 *   If ring-3 never calls SYS_IRQ_ACK the IRQ line stays masked permanently.
 *   This gives the handler full control over IRQ delivery rate.
 */
#define SYS_IRQ_ACK    68

/*
 * Scheduler diagnostic snapshot — modern/conforming (iris_error_t).
 *
 * SYS_SCHED_INFO(buf_uptr, buf_size) → 0 or negative iris_error_t
 *   buf_uptr:  user pointer to a buffer of at least 40 bytes.
 *   buf_size:  byte size of the buffer; must be ≥ sizeof(struct iris_sched_info).
 *   Requires IRIS_BOOTCAP_KDEBUG.
 *   Fills the buffer with a snapshot of scheduler counters (see iris/sched_info.h).
 *   Returns 0 on success.
 *   A1.7 additive extension: buf_size >= 88 additionally fills handle-table
 *   and IPC-delivery diagnostics at offsets 40..87 (see syscall_diag.c for
 *   the field layout).  Callers passing 40..87 get the exact historical
 *   40-byte snapshot — no signature, number, or legacy-behavior change.
 */
#define SYS_SCHED_INFO 69

/*
 * Nanosleep — modern/conforming (iris_error_t).
 *
 * SYS_CLOCK_NANOSLEEP(duration_ns) → 0 or negative iris_error_t
 *   Suspends the calling task for approximately duration_ns nanoseconds.
 *   Resolution is one scheduler tick (10 ms at 100 Hz); durations shorter than
 *   one tick sleep for exactly one tick.  Passing 0 returns immediately (no sleep).
 *   Does not return IRIS_ERR_INTERRUPTED; always sleeps the full requested duration.
 */
#define SYS_CLOCK_NANOSLEEP 70

/*
 * Process exit code query — modern/conforming (iris_error_t).
 *
 * SYS_PROCESS_EXIT_CODE(proc_handle) → uint32_t exit_code or negative iris_error_t
 *   proc_handle: KOBJ_PROCESS with RIGHT_READ.
 *   Returns the exit code supplied to SYS_EXIT by the process.
 *   Returns IRIS_ERR_WOULD_BLOCK if the process is still alive.
 *   The exit code is 0 for processes terminated by SYS_PROCESS_KILL.
 */
#define SYS_PROCESS_EXIT_CODE 71

/*
 * SYS_WAIT_ANY_TIMEOUT(72) — retired in Fase 13/Track G (zero callers).
 * Permanently reserved: the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.
 */
#define SYS_WAIT_ANY_TIMEOUT 72  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * Thread creation — modern/conforming (iris_error_t).
 *
 * SYS_THREAD_CREATE(entry_vaddr, user_rsp, arg) → tid or negative iris_error_t
 *   entry_vaddr: user virtual address where the new thread begins executing.
 *   user_rsp:    user stack pointer for the new thread; must be 8-byte aligned.
 *                Caller is responsible for allocating a stack (e.g. via VMO).
 *   arg:         value delivered to the thread in RBX on first execution.
 *   Returns the new thread's task ID (≥ 0) on success.
 *   The new thread shares the caller's KProcess: same address space and
 *   handle table.  No new process is created.
 *   Use SYS_THREAD_EXIT or SYS_EXIT to terminate the thread.
 */
#define SYS_THREAD_CREATE  48

/*
 * Thread exit — modern/conforming (iris_error_t).
 *
 * SYS_THREAD_EXIT() — does not return.
 *   Exits the calling thread.  If this is the last thread in the process,
 *   the process is torn down (equivalent to SYS_EXIT).
 *   Pending blocked waits (IPC, futex) on this thread are cancelled.
 */
#define SYS_THREAD_EXIT  49

/*
 * Futex wait/wake — modern/conforming (iris_error_t).
 *
 * SYS_FUTEX_WAIT(uaddr, expected) → 0 or negative iris_error_t
 *   uaddr:    user pointer to a 4-byte aligned uint32_t in the caller's aspace.
 *   expected: the value the kernel checks at *uaddr before blocking.
 *   If *uaddr == expected, the calling thread blocks until woken by
 *   SYS_FUTEX_WAKE on the same uaddr.
 *   If *uaddr != expected, returns IRIS_ERR_WOULD_BLOCK immediately.
 *   Returns 0 on successful wake.
 *
 * SYS_FUTEX_WAKE(uaddr, count) → number of threads woken (≥ 0)
 *   uaddr: must be 4-byte aligned; identifies the wait queue.
 *   count: maximum number of waiting threads to wake.
 *   Returns the number of threads actually woken (0 if none were waiting).
 *   Does NOT access *uaddr — only uses the address as a wait queue key.
 */
#define SYS_FUTEX_WAIT  50
#define SYS_FUTEX_WAKE  51

/*
 * Handle inspection helpers — modern/conforming (iris_error_t).
 *
 * SYS_HANDLE_TYPE(handle) → kobject_type_t or negative iris_error_t
 *   Returns the underlying KObject type for a live handle in the caller's
 *   table. Useful for userland supervisors that need to narrow protocol
 *   behavior without dereferencing kernel objects.
 *
 * SYS_HANDLE_SAME_OBJECT(handle_a, handle_b) → 1 if both handles reference the
 * same KObject, 0 if they do not, or negative iris_error_t on failure.
 *   This is identity comparison only; it does not compare rights.
 */
#define SYS_HANDLE_TYPE        52
#define SYS_HANDLE_SAME_OBJECT 53

/*
 * Graceful system power-off — modern/conforming (iris_error_t).
 *
 * SYS_POWEROFF(type, arg0, arg1) → does not return on success, or negative iris_error_t.
 *   Requires IRIS_BOOTCAP_KDEBUG in the caller's bootstrap capability.
 *   type 0: ACPI S5 soft-off (writes 0x2000 to port 0x604; QEMU ACPI).
 *   type 1: legacy QEMU ISA debug exit (writes 0x01 to port 0xB004; any arg0/arg1 ignored).
 *   Any type not listed above returns IRIS_ERR_INVALID_ARG.
 */
#define SYS_POWEROFF           54

/*
 * Synchronous endpoint IPC — modern/conforming (iris_error_t).
 *
 * KEndpoint is a seL4-style rendezvous IPC primitive.  Unlike KChannel it has
 * no message queue: every send blocks until a receiver is ready (or vice versa).
 * Message delivery is atomic — both sides unblock in the same scheduler step.
 *
 * SYS_ENDPOINT_CREATE — RETIRED (Fase S1) → IRIS_ERR_NOT_SUPPORTED.
 *   Endpoints are created via SYS_UNTYPED_RETYPE2 (Untyped storage, cap
 *   directly in CSpace).  Number permanently reserved.
 *
 * SYS_EP_SEND(ep_h, msg_uptr) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE.  Blocks until a receiver is ready (rendezvous).
 *   msg_uptr: user pointer to struct IrisMsg (48 bytes).
 *   Returns IRIS_ERR_CLOSED if the endpoint is closed while blocked.
 *
 * SYS_EP_RECV(ep_h, msg_uptr) → 0 or negative iris_error_t
 *   Requires RIGHT_READ.  Blocks until a sender is ready (rendezvous).
 *   msg_uptr: user pointer to struct IrisMsg; filled with the received message.
 *   Returns IRIS_ERR_CLOSED if the endpoint is closed while blocked.
 *
 * SYS_EP_NB_SEND(ep_h, msg_uptr) → 0 or negative iris_error_t
 *   Non-blocking send: returns IRIS_ERR_WOULD_BLOCK immediately if no receiver
 *   is already waiting.  Otherwise identical to SYS_EP_SEND.
 *
 * SYS_EP_NB_RECV(ep_h, msg_uptr) → 0 or negative iris_error_t
 *   Non-blocking receive: returns IRIS_ERR_WOULD_BLOCK immediately if no sender
 *   is already waiting.  Otherwise identical to SYS_EP_RECV.
 */
#define SYS_ENDPOINT_CREATE 73
#define SYS_EP_SEND         74
#define SYS_EP_RECV         75
#define SYS_EP_NB_SEND      76
#define SYS_EP_NB_RECV      77

/*
 * CSpace — capability derivation and revocation (Phase 70-72).
 *
 * SYS_CAP_DERIVE(src_h, new_rights) → handle_id or negative iris_error_t
 *   Requires RIGHT_DUPLICATE on src_h.
 *   new_rights must be a non-empty subset of src_h's current rights.
 *   Returns a new handle to the same object with reduced rights.
 *   The new handle participates in the derivation tree: SYS_CAP_REVOKE on
 *   src_h will cascade-delete derived handles.
 *
 * SYS_CAP_REVOKE(h) → 0 or negative iris_error_t
 *   Deletes all handles transitively derived from h via SYS_CAP_DERIVE.
 *   h itself is NOT deleted and remains valid after the call.
 *   O(N) scan over the caller's handle table where N = HANDLE_TABLE_MAX.
 *
 * SYS_CNODE_CREATE — RETIRED (Fase S1) → IRIS_ERR_NOT_SUPPORTED.
 *   Runtime CNodes are created via SYS_UNTYPED_RETYPE2 (KOBJ_CNODE,
 *   obj_arg = num_slots).  Number permanently reserved.
 *
 * SYS_CNODE_MINT(cnode_h, slot_idx, src_h, new_rights) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on cnode_h; RIGHT_DUPLICATE on src_h.
 *   Mints a capability from src_h into KCNode slot slot_idx with new_rights
 *   (must be a non-empty subset of src_h's rights).
 *   If the slot was occupied the old capability reference is released first.
 *   Uses 4-arg syscall ABI (new_rights via r10).
 */
#define SYS_CAP_DERIVE    78
#define SYS_CAP_REVOKE    79
#define SYS_CNODE_CREATE  80
#define SYS_CNODE_MINT    81

/*
 * Block 3 — Scheduler (Phase 73-75).
 *
 * SYS_THREAD_PRIORITY(new_prio) → old_priority or negative iris_error_t
 *   Sets the calling thread's scheduling priority (0=lowest, 255=highest).
 *   Returns the previous priority on success.
 *   Default priority for all user threads is 128.  The idle task runs at 0.
 *
 * SYS_SC_CREATE — RETIRED (Fase S2) → IRIS_ERR_NOT_SUPPORTED.
 *   SchedulingContexts se crean vía SYS_UNTYPED_RETYPE2 (KOBJ_SCHED_CONTEXT,
 *   storage Untyped, cap en CSpace) y se configuran con SYS_SC_CONFIGURE.
 *   Número permanentemente reservado.
 *
 * SYS_SC_CONFIGURE(sc_h, budget_ticks, period_ticks) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on sc_h.
 *   budget_ticks: ticks the bound task may run per period (1 .. period_ticks-1).
 *   period_ticks: length of the period in scheduler ticks (> budget_ticks).
 *   Resets remaining_budget to budget_ticks immediately.
 *
 * SYS_THREAD_SET_SC(sc_h) → 0 or negative iris_error_t
 *   LEGACY FROZEN (Fase S2): self-bind del hilo llamante.  NO puede recibir
 *   consumidores nuevos — la ruta canónica de binding es SYS_SC_BIND(sc, tcb)
 *   por CPtr.  Conservado para código existente; one-to-one enforced (BUSY si
 *   sc_h ya está ligado a otra task).  Pass 0 to unbind.
 *
 * SYS_SC_BIND(sc_cptr, tcb_cptr) → 0 or negative iris_error_t   (Fase S2)
 *   Enlaza explícitamente un SchedulingContext a un TCB, ambos por CPtr,
 *   ambos vivos, uno-a-uno (BUSY si cualquiera ya está ligado a otro).  El SC
 *   debe estar configurado (SC_CONFIGURE).  tcb_cptr == 0 desliga el SC.
 *   Requiere RIGHT_WRITE en ambos.  Es el camino canónico de binding para la
 *   construcción de tareas desde userland (SYS_THREAD_SET_SC es el self-bind).
 */
#define SYS_THREAD_PRIORITY 82
#define SYS_SC_CREATE       83
#define SYS_SC_CONFIGURE    84
#define SYS_THREAD_SET_SC   85
#define SYS_SC_BIND        113

/*
 * Block 4 — Untyped Memory (Ph76-78)
 *
 * SYS_UNTYPED_INFO(ut_h, out_phys_uptr, out_avail_uptr) → 0 or error
 *   Writes phys_base and available bytes to the provided user pointers
 *   (either may be NULL to skip that field).
 *
 * SYS_UNTYPED_RETYPE(ut_h, obj_type, obj_arg) → handle_id or error
 *   LEGACY single-object retype returning a handle (Fase S1: TRANSITIONAL).
 *   Restricted to the non-migrated types: KOBJ_UNTYPED (obj_arg = sub-region
 *   bytes, page-aligned), KOBJ_FRAME (obj_arg = bytes, page-aligned) and
 *   KOBJ_SCHED_CONTEXT.  The migrated family (ENDPOINT / NOTIFICATION /
 *   REPLY / CNODE) returns IRIS_ERR_NOT_SUPPORTED here — those objects can
 *   only be born via SYS_UNTYPED_RETYPE2 (never through a handle: S20).
 */
#define SYS_UNTYPED_INFO   86
#define SYS_UNTYPED_RETYPE 87
#define SYS_UNTYPED_RESET  88

/* Fase S1: userland-visible object-type codes for SYS_UNTYPED_RETYPE(2).
 * ABI-stable mirrors of the kernel kobject_type_t enum (statically asserted
 * in syscall_untyped.c). */
#define IRIS_KOBJ_NOTIFICATION   2u
#define IRIS_KOBJ_ENDPOINT       8u
#define IRIS_KOBJ_CNODE          9u
#define IRIS_KOBJ_SCHED_CONTEXT 10u
#define IRIS_KOBJ_UNTYPED       11u
#define IRIS_KOBJ_REPLY         12u
#define IRIS_KOBJ_TCB           13u
#define IRIS_KOBJ_FRAME         15u

/*
 * Block 6 — CNode slot operations (Ph82-84).
 *
 * SYS_CNODE_MOVE(cnode_h, slot_idx, src_h) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on cnode_h.
 *   Moves the capability from the caller's HT handle src_h into CNode slot slot_idx.
 *   src_h is consumed (removed from HT) — seL4 Move semantics.
 *   If slot_idx was occupied, the old capability is released.
 *
 * SYS_CNODE_FETCH(cnode_h, slot_idx) → new_handle_id or negative iris_error_t
 *   Requires RIGHT_READ on cnode_h.
 *   Copies the CNode slot capability into a new HT handle entry.
 *   The CNode slot remains populated (non-destructive).
 *   Returns IRIS_ERR_NOT_FOUND if the slot is empty.
 *
 * SYS_CNODE_DELETE(cnode_h, slot_idx) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on cnode_h.
 *   Clears CNode slot slot_idx, releasing its capability reference.
 *   Idempotent: deleting an already-empty slot returns 0.
 *
 * SYS_CNODE_SWAP(cnode_h, slot_a, slot_b) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on cnode_h.
 *   Atomically swaps the contents of slot_a and slot_b within the same CNode.
 *   slot_a must not equal slot_b (returns IRIS_ERR_INVALID_ARG if they match).
 *   No capability references change — only slot pointers are exchanged.
 */
#define SYS_CNODE_MOVE    89
#define SYS_CNODE_FETCH   90
#define SYS_CNODE_DELETE  91
#define SYS_CNODE_SWAP    92

/*
 * Block 7 — Reply Capabilities (Ph85-87).
 *
 * SYS_EP_CALL(ep_h, msg_uptr) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on ep_h.
 *   Sends message on ep_h and blocks waiting for a reply via a KReply token.
 *   The kernel creates a KReply during rendezvous and delivers it to the
 *   receiver's handle table in msg.attached_handle.
 *   msg_uptr is both input (send message) and output (reply message).
 *   buf_uptr in the send message is used as the reply bulk destination.
 *   EP_CALL does NOT support simultaneous capability transfer.
 *   Returns IRIS_ERR_CLOSED if the endpoint is closed or the server drops
 *   the KReply handle without calling SYS_REPLY.
 *
 * SYS_REPLY(kreply_h, msg_uptr) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on kreply_h (a KOBJ_REPLY handle).
 *   Sends reply_msg to the blocked EP_CALL caller and unblocks it.
 *   One-shot: the KReply is consumed; future SYS_REPLY on same handle
 *   returns IRIS_ERR_NOT_FOUND.
 *   Does NOT require RIGHT_READ on kreply_h — server may hold write-only reply cap.
 *
 *   Reply-cap transfer (Fase 7.1 ABI extension): the reply MAY carry one
 *   capability in msg.attached_handle / msg.attached_rights, with the same
 *   staging semantics as SYS_EP_SEND (server handle needs RIGHT_TRANSFER and
 *   is consumed; rights are reduced by msg.attached_rights). The cap is
 *   installed in the EP_CALL caller's handle table; the caller observes the
 *   new handle id in msg.attached_handle after EP_CALL returns. On errors
 *   before staging (bad kreply_h, unreadable msg, stage validation failure)
 *   the server handle is NOT consumed; on IRIS_ERR_NOT_FOUND (KReply already
 *   invoked) the staged cap is destroyed and the handle IS consumed.
 *   Before Fase 7.1 the attached_handle field was ignored on replies.
 */
#define SYS_EP_CALL  93
#define SYS_REPLY    94

/*
 * Hierarchical CSpace traversal — modern/conforming (iris_error_t).
 *
 * SYS_CSPACE_RESOLVE(cptr) → handle_id or negative iris_error_t
 *   cptr: capability pointer into the process's CNode tree.
 *   Starting from the process root CNode (installed at creation), extracts
 *   ctzll(slot_count) bits per level to select a slot index, descends if the
 *   slot holds another CNode and bits remain, or materializes the leaf
 *   capability into a new flat handle-table entry and returns the handle_id.
 *   Max traversal depth: 8 levels.
 *   Returns IRIS_ERR_NOT_FOUND if the process has no root CNode or a slot
 *   is empty.  Returns IRIS_ERR_INVALID_ARG if the cptr exhausts all CNode
 *   levels without reaching a leaf.
 */
#define SYS_CSPACE_RESOLVE 95

/*
 * SYS_PROC_CSPACE_MINT(proc_h, slot_idx, src_h, rights_and_badge) → 0 or
 * negative iris_error_t.  Fase 8: CPtr-first bootstrap handoff.
 *
 *   Mints the caller's src_h capability into the ROOT CNode of the process
 *   referenced by proc_h, at slot slot_idx, with rights reduced to
 *   (src_rights & new_rights).  The child can then invoke the capability
 *   directly by CPtr (e.g. SYS_EP_CALL with arg0 = slot_idx) without any
 *   handle transfer over a KChannel.
 *
 *   Fase 9 packing: arg3 low 32 bits = rights mask; HIGH 32 bits = badge.
 *   Badge rules (sender identity, see iris/endpoint_proto.h):
 *     badge 0      → inherit the source cap's badge (preservation);
 *     badge != 0   → only if the source is UNBADGED and is an
 *                    endpoint/notification; re-badging an already-badged
 *                    cap fails ACCESS_DENIED (identities cannot be forged
 *                    by holders), wrong type fails INVALID_ARG.
 *
 *   Authority: caller needs RIGHT_WRITE on proc_h (spawner authority) and
 *   RIGHT_DUPLICATE on src_h.  Rights can only be reduced, never amplified.
 *   Fails NOT_FOUND if the target process has no root CNode; INVALID_ARG on
 *   slot 0 (null slot) or empty effective rights; ALREADY_EXISTS if the
 *   slot is occupied.
 */
#define SYS_PROC_CSPACE_MINT 104

/*
 * Block 9 — Frame capabilities (Fase 5 / 5.1).
 *
 * SYS_FRAME_MAP(frame_cptr, vspace_cptr, user_va, flags) → 0 or negative iris_error_t
 *   frame_cptr:  KOBJ_FRAME with RIGHT_READ (+ RIGHT_WRITE if flags bit 0 set).
 *   vspace_cptr: KOBJ_VSPACE with RIGHT_WRITE to install the PTE.  Fase 25:
 *                dual resolver (CPtr < 1024 or handle), same as the frame —
 *                a SYS_PROCESS_VSPACE handle works directly.
 *   user_va:     page-aligned target virtual address in the VSpace's address space.
 *                Must be in [USER_PRIVATE_BASE, USER_SPACE_TOP).
 *   flags:       bit 0 = MAP_WRITABLE, bit 1 = MAP_EXEC; W^X enforced.
 *   Maps one page from the Frame into the VSpace's page tables.
 *   Increments frame->mapped_count on success.
 *   Returns IRIS_ERR_BUSY if user_va is already mapped.
 *   Returns IRIS_ERR_INVALID_ARG for bad VA alignment, kernel address, bad flags.
 *   Returns IRIS_ERR_ACCESS_DENIED if required rights are absent.
 *   Returns IRIS_ERR_BAD_HANDLE if the VSpace has been invalidated (process dead).
 *   Returns IRIS_ERR_NO_MEMORY if page table allocation fails.
 *   Uses 4-arg syscall ABI (flags via r10).
 */
#define SYS_FRAME_MAP 102

/*
 * SYS_FRAME_UNMAP(frame_cptr, vspace_cptr, user_va) → 0 or negative iris_error_t
 *   Removes the PTE at user_va in the given VSpace, but only if it maps
 *   exactly this frame's physical page (paddr).  Decrements frame->mapped_count.
 *   Issues invlpg for the unmapped VA (TLB invalidation; sufficient for single-core).
 *
 *   frame_cptr:  KOBJ_FRAME with RIGHT_READ.
 *   vspace_cptr: KOBJ_VSPACE with RIGHT_WRITE (dual resolver since Fase 25).
 *   user_va:     page-aligned VA that was previously mapped via SYS_FRAME_MAP.
 *
 *   Returns IRIS_ERR_NOT_FOUND   — user_va has no PTE in this VSpace.
 *   Returns IRIS_ERR_INVALID_ARG — VA unaligned, or VA maps a different frame.
 *   Returns IRIS_ERR_BAD_HANDLE  — VSpace invalidated (process dead).
 *   Returns IRIS_ERR_ACCESS_DENIED if required rights are absent.
 *   Idempotent with respect to error: a failed unmap leaves mapped_count unchanged.
 */
#define SYS_FRAME_UNMAP 103

/*
 * SYS_VSPACE_SELF() → handle_id or negative iris_error_t   (Fase 19)
 *
 * Returns a new handle to the CALLER'S OWN VSpace (KOBJ_VSPACE) with
 * RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE.  Self-authority only: a process
 * already controls its own address space (via the VMO map/unmap syscalls), so
 * a cap to its own VSpace grants no new authority — it exists so the caller can
 * mint that cap into a CSpace slot and drive SYS_FRAME_MAP / SYS_FRAME_UNMAP on
 * itself by CPtr.  There is no argument and no way to name another process's
 * VSpace; cross-process address-space authority still requires an explicit
 * process capability (SYS_VMO_MAP_INTO with RIGHT_MANAGE).
 *
 *   Returns IRIS_ERR_INVALID_ARG if the caller has no address space.
 *   Returns IRIS_ERR_NO_MEMORY if the handle table is full.
 */
#define SYS_VSPACE_SELF 106

/*
 * SYS_PROCESS_VSPACE(proc_h) → handle_id or negative iris_error_t   (Fase 25)
 *
 * Returns a new handle to the TARGET process's VSpace (KOBJ_VSPACE) with
 * RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE.  Requires RIGHT_MANAGE on proc_h
 * (dual resolver, no fallback); HANDLE_INVALID names the caller itself and
 * is then equivalent to SYS_VSPACE_SELF.
 *
 * This is the map-into-target authority for a user pager: a supervisor that
 * already manages a process may take a cap to that process's address space
 * and mint it (RIGHT_WRITE suffices for SYS_FRAME_MAP) into a pager's CSpace.
 * It grants nothing MANAGE did not already imply — SYS_VMO_MAP_INTO has
 * always let a MANAGE holder install pages — but it makes the authority a
 * first-class, delegable, attenuable object capability instead of a
 * process-cap side effect.  No argument names a VSpace ambiently: the only
 * route to another process's VSpace remains an explicit process capability.
 *
 *   Returns IRIS_ERR_ACCESS_DENIED without RIGHT_MANAGE (no fallback).
 *   Returns IRIS_ERR_WRONG_TYPE if proc_h is not a process capability.
 *   Returns IRIS_ERR_BAD_HANDLE if the target has been torn down.
 *   Returns IRIS_ERR_INVALID_ARG if the target has no address space.
 *   Returns IRIS_ERR_NO_MEMORY if the handle table is full.
 */
#define SYS_PROCESS_VSPACE 107

/*
 * SYS_VMO_MAP_PAGE(vmo_cptr, vspace_cptr, target_va, offset_flags)
 *                                          → 0 or negative iris_error_t   (Fase 26)
 *
 * Maps exactly ONE page of a memory object at a chosen byte offset into a
 * VSpace at a chosen VA.  This is the page-granular, offset-addressed
 * primitive a VMO-backed user pager uses to resolve a fault: it is to
 * SYS_FRAME_MAP what a VMO page is to a raw frame — same authority shape
 * (the VSpace WRITE cap is the map-into-target authority, no process MANAGE),
 * composing directly with SYS_PROCESS_VSPACE (Fase 25).
 *
 *   vmo_cptr:     KOBJ_VMO with RIGHT_READ (+ RIGHT_WRITE if flags bit 0 set).
 *                 Dual resolver (CPtr slot or handle).
 *   vspace_cptr:  KOBJ_VSPACE with RIGHT_WRITE.  Dual resolver.
 *   target_va:    page-aligned VA in [USER_PRIVATE_BASE, USER_SPACE_TOP).
 *   offset_flags: bits [1:0]   = map flags (bit0 MAP_WRITABLE, bit1 MAP_EXEC;
 *                                W^X enforced);
 *                 bits [11:2]  = reserved, MUST be zero (rejects an unaligned
 *                                offset — the offset field is page-granular);
 *                 bits [63:12] = page-aligned byte offset into the VMO.
 *
 *   The addressed VMO page must lie within the VMO: offset < round_up(size),
 *   else INVALID_ARG.  For a sparse VMO the page is allocated and zeroed on
 *   first touch (eager; NO demand paging), charged to the CALLER's page quota.
 *   The installed PTE's rights never exceed the VMO cap's rights (RIGHT_READ
 *   without RIGHT_WRITE forbids a writable PTE).  mapped_count is incremented;
 *   the mapping (and the VMO retain behind it) is swept on target VSpace
 *   teardown exactly as for SYS_VMO_MAP_INTO.
 *
 *   Returns IRIS_ERR_INVALID_ARG   — bad VA/offset alignment, reserved bits set,
 *                                    bad flags, or offset beyond the VMO.
 *   Returns IRIS_ERR_ACCESS_DENIED — missing VMO/VSpace rights (no fallback).
 *   Returns IRIS_ERR_WRONG_TYPE    — wrong object in either slot.
 *   Returns IRIS_ERR_BAD_HANDLE    — VSpace invalidated (target dead).
 *   Returns IRIS_ERR_BUSY          — target_va already mapped.
 *   Returns IRIS_ERR_NO_MEMORY     — page/frame allocation or quota failure.
 *   Uses the 4-arg syscall ABI (offset_flags via r10).
 */
#define SYS_VMO_MAP_PAGE 108

/*
 * SYS_VMO_CREATE_FOR(size, charge_target) → handle_id or iris_error_t (Fase 29)
 *
 * Like SYS_VMO_CREATE, but the VMO OBJECT quota and its sparse physical pages
 * are charged to `charge_target` — a CPtr/handle to a KProcess the caller holds
 * RIGHT_MANAGE on — instead of to the caller.  The handle is installed in the
 * CALLER's table (holder), while the target is the OWNER/payer.  This lets a
 * loader create a child's image VMOs charged to the CHILD's resource domain, so
 * the loader's own quota stays flat regardless of how many children it launches
 * (Fase 29 root-cause fix for caller-charged accounting).
 *
 *   Returns IRIS_ERR_ACCESS_DENIED — missing RIGHT_MANAGE on charge_target.
 *   Returns IRIS_ERR_WRONG_TYPE    — charge_target is not a KProcess.
 *   Returns IRIS_ERR_BAD_HANDLE    — charge_target is torn down.
 *   Returns IRIS_ERR_NO_MEMORY     — object alloc or the target's quota is full.
 */
#define SYS_VMO_CREATE_FOR 109

/*
 * SYS_RESOURCE_INFO(proc_h, out_ptr) → 0 or iris_error_t (Fase 29)
 *
 * Read-only resource-accounting snapshot for a process (its resource domain).
 * proc_h == HANDLE_INVALID → self; otherwise a KProcess cap (any rights: the
 * snapshot is a read-only oracle).  Writes a struct iris_resource_info (see
 * iris/syscall.h) to out_ptr: per-type usage / limit / high-water for the
 * process, plus global failed-charge / rollback / kslab counters.  Additive,
 * size-validated, versioned.
 */
#define SYS_RESOURCE_INFO 110

/*
 * Fase S1 — seL4 Architectural Convergence.
 *
 * SYS_UNTYPED_RETYPE2(ut, type|count<<32, dest_cnode|slot<<32, obj_arg)
 *     → 0 or negative iris_error_t
 *
 *   The canonical object-creation path.  Storage for the new object(s) IS the
 *   retyped untyped memory (header + payload live inside the source region);
 *   the created capabilities are published DIRECTLY into CSpace destination
 *   slots — no handle, no per-process quota, no hidden allocator.
 *
 *   arg0 = source KUntyped (CPtr < 1024 or handle >= 1024); RIGHT_WRITE.
 *   arg1 = obj_type (low 32) | object count (high 32; 0 → 1; max 32).
 *   arg2 = destination CNode (low 32; 0 → caller's root CNode; RIGHT_WRITE)
 *          | first destination slot (high 32).  Slots [slot, slot+count) must
 *          be empty (IRIS_ERR_ALREADY_EXISTS otherwise); slot 0 is refused.
 *   arg3 = obj_arg: KOBJ_CNODE → num_slots (power of 2, ≤ KCNODE_MAX_SLOTS);
 *          KOBJ_UNTYPED / KOBJ_FRAME → bytes (page-aligned, count must be 1);
 *          otherwise 0.
 *
 *   Batch is atomic (U14/U15): on any failure no capability is published, no
 *   object is live and no untyped range is consumed.
 *   Device untyped only produces KOBJ_UNTYPED / KOBJ_FRAME (U11/U12).
 *
 *   KOBJ_TCB (Fase S2 Etapa 0): the created TCB is a canonical, INACTIVE
 *   object — storage inside the untyped, capability in CSpace, observable
 *   (TCB_GET_INFO: state = SUSPENDED, task_id = 0) and delegable, but NOT
 *   runnable: it has no kstack, no registry slot and no process.  Execution
 *   syscalls (TCB_SUSPEND/RESUME/EXIT, SC_BIND) refuse it with
 *   IRIS_ERR_NOT_SUPPORTED until TCB_CONFIGURE lands (roadmap Etapa 5/6);
 *   TCB_SET_PRIORITY works (stored for later, seL4-style inactive TCB).
 *   Deleting the last capability returns the zeroed block to the untyped.
 *
 * SYS_UNTYPED_QUERY(kind|version<<16|size<<32, buf_uptr, ut) → 0 or error
 *   Read-only, versioned instrumentation (never authority).  Fase S2 C.1:
 *   arg0 packs the caller-declared version (bits 16..31, 0 = don't-care) and
 *   buffer size (high 32).  The kernel writes at most min(size, kernel_size)
 *   bytes (prefix-compatible) and never past the declared buffer; size below
 *   the 8-byte header, or an unsupported version, returns IRIS_ERR_INVALID_ARG
 *   without writing.  kinds:
 *     kind 1 (GLOBAL)  — struct iris_untyped_query_global.
 *     kind 2 (ONE)     — struct iris_untyped_query_one for the untyped in arg2
 *                        (RIGHT_READ).
 *     kind 3 (OBJECTS) — struct iris_untyped_query_objects (live gauges for
 *                        the migrated object family).
 *     kind 4 (TASKOBJ) — struct iris_untyped_query_taskobj (TCB/SC/CDT/registry).
 */
#define SYS_UNTYPED_RETYPE2 111
#define SYS_UNTYPED_QUERY   112

#define IRIS_UNTYPED_QUERY_VERSION 1u
#define IRIS_UNTYPED_QUERY_GLOBAL  1u
#define IRIS_UNTYPED_QUERY_ONE     2u
#define IRIS_UNTYPED_QUERY_OBJECTS 3u
#define IRIS_UNTYPED_QUERY_TASKOBJ 4u  /* Fase S2: TCB/SC gauges + CDT counters */

#ifndef __ASSEMBLER__
struct iris_untyped_query_global {
    uint32_t version;          /* IRIS_UNTYPED_QUERY_VERSION */
    uint32_t struct_size;
    uint32_t live_untypeds;    /* live KUntyped objects (incl. sub-untypeds) */
    uint32_t _pad0;
    uint64_t retype_count;     /* objects successfully created by RETYPE/RETYPE2 */
    uint64_t retype_failures;  /* denied/failed retype operations */
    uint64_t reset_count;      /* successful SYS_UNTYPED_RESET calls */
    uint64_t reclaimed_bytes;  /* bytes returned to reusable state by RESET */
    uint64_t reuse_count;      /* RESETs that reclaimed a consumed region */
    uint64_t overlap_denials;  /* occupied-slot / range denials */
};

struct iris_untyped_query_one {
    uint32_t version;
    uint32_t struct_size;
    uint64_t phys_base;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t generation;       /* bumped on every successful RESET */
    uint32_t child_count;
    uint32_t is_device;
};

struct iris_untyped_query_objects {
    uint32_t version;
    uint32_t struct_size;
    uint32_t endpoints_live;
    uint32_t notifications_live;
    uint32_t replies_live;
    uint32_t cnodes_live;
};

/* Fase S2 — task-object gauges + CSpace-native derivation counters. */
struct iris_untyped_query_taskobj {
    uint32_t version;
    uint32_t struct_size;
    uint32_t tcb_live;
    uint32_t tcb_hwm;
    uint32_t tcb_retyped;
    uint32_t tcb_destroyed;
    uint32_t sc_live;
    uint32_t sc_hwm;
    uint32_t sc_retyped;
    uint32_t sc_destroyed;
    /* CSpace-native derivation tree (MDB) — 0 for handle-tree legacy. */
    uint32_t cdt_derivation_count;      /* mint/copy/derive descendants created */
    uint32_t cdt_derivation_hwm;
    uint32_t cdt_revoke_count;
    uint32_t cdt_delete_count;
    uint32_t cdt_cross_cnode_descendants;
    uint32_t cdt_ipc_transfer_count;
    /* Legacy handle-tree derivations for the migrated canonical types — must
     * be provably 0 (TCB/SC/CNode/EP/Notif/Reply). */
    uint32_t legacy_handle_derivation_migrated;
    /* Fase S2 Etapa C — KTCB registry (references, not payload). */
    uint32_t tcb_registry_active;
    uint32_t tcb_registry_hwm;
    uint32_t tcb_registry_exhaustions;
    uint32_t tcb_registry_generation_mismatch;
};
#endif /* !__ASSEMBLER__ */

/*
 * Block 8 — TCB capabilities (Ph96-101).
 *
 * Each user thread receives a KTcb at creation time; handles are installed
 * in the owning process's handle table automatically.
 *
 * SYS_TCB_SELF() → handle_id or negative iris_error_t
 *   Returns a new handle to the calling thread's KTcb with
 *   RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 *
 * SYS_TCB_SUSPEND(tcb_h) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE.  Transitions the target thread to TASK_SUSPENDED
 *   and removes it from the run queue.  If the caller suspends itself the
 *   syscall yields before returning; execution resumes after SYS_TCB_RESUME.
 *   Returns IRIS_ERR_NOT_FOUND if the thread is already dead.
 *
 * SYS_TCB_RESUME(tcb_h) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE.  Transitions a TASK_SUSPENDED thread to TASK_READY.
 *   No-op if the thread is already runnable.
 *   Returns IRIS_ERR_NOT_FOUND if the thread is dead.
 *
 * SYS_TCB_SET_PRIORITY(tcb_h, prio) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE.  Sets the thread scheduling priority (0–255).
 *   Returns IRIS_ERR_NOT_FOUND if the thread is dead.
 *
 * SYS_TCB_EXIT(tcb_h) → 0 or negative iris_error_t; does not return for self.
 *   Requires RIGHT_WRITE.  Forcibly terminates the target thread.
 *   If the target is the caller, equivalent to SYS_EXIT (does not return).
 *
 * SYS_TCB_GET_INFO(tcb_h, info_uptr) → 0 or negative iris_error_t
 *   Requires RIGHT_READ.  Writes struct iris_tcb_info at info_uptr.
 */
#define SYS_TCB_SELF          96
#define SYS_TCB_SUSPEND       97
#define SYS_TCB_RESUME        98
#define SYS_TCB_SET_PRIORITY  99
#define SYS_TCB_EXIT         100
#define SYS_TCB_GET_INFO     101

#ifndef __ASSEMBLER__
struct iris_tcb_info {
    uint32_t task_id;
    uint8_t  priority;
    uint8_t  state;    /* task_state_t cast to uint8_t */
    uint8_t  _pad[2];
};
#endif

#define IRIS_HANDLE_TYPE_PROCESS        0u
#define IRIS_HANDLE_TYPE_CHANNEL        1u
#define IRIS_HANDLE_TYPE_NOTIFICATION   2u
#define IRIS_HANDLE_TYPE_BOOTSTRAP_CAP  3u
#define IRIS_HANDLE_TYPE_VMO            4u
#define IRIS_HANDLE_TYPE_IRQ_CAP        5u
#define IRIS_HANDLE_TYPE_IOPORT         6u
#define IRIS_HANDLE_TYPE_INITRD_ENTRY   7u
#define IRIS_HANDLE_TYPE_ENDPOINT       8u
#define IRIS_HANDLE_TYPE_CNODE          9u
#define IRIS_HANDLE_TYPE_SCHED_CONTEXT  10u
#define IRIS_HANDLE_TYPE_UNTYPED        11u
#define IRIS_HANDLE_TYPE_REPLY          12u
#define IRIS_HANDLE_TYPE_TCB            13u
#define IRIS_HANDLE_TYPE_VSPACE         14u  /* Fase 4: KVSpace — virtual address space */
#define IRIS_HANDLE_TYPE_FRAME          15u  /* Fase 5: KFrame  — physical memory frame */

/*
 * Fase 29 — resource-accounting snapshot (SYS_RESOURCE_INFO out payload).
 * Additive, versioned; a caller sets `struct_size = sizeof(*this)` and the
 * kernel fills up to that many bytes (older callers with a smaller struct still
 * work).  Per-type fields are for the queried process (its resource domain);
 * the global_* / kslab_* fields are system-wide gauges.
 */
#define IRIS_RESOURCE_INFO_VERSION 1u
#ifndef __ASSEMBLER__
struct iris_resource_info {
    uint32_t version;         /* IRIS_RESOURCE_INFO_VERSION */
    uint32_t struct_size;     /* caller-provided sizeof; kernel clamps its write */
    /* per-process (domain) usage / limit / high-water */
    uint32_t vmos_usage;
    uint32_t vmos_limit;
    uint32_t vmos_hwm;
    uint32_t notifs_usage;
    uint32_t notifs_limit;
    uint32_t notifs_hwm;
    uint32_t pages_usage;
    uint32_t pages_limit;
    uint32_t pages_hwm;
    /* system-wide accounting gauges */
    uint32_t global_failed_charges;  /* charges rejected at a domain limit */
    uint32_t global_rollbacks;       /* provisional charges rolled back on failure */
    uint32_t kslab_used_bytes;
    uint32_t kslab_total_bytes;
    uint32_t kslab_hwm_bytes;
    uint32_t kslab_alloc_failures;
};
#endif /* __ASSEMBLER__ */

#ifndef __ASSEMBLER__
#ifdef __KERNEL__
void syscall_init(void);
void syscall_set_kstack(uint64_t kstack_top);
void syscall_set_user_cr3(uint64_t val);

/* Called from ASM handler — 5 params: num + 4 user args (arg3 via r10) */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3);
#endif /* __KERNEL__ */
#endif /* __ASSEMBLER__ */

#endif
