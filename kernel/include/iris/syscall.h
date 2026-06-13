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
 * Serial output is now handled by the ring-3 console service over KChannel. */
#define SYS_WRITE   0
#define SYS_EXIT    1   /* modern/conforming: success path does not return */
#define SYS_GETPID  2   /* modern/conforming: returns pid >= 0 */
#define SYS_YIELD   3   /* modern/conforming: returns 0 on success */
/* Numbers 4, 5, 6 are permanently reserved; the dispatch returns
 * IRIS_ERR_NOT_SUPPORTED.  File I/O uses the VFS service over KChannel. */
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
/* modern/conforming: capability IPC over KObjects/handles */
#define SYS_CHAN_CREATE  12  /* () → handle_id or negative iris_error_t */
#define SYS_CHAN_SEND    13  /* (handle, msg_uptr) → 0 or negative iris_error_t */
#define SYS_CHAN_RECV    14  /* (handle, msg_uptr) → 0 or negative iris_error_t */
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
/* live/conforming: notification objects */
#define SYS_NOTIFY_CREATE 19 /* () → handle_id or negative iris_error_t */
#define SYS_NOTIFY_SIGNAL 20 /* (handle, bits) → 0 or negative iris_error_t */
#define SYS_NOTIFY_WAIT   21 /* (handle, *out_bits) → 0 or negative iris_error_t */
/* modern/conforming: handle management */
#define SYS_HANDLE_DUP      22  /* (src_handle, new_rights) → new_handle_id or negative iris_error_t
                                 *   Requires RIGHT_DUPLICATE on src_handle.
                                 *   new_rights must be a subset of existing rights.
                                 *   RIGHT_NONE is rejected.
                                 *   Pass RIGHT_SAME_RIGHTS to keep the same rights. */
#define SYS_HANDLE_TRANSFER 23  /* (src_handle, dest_proc_handle, new_rights) → new_handle_id
                                 *   or negative iris_error_t.
                                 *   Requires RIGHT_TRANSFER on src_handle.
                                 *   Requires RIGHT_MANAGE on dest_proc_handle.
                                 *   Consumes src_handle. new_rights ⊆ src rights.
                                 *   RIGHT_NONE is rejected. */
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
 * Channel seal — modern/conforming (iris_error_t).
 *
 * SYS_CHAN_SEAL(chan_handle) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on chan_handle.
 *   Immediately marks the channel closed and wakes all blocked receivers,
 *   which return IRIS_ERR_CLOSED.  Future sends also return IRIS_ERR_CLOSED.
 *   Already-buffered messages can still be drained by existing receivers.
 *   The handle itself remains valid and must still be closed with SYS_HANDLE_CLOSE.
 *   Idempotent: sealing an already-sealed channel returns 0.
 *
 *   Primary use case: supervisor (svcmgr) poisons old service channels before
 *   restarting a crashed service, ensuring stale client handles fail fast instead
 *   of silently queuing to a dead endpoint.
 */
#define SYS_CHAN_SEAL  37  /* (chan_handle) → 0 or negative iris_error_t */

/*
 * Synchronous channel call — modern/conforming (iris_error_t).
 *
 * SYS_CHAN_CALL(req_chan, msg_uptr, reply_chan) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on req_chan and RIGHT_READ on reply_chan.
 *   Sends the message at *msg_uptr on req_chan, then blocks on reply_chan
 *   until a reply arrives; the reply overwrites *msg_uptr in place.
 *
 *   Limitation: the outbound request may not carry an attached handle
 *   (msg->attached_handle is ignored / forced to HANDLE_INVALID).
 *   The inbound reply may carry an attached handle, which is installed in
 *   the caller's handle table and written to msg->attached_handle on return.
 *
 *   Lifecycle: req_chan and reply_chan are NOT consumed; both handles remain
 *   valid after the call.  The caller is responsible for closing them.
 *
 *   This is a convenience primitive — the equivalent of:
 *     SYS_CHAN_SEND(req_chan, msg) + SYS_CHAN_RECV(reply_chan, msg)
 *   but in a single syscall to minimize round-trips through ring-0.
 */
