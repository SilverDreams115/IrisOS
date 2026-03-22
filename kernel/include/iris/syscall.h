#ifndef IRIS_SYSCALL_H
#define IRIS_SYSCALL_H

#include <stdint.h>

/* Syscall numbers */
#define SYS_WRITE   0
#define SYS_EXIT    1
#define SYS_GETPID  2
#define SYS_YIELD   3
#define SYS_OPEN    4
#define SYS_READ    5
#define SYS_CLOSE   6
#define SYS_BRK     7
#define SYS_SLEEP       8
/* Legacy IPC — removed from user-space dispatch; internal use only */
/* SYS_IPC_CREATE  9  (retired) */
/* SYS_IPC_SEND   10  (retired) */
/* SYS_IPC_RECV   11  (retired) */
/* Capability IPC — canales como KObjects accesibles por handle */
#define SYS_CHAN_CREATE  12
#define SYS_CHAN_SEND    13
#define SYS_CHAN_RECV    14
#define SYS_HANDLE_CLOSE 15
/* Virtual Memory Objects */
#define SYS_VMO_CREATE   16   /* (size) → handle */
#define SYS_VMO_MAP      17   /* (handle, virt_addr, flags) → 0 or error */
/* Process management */
#define SYS_SPAWN        18   /* (entry_vaddr, out_chan_ptr) → proc_handle
                               *   proc_handle = KProcess handle in caller's table
                               *   *out_chan_ptr = bootstrap KChannel handle (optional)
                               *   child's arg0 = KChannel handle in child's table */
/* Notification objects */
#define SYS_NOTIFY_CREATE 19  /* () → handle */
#define SYS_NOTIFY_SIGNAL 20  /* (handle, bits) → 0 or error */
#define SYS_NOTIFY_WAIT   21  /* (handle, *out_bits) → 0 or error */
/* Handle management */
#define SYS_HANDLE_DUP      22  /* (src_handle, new_rights) → new_handle
                                 *   Requires RIGHT_DUPLICATE on src_handle.
                                 *   new_rights must be a subset of existing rights.
                                 *   Pass RIGHT_SAME_RIGHTS to keep the same rights. */
#define SYS_HANDLE_TRANSFER 23  /* (src_handle, dest_proc_handle, new_rights) → new_handle_id
                                 *   Requires RIGHT_TRANSFER on src_handle.
                                 *   Requires RIGHT_MANAGE on dest_proc_handle.
                                 *   Consumes src_handle. new_rights ⊆ src rights. */
/* Nameserver */
#define SYS_NS_REGISTER     24  /* (name_uptr, handle, rights) → error_code
                                 *   Registers handle's KObject under name with given rights.
                                 *   rights must be a subset of caller's rights on handle. */
#define SYS_NS_LOOKUP       25  /* (name_uptr, req_rights) → handle_id or error
                                 *   Looks up name; inserts a handle into caller's table.
                                 *   Pass RIGHT_SAME_RIGHTS to get all registered rights. */

void syscall_init(void);
void syscall_set_kstack(uint64_t kstack_top);

/* Called from ASM handler */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2);

#endif
