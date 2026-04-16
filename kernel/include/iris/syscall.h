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
 * Surface status:
 *   - modern/conforming: already on the v1 error model
 *   - modern/non-conforming: modern surface still requiring ABI cleanup
 *   - transitional: compatibility-preserving legacy contract scheduled to
 *     converge later; do not expand this style
 *   - legacy: retired or kernel-internal only
 */

/* Syscall numbers */
#define SYS_WRITE   0   /* transitional: returns byte count, generic -1 on bad user pointer */
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
#define SYS_VMO_MAP      17  /* (handle, virt_addr, flags) → 0 or negative iris_error_t */
/* SYS_SPAWN 18 retired in Phase 19 — permanently reserved, returns IRIS_ERR_NOT_SUPPORTED.
 * All userland process creation must go through SYS_SPAWN_SERVICE (31). */
#define SYS_SPAWN        18
/* modern/conforming: notification objects */
#define SYS_NOTIFY_CREATE 19 /* () → handle_id or negative iris_error_t */
#define SYS_NOTIFY_SIGNAL 20 /* (handle, bits) → 0 or negative iris_error_t */
#define SYS_NOTIFY_WAIT   21 /* (handle, *out_bits) → 0 or negative iris_error_t */
/* modern/conforming: handle management */
#define SYS_HANDLE_DUP      22  /* (src_handle, new_rights) → new_handle_id or negative iris_error_t
                                 *   Requires RIGHT_DUPLICATE on src_handle.
                                 *   new_rights must be a subset of existing rights.
                                 *   Pass RIGHT_SAME_RIGHTS to keep the same rights. */
#define SYS_HANDLE_TRANSFER 23  /* (src_handle, dest_proc_handle, new_rights) → new_handle_id
                                 *   or negative iris_error_t.
                                 *   Requires RIGHT_TRANSFER on src_handle.
                                 *   Requires RIGHT_MANAGE on dest_proc_handle.
                                 *   Consumes src_handle. new_rights ⊆ src rights. */
#define SYS_PROCESS_WATCH   29  /* (proc_handle, chan_handle, cookie) → 0 or negative iris_error_t
                                 *   Registers one process-exit watch for proc_handle.
                                 *   On death, the kernel sends PROC_EVENT_MSG_EXIT to
                                 *   chan_handle with the watched proc_handle id and cookie.
                                 *   Requires RIGHT_READ on proc_handle and RIGHT_WRITE on
                                 *   chan_handle. Single-subscriber per target process today. */
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

/*
 * Process-exit event delivered over KChannel by SYS_PROCESS_WATCH.
 *
 *   type                         PROC_EVENT_MSG_EXIT
 *   data[PROC_EVENT_OFF_HANDLE]  handle_id_t watched by the subscriber
 *   data[PROC_EVENT_OFF_COOKIE]  uint32_t subscriber-defined cookie
 */
#define PROC_EVENT_MSG_EXIT       0x90000001u
#define PROC_EVENT_OFF_HANDLE     0u
#define PROC_EVENT_OFF_COOKIE     4u
#define PROC_EVENT_MSG_LEN        8u
/*
 * Global kernel diagnostics snapshot — modern/conforming (iris_error_t).
 *
 * Atomically captures a compact snapshot of kernel-side state into the
 * caller-supplied user buffer.  Unrestricted: any task may call this.
 *
 * arg0 = user pointer to struct iris_diag_snapshot (IRIS_DIAG_SNAPSHOT_SIZE bytes).
 *        Must be writable user-space memory.
 * Returns 0 on success, negative iris_error_t on error.
 *
 * The snapshot covers: task count, KProcess pool usage, active IRQ routes,
 * and scheduler tick counter.  See iris/diag.h for the full layout.
 * For service-owned status (svcmgr/vfs/kbd), use the per-service STATUS
 * channels defined in svcmgr_proto.h / vfs_proto.h / kbd_proto.h.
 */
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

/*
 * ELF service spawning — modern/conforming (iris_error_t).
 *
 * SYS_SPAWN_SERVICE: spawn a named service from the kernel initrd.
 * Restricted by an explicit bootstrap capability handle.
 * Semantics mirror SYS_SPAWN but load the named ELF from the initrd
 * rather than executing a kernel-text function pointer.
 *
 *   arg0 = name_uptr   — user pointer to NUL-terminated service name (≤ 31 chars)
 *   arg1 = out_chan_ptr — user pointer to handle_id_t; filled with parent's
 *                        bootstrap channel handle on success (may be NULL)
 *   arg2 = auth_handle   — bootstrap capability handle with RIGHT_READ and
 *                         IRIS_BOOTCAP_SPAWN_SERVICE permission
 *   returns: proc_handle or negative iris_error_t
 *
 *   On success:
 *     - A new process is created from the named ELF in the initrd.
 *     - Child receives its bootstrap KChannel handle as arg0 (RBX).
 *     - Parent receives the other end of that channel via *out_chan_ptr.
 *     - Parent receives a proc_handle with
 *       RIGHT_READ|RIGHT_ROUTE|RIGHT_MANAGE|RIGHT_DUPLICATE.
 *   On failure: all resources are rolled back.
 *
 * Authority: auth_handle must resolve to a KOBJ_BOOTSTRAP_CAP with
 * IRIS_BOOTCAP_SPAWN_SERVICE permission.
 */
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

#ifndef __ASSEMBLER__
#ifdef __KERNEL__
void syscall_init(void);
void syscall_set_kstack(uint64_t kstack_top);

/* Called from ASM handler */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2);
#endif /* __KERNEL__ */
#endif /* __ASSEMBLER__ */

#endif