#define SYS_CHAN_CALL  38  /* (req_chan, msg_uptr, reply_chan) → 0 or negative iris_error_t */

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
 * Timed channel receive (Phase 42).
 *
 * SYS_CHAN_RECV_TIMEOUT(chan_h, msg_uptr, timeout_ns) → 0 or negative iris_error_t.
 *   Blocks until a message is available on chan_h or timeout_ns nanoseconds
 *   have elapsed since the call.  Resolution is one scheduler tick (10 ms at
 *   100 Hz); timeouts are rounded up to the next tick boundary.
 *   Returns IRIS_ERR_TIMED_OUT (-15) if the deadline expires before a message
 *   arrives.  All other semantics are identical to SYS_CHAN_RECV.
 *   Requires RIGHT_READ on chan_h.
 */
#define SYS_CHAN_RECV_TIMEOUT 63

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
 * Multi-channel readable wait — modern/conforming (iris_error_t).
 *
 * SYS_WAIT_ANY(handles_uptr, count, out_index_uptr) → 0 or negative iris_error_t
 *   handles_uptr: user pointer to an array of count handle_id_t values (max 64).
 *   count: number of handles to watch (1–64); each must be KOBJ_CHANNEL + RIGHT_READ.
 *   out_index_uptr: user pointer to uint32_t; receives the 0-based index of the
 *                   first channel found to have a pending message.
 *   Blocks until at least one channel has a message; does NOT consume the message.
 *   Caller must follow up with SYS_CHAN_RECV / SYS_CHAN_RECV_NB to read it.
 *   Returns IRIS_ERR_CLOSED if any watched channel is sealed while waiting.
 */
#define SYS_WAIT_ANY  44

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
 * Non-blocking channel receive — modern/conforming (iris_error_t).
 *
 * SYS_CHAN_RECV_NB(chan_handle, msg_uptr) → 0 or negative iris_error_t
 *   Requires RIGHT_READ on chan_handle.
 *   Returns IRIS_ERR_WOULD_BLOCK immediately when the channel is empty instead
 *   of blocking the calling task.  All other semantics (attached handle
 *   installation, closed-channel detection) are identical to SYS_CHAN_RECV.
 */
#define SYS_CHAN_RECV_NB  34  /* (handle, msg_uptr) → 0 or negative iris_error_t */

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
 * Service discovery uses svcmgr IPC over KChannel. */
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
 */
#define SYS_VMO_SHARE  46

/*
 * User-level exception handler registration — modern/conforming (iris_error_t).
 *
 * SYS_EXCEPTION_HANDLER(proc_h, chan_h) → 0 or negative iris_error_t
 *   proc_h: KOBJ_PROCESS with RIGHT_MANAGE, or HANDLE_INVALID for own process.
 *   chan_h: KOBJ_CHANNEL with RIGHT_WRITE; receives FAULT_MSG_NOTIFY messages.
 *   Registers chan_h as the exception handler channel for the process.
 *   Replaces any previously registered handler.
 *   On a ring-3 hardware exception the kernel sends a FAULT_MSG_NOTIFY message
 *   (see iris/fault_proto.h) and suspends the faulting task in TASK_BLOCKED_FAULT.
 *   The handler must call SYS_EXCEPTION_RESUME to resume or kill the faulting task.
 *   If no handler is registered, faults kill the task silently (current behaviour).
 */
#define SYS_EXCEPTION_HANDLER  47

