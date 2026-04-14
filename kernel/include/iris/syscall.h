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
#define SYS_OPEN    4   /* retired compatibility stub: returns legacy -1 */
#define SYS_READ    5   /* retired compatibility stub: returns legacy -1 */
#define SYS_CLOSE   6   /* retired compatibility stub: returns legacy -1 */
#define SYS_BRK     7   /* transitional/brk: address-as-value return, NOT iris_error_t.
                          *   arg0=0 → query (returns current brk).
                          *   arg0=addr → move brk; success → addr, failure → old brk.
                          *   No process context → -1 (raw sentinel, not an error code).
                          *   Future heap ops must use SYS_VMO_CREATE + SYS_VMO_MAP. */
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
/* modern/conforming: process management */
#define SYS_SPAWN        18  /* (entry_vaddr, out_chan_ptr) → proc_handle or negative iris_error_t
                              *   proc_handle = KProcess handle in caller's table
                              *   *out_chan_ptr = bootstrap KChannel handle (optional)
                              *   child's arg0 = KChannel handle in child's table
                              *   parent proc_handle rights:
                              *     RIGHT_READ | RIGHT_MANAGE | RIGHT_DUPLICATE */
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

#define SYS_IRQ_ROUTE_REGISTER 27 /* (irq_num, chan_handle, proc_handle) → 0 or negative iris_error_t
                                   *   Routes hardware IRQ irq_num into chan_handle, owned by
                                   *   proc_handle.  When proc_handle's process exits,
                                   *   kprocess_teardown auto-clears the route via
                                   *   irq_routing_unregister_owner.
                                   *   Capability-driven authorization:
                                   *   proc_handle must carry RIGHT_READ|RIGHT_ROUTE.
                                   *   irq_num must be < IRQ_ROUTE_MAX.
                                   *   chan_handle must be KOBJ_CHANNEL with RIGHT_READ|RIGHT_WRITE.
                                   *   proc_handle must be KOBJ_PROCESS with
                                   *   RIGHT_READ|RIGHT_ROUTE. */

/* Retired bootstrap nameserver syscalls.
 * These numbers remain reserved for ABI stability but the implementation now
 * returns IRIS_ERR_NOT_SUPPORTED. Service discovery lives in svcmgr IPC. */
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