/*
 * Exception resume — modern/conforming (iris_error_t).
 *
 * SYS_EXCEPTION_RESUME(proc_h, task_id, action) → 0 or negative iris_error_t
 *   proc_h:  KOBJ_PROCESS with RIGHT_MANAGE, or HANDLE_INVALID for own process.
 *   task_id: task ID from the FAULT_MSG_NOTIFY message.
 *   action:  0 = resume the task at the faulting RIP; 1 = kill the task.
 *   The target task must belong to the specified process and be in BLOCKED_FAULT.
 *   Returns IRIS_ERR_NOT_FOUND if no matching suspended task exists.
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
 *   to the registered KChannel.  Must be called after consuming the IRQ
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
 * Timed multi-channel wait — modern/conforming (iris_error_t).
 *
 * SYS_WAIT_ANY_TIMEOUT(handles_uptr, count, out_index_uptr, timeout_ns) → 0 or iris_error_t
 *   Same semantics as SYS_WAIT_ANY plus a deadline expressed in nanoseconds.
 *   Returns IRIS_ERR_TIMED_OUT (-15) if no channel becomes readable within timeout_ns.
 *   timeout_ns == 0 is equivalent to a non-blocking scan (no sleep).
 *   Uses 4-arg syscall ABI (arg3 = timeout_ns via r10).
 */
#define SYS_WAIT_ANY_TIMEOUT 72

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
 * SYS_ENDPOINT_CREATE() → handle_id or negative iris_error_t
 *   Creates a new KEndpoint with RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
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
 * SYS_CNODE_CREATE(num_slots) → handle_id or negative iris_error_t
 *   Creates a KCNode with num_slots (1..KCNODE_MAX_SLOTS=64) empty slots.
 *   Returns a handle with RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
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
 * SYS_SC_CREATE() → handle_id or negative iris_error_t
 *   Creates a KSchedContext with default budget (5 ticks) and period (20 ticks).
 *   Returns handle with RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 *
 * SYS_SC_CONFIGURE(sc_h, budget_ticks, period_ticks) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on sc_h.
 *   budget_ticks: ticks the bound task may run per period (1 .. period_ticks-1).
 *   period_ticks: length of the period in scheduler ticks (> budget_ticks).
 *   Resets remaining_budget to budget_ticks immediately.
 *
 * SYS_THREAD_SET_SC(sc_h) → 0 or negative iris_error_t
 *   Binds sc_h to the calling thread.  Pass 0 to unbind.
 *   When bound, the thread's remaining_budget is decremented each scheduler
 *   tick; when exhausted the thread is suspended (TASK_BUDGET_EXHAUSTED)
 *   until the next refill at remaining_budget + period_ticks.
 */
#define SYS_THREAD_PRIORITY 82
#define SYS_SC_CREATE       83
#define SYS_SC_CONFIGURE    84
#define SYS_THREAD_SET_SC   85

/*
 * Block 4 — Untyped Memory (Ph76-78)
 *
 * SYS_UNTYPED_INFO(ut_h, out_phys_uptr, out_avail_uptr) → 0 or error
 *   Writes phys_base and available bytes to the provided user pointers
 *   (either may be NULL to skip that field).
 *
 * SYS_UNTYPED_RETYPE(ut_h, obj_type, obj_arg) → handle_id or error
 *   Carves memory from the untyped region and creates a typed kernel object
 *   without touching the kernel heap (kpage-free path for typed objects).
 *   obj_arg: for KOBJ_UNTYPED  = size of sub-region in bytes (page-aligned)
 *             for KOBJ_CNODE   = num_slots (NOT YET — use SYS_CNODE_CREATE)
 *             otherwise        = 0
 *   Supported obj_type values: KOBJ_ENDPOINT(8), KOBJ_NOTIFICATION(2), KOBJ_UNTYPED(11)
 */
#define SYS_UNTYPED_INFO   86
#define SYS_UNTYPED_RETYPE 87
#define SYS_UNTYPED_RESET  88

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
 *   vspace_cptr: KOBJ_VSPACE with RIGHT_WRITE to install the PTE.
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
 *   vspace_cptr: KOBJ_VSPACE with RIGHT_WRITE.
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
