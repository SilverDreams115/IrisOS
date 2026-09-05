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
 * Current exported syscall number surface: 0..122 (119-121 are Stage 5's
 * SYS_CSPACE_SELF / SYS_TCB_CONFIGURE / SYS_TCB_WRITE_REGS, 122 is Stage
 * 6-pure's SYS_VSPACE_MAP_TABLE, 123 is Stage 7's SYS_TCB_FAULT_INFO; the
 * first unassigned number is 124).
 */

/*
 * Register convention, and one rule that follows from it.
 *
 * Arguments travel in RDI, RSI, RDX and R10 (arg0..arg3); the number is in
 * RAX.  A caller that passes fewer than four arguments MUST still present a
 * DEFINED r10 — zero — because a syscall that later grows a fourth argument
 * reads that register, and whatever the compiler happened to leave there is
 * not zero.
 *
 * This is not hypothetical: it is how SYS_INITRD_VMO's budget argument
 * (Stage 6 Step 5) broke every three-argument caller until their stubs were
 * fixed.  Zero also has a defined MEANING in every syscall that has grown an
 * argument so far — "no destination", "my own budget" — so a stub that zeroes
 * r10 degrades to the old behaviour instead of resolving garbage.
 *
 * The rule and the code that keeps it live together, below, because a rule
 * stated in a comment and implemented in twenty hand-written copies is a rule
 * that holds until one copy is forgotten — which is what happened.  Every
 * userland stub forwards to iris_syscall4; the arity-reducing wrappers pass
 * the zero so no caller can omit it.
 */

/*
 * Guarded against BOTH non-userland readers, because there are two.
 *
 * __KERNEL__ excludes the kernel, which has no business issuing a syscall
 * instruction.  __ASSEMBLER__ excludes the three service entry stubs that
 * include this header for its numbers (services/userboot/entry.S,
 * services/init/entry.S, services/kbd/main.S) — the C preprocessor runs over
 * a .S file and hands whatever survives to the assembler, so a declaration
 * that is merely "not kernel" still reaches it.  Missing that guard does not
 * show up in an incremental build, only in `make clean && make`.
 */
#if !defined(__KERNEL__) && !defined(__ASSEMBLER__)
static inline long iris_syscall4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}
static inline long iris_syscall3(long nr, long a0, long a1, long a2) {
    return iris_syscall4(nr, a0, a1, a2, 0);
}
static inline long iris_syscall2(long nr, long a0, long a1) {
    return iris_syscall4(nr, a0, a1, 0, 0);
}
static inline long iris_syscall1(long nr, long a0) {
    return iris_syscall4(nr, a0, 0, 0, 0);
}
static inline long iris_syscall0(long nr) {
    return iris_syscall4(nr, 0, 0, 0, 0);
}
#endif /* !__KERNEL__ && !__ASSEMBLER__ */

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
/* SYS_CHAN_CREATE(12)/SEND(13)/RECV(14) — retired in Phase 13/Track G with the
 * KChannel object.  Permanently reserved: the dispatch falls through to
 * IRIS_ERR_NOT_SUPPORTED.  Do not reuse 12-14.  Productive IPC is the KEndpoint
 * family (SYS_EP_SEND/RECV/CALL) + KNotification. */
#define SYS_CHAN_CREATE  12  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */
#define SYS_CHAN_SEND    13  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */
#define SYS_CHAN_RECV    14  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */
#define SYS_HANDLE_CLOSE 15  /* (handle) → 0 or negative iris_error_t */
/*
 * modern/conforming: Virtual Memory Objects
 *
 * SYS_VMO_CREATE(size, budget_cptr, dest) → 0 or negative iris_error_t
 *   size:        bytes, rounded up to whole pages.
 *   budget_cptr: Stage 6 Step 5 — the KUntyped (RIGHT_WRITE) this VMO's
 *                pages, page-address array and header are carved from.  A
 *                process holds several budgets and they are not
 *                interchangeable, which is why it says which one pays rather
 *                than the kernel guessing.  Stage 7 Step 14: REQUIRED — 0 used
 *                to mean "the budget my address space was built from", read
 *                off the KProcess, which is the guess this argument exists to
 *                remove.
 *   dest:        destination slot (cnode | slot<<32); required since Stage 4.
 *   The pages return to that Untyped's child count when the VMO is destroyed,
 *   so the region becomes RESET-able once nothing maps through it.
 */
#define SYS_VMO_CREATE   16  /* (size, budget, dest) → 0 or negative iris_error_t */
#define SYS_VMO_MAP      17  /* (handle, virt_addr, flags) → 0 or negative iris_error_t
                               * flags bit 0: MAP_WRITABLE  — map with PAGE_WRITABLE
                               * flags bit 1: MAP_EXEC      — map without PAGE_NX (executable)
                               * W^X enforced: bit 0 + bit 1 simultaneously → ERR_INVALID_ARG */
/* SYS_SPAWN 18 retired in Phase 19 — permanently reserved, returns
 * IRIS_ERR_NOT_SUPPORTED. Healthy-path process creation now uses the
 * composable primitives rooted in SYS_INITRD_VMO / SYS_PROCESS_CREATE /
 * SYS_VMO_MAP_INTO / SYS_THREAD_START / SYS_HANDLE_INSERT. */
#define SYS_SPAWN        18
/* Phase S1: SYS_NOTIFY_CREATE (19) RETIRED — returns IRIS_ERR_NOT_SUPPORTED.
 * Notifications are created via SYS_UNTYPED_RETYPE2 (Untyped storage, cap
 * directly in CSpace; no kslab, no per-process quota, no handle).  Number
 * permanently reserved. */
#define SYS_NOTIFY_CREATE 19 /* RETIRED (Phase S1) → IRIS_ERR_NOT_SUPPORTED */
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
                                 *   SYS_CSPACE_MINT with the destination
                                 *   CNode named (Stage 7 Step 9), or an IPC
                                 *   receive-slot (A1.5/A1.6). */
/*
 * SYS_PROCESS_WATCH (29) — RETIRED (Stage 7 Step 10).  Number permanently
 * reserved; answers IRIS_ERR_NOT_SUPPORTED.  A death is watched on the THREAD
 * that dies (SYS_TCB_WATCH), by whoever holds its TCB — which a supervisor
 * has, because it retyped and configured that thread.  Watching a PROCESS
 * meant needing authority over an object you did not create to learn about an
 * execution you did.
 */
#define SYS_PROCESS_WATCH   29
/*
 * SYS_PROCESS_SELF (28) — RETIRED (Stage 7 Step 15).  Number permanently
 * reserved; answers IRIS_ERR_NOT_SUPPORTED.  It handed a task a capability to
 * its own KProcess for the things that used to need one — minting into your
 * own CSpace, mapping into your own address space, being named as a payer,
 * watching yourself — and every one of those was re-aimed at the object it
 * actually concerns.  SYS_CSPACE_SELF (119), SYS_VSPACE_SELF and SYS_TCB_SELF
 * (96) hand out the three self-capabilities that are left.
 */
#define SYS_PROCESS_SELF    28
/* SYS_PROCESS_STATUS — RETIRED (Stage 7 Step 13).  Number permanently
 * reserved; returns IRIS_ERR_NOT_SUPPORTED.  Liveness is a property of an
 * EXECUTION: read it with SYS_TCB_GET_INFO (101) on a thread you hold, whose
 * `state` says more than the one bit this answered. */
#define SYS_PROCESS_STATUS  26

/* Phase 13 (Track B): the legacy PROC_EVENT_MSG_EXIT KChannel event is retired.
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
 * SYS_CHAN_SEAL(37) — retired in Phase 13/Track G with the KChannel object.
 * Permanently reserved: the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.
 * Do not reuse 37.  (Service teardown now relies on KEndpoint close semantics,
 * which wake blocked peers with IRIS_ERR_CLOSED.)
 */
#define SYS_CHAN_SEAL  37  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * Synchronous channel call — modern/conforming (iris_error_t).
 *
 * SYS_CHAN_CALL(38) — retired in Phase 13/Track G (zero callers; the productive
 * request/reply path is the KEndpoint SYS_EP_CALL).  Permanently reserved:
 * the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.  Do not reuse 38.
 */
#define SYS_CHAN_CALL  38  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * Hardware capability creation — modern/conforming (iris_error_t).
 *
 * Stage 5 Step 2: each syscall requires ITS OWN control capability, matched
 * exactly.  The IRQ control capability does not authorise ioport creation and
 * the ioport control capability does not authorise IRQ creation; neither is a
 * bit on a larger capability, so holding one says nothing about the other.
 * Policy (which service gets which resource) lives in svcmgr; the kernel
 * validates ranges and creates the cap object.
 *
 * Stage 7 Step 14: both take the BUDGET the kernel object is carved from in
 * arg2, and it is required.  They used to charge the caller's KProcess default
 * — the last place the kernel picked whose memory pays.  IRQCAP had a free
 * argument; IOPORT did not, so its two 16-bit device facts share arg1, which
 * is what they always were: one range, described in one word.
 *
 * SYS_CAP_CREATE_IRQCAP(auth_cptr, irq_num, budget_cptr, dest_slot) → 0 or negative iris_error_t
 *   auth_cptr: KOBJ_BOOTSTRAP_CAP whose authority is exactly
 *     IRIS_BOOTCAP_IRQ_CONTROL; becomes the MDB parent of the new cap.
 *   irq_num: hardware IRQ line (0–15).
 *   budget_cptr: KUntyped (RIGHT_WRITE) the KIrqCap is carved from.  Required.
 *   dest_slot: root-CNode slot receiving a KIrqCap with
 *     RIGHT_ROUTE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 *
 * SYS_CAP_CREATE_IOPORT(auth_cptr, base|count<<16, budget_cptr, dest_slot) → 0 or negative iris_error_t
 *   auth_cptr: KOBJ_BOOTSTRAP_CAP whose authority is exactly
 *     IRIS_BOOTCAP_IOPORT_CONTROL.
 *   arg1 low 16:  first I/O port in the range (0–0xFFFF).
 *   arg1 high 16: number of consecutive ports (1–0x10000, base+count ≤ 0x10000).
 *   budget_cptr: KUntyped (RIGHT_WRITE) the KIoPort is carved from.  Required.
 *   dest_slot: root-CNode slot receiving a KIoPort with
 *     RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
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
 * SYS_INITRD_VMO(auth_cptr, index, dest, budget_cptr) → 0 or iris_error_t
 *   auth_cptr:   the initrd capability (IRIS_BOOTCAP_INITRD_CONTROL).
 *   index:       initrd catalog index (name→index mapping is a ring-3 concern).
 *   dest:        destination slot (cnode | slot<<32).
 *   budget_cptr: Stage 6 Step 5 — the KUntyped the image COPY is carved from.
 *                Reading an entry allocates as many pages as the image is
 *                long, and a loader parses it and drops it, so a caller that
 *                points this at a scratch Untyped can RESET that region
 *                between spawns instead of spending its whole pool one image
 *                at a time.  Stage 7 Step 14: REQUIRED — 0 used to mean "the
 *                caller's own budget", read off the KProcess.
 *   The result is a read-only KOBJ_VMO (RIGHT_READ) over a private copy of the
 *   embedded ELF bytes; map with flags=0.
 *
 * SYS_PROCESS_CREATE(auth_cptr, dest, vspace_cptr, cnode_cptr) → 0 or iris_error_t
 *   auth_cptr:   the process control capability (IRIS_BOOTCAP_PROC_CONTROL).
 *   dest:        destination slot (cnode | slot<<32); required since Stage 4.
 *   vspace_cptr: Stage 6-pure Step 4 — REQUIRED.  A KOBJ_VSPACE (RIGHT_WRITE)
 *                the CALLER retyped from its own Untyped
 *                (RETYPE2 IRIS_KOBJ_VSPACE, obj_arg 4096).  A process is
 *                COMPOSED from objects its creator made, not conjured from a
 *                quota: this argument used to be a budget the kernel built an
 *                address space out of, which meant the holder paid for a walk
 *                it could not name until the process existed and could not
 *                have built differently.
 *
 *                IRIS_ERR_BUSY if that address space is already bound to a
 *                process — one walk, one process, because teardown is per
 *                process.  What remains kernel-side (the KProcess object and
 *                its root CNode) comes from the Untyped the address space
 *                itself was retyped from, so a child costs exactly one region
 *                and RESETting it returns all of the child.
 *   cnode_cptr:  Stage 6-pure Step 5 — REQUIRED, and passed in arg3 (r10).  A
 *                KOBJ_CNODE (RIGHT_WRITE) the CALLER retyped, which becomes
 *                the child's ROOT CSpace; the spawner therefore also chooses
 *                how wide that CSpace is, where the kernel used to pick 256
 *                slots for everyone.  IRIS_ERR_BUSY if it is already some
 *                process's root — one CNode, one root, because teardown
 *                empties a root's slots.
 *
 *                A create that fails after either capability was accepted
 *                releases both claims, so the same VSpace and CNode can be
 *                passed to a retry.
 *   Creates an empty KProcess around that address space; no threads start.
 *   Publishes the process capability into `dest` with RIGHT_READ|RIGHT_WRITE|
 *   RIGHT_MANAGE|RIGHT_DUPLICATE|RIGHT_TRANSFER|RIGHT_ROUTE.
 *
 * SYS_VMO_MAP_INTO(vmo_h, vspace_cptr, vaddr, flags) → 0 or negative iris_error_t
 *   vmo_h:   KOBJ_VMO with RIGHT_READ (plus RIGHT_WRITE for MAP_WRITABLE).
 *   vspace_cptr: Stage 7 Step 9 — KOBJ_VSPACE with RIGHT_WRITE, the address
 *            space the mapping is installed in.  It was a KOBJ_PROCESS with
 *            RIGHT_MANAGE, out of which the kernel read `proc->vspace`: a
 *            caller that already held the address space had to hold authority
 *            over the whole process as well, and the process capability
 *            carried nothing but a pointer to the thing being used.  A spawner
 *            HAS the VSpace — it retyped it and handed it to
 *            SYS_PROCESS_CREATE.  This is the shape SYS_VMO_MAP_PAGE and
 *            SYS_FRAME_MAP have had since Phase 25/26.
 *   vaddr:   page-aligned target virtual address in that address space.
 *   flags:   bit 0 = MAP_WRITABLE, bit 1 = MAP_EXEC; W^X enforced.
 *   Eagerly allocates and maps all pages into proc's page table at call time.
 *   Also registers the mapping descriptor in proc's vmo_mappings for teardown.
 *   Uses 4-arg syscall ABI (arg3 = flags via r10).
 *
 * SYS_THREAD_START — RETIRED (Stage 7 Step 1).  58 answers NOT_SUPPORTED and
 *   the number stays reserved.  It carved a spawned process's first thread out
 *   of the kernel's static task pool.  Composing one instead: RETYPE2 a
 *   KOBJ_TCB from the child's budget, SYS_TCB_CONFIGURE it with the child's
 *   CSpace, VSpace and process capability, SYS_TCB_WRITE_REGS to say where it
 *   starts, SYS_TCB_RESUME to start it.
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
/*
 * SYS_INITRD_VMO — RETIRED (Stage 6, ledger D-5).  Permanently reserved.
 *
 * It handed a boot image over as a KVMO, so the loader and vfs both had to
 * speak a second memory ABI to read a file the kernel already had, and had to
 * ask a separate syscall how big it was.  SYS_INITRD_FRAME (134) hands over a
 * FRAME and answers the size; one map covers it.
 */
#define SYS_INITRD_VMO      55
#define SYS_PROCESS_CREATE  56
#define SYS_VMO_MAP_INTO    57
#define SYS_THREAD_START    58
#define SYS_HANDLE_INSERT   59

/*
 * Framebuffer VMO claim — modern/conforming (iris_error_t).
 *
 * SYS_FRAMEBUFFER_VMO(auth, info_uptr, dest) → vmo_handle, or 0 when dest
 *   names a slot, or negative iris_error_t   (Stage 4 destination slot in
 *   arg2, RETYPE2 packing — see SYS_VSPACE_SELF)
 *   auth_h:    the framebuffer control capability (IRIS_BOOTCAP_FB_CONTROL).
 *   info_uptr: user pointer to struct iris_fb_params (see iris/fb_info.h);
 *              filled with physical base, size, and pixel geometry on success.
 *   Returns a KOBJ_VMO (eager wrap, owned=0) covering the framebuffer region
 *   with RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE|RIGHT_TRANSFER.
 *   One-shot: the kernel clears the framebuffer-valid flag on first call so that
 *   only the first caller can claim the physical framebuffer window.
 */
/*
 * SYS_FRAMEBUFFER_VMO — RETIRED (Stage 6).  Permanently reserved.
 *
 * It answered "where is the framebuffer" AND fabricated a KVMO over the region
 * in the same call, so the geometry could only be learned by accepting a
 * kernel-made object — the last memory object in the system nobody retyped
 * (ledger D-5).  It was also a one-shot GRANT wearing a query's clothes: the
 * first caller consumed a valid flag and every later one got NOT_FOUND.
 *
 * The two halves are now what they are.  SYS_FRAMEBUFFER_INFO (133) reports
 * the geometry and creates nothing; the REGION is a DEVICE Untyped published
 * in BootInfo (D-9), and exclusivity is who holds that capability.
 */
#define SYS_FRAMEBUFFER_VMO 60

/*
 * Initrd catalog count — modern/conforming (iris_error_t).
 *
 * SYS_INITRD_COUNT(auth_h) → uint32_t count or negative iris_error_t
 *   auth_h: the process control capability (IRIS_BOOTCAP_PROC_CONTROL) for
 *     SYS_PROCESS_CREATE; the initrd capability (IRIS_BOOTCAP_INITRD_CONTROL)
 *     for the initrd calls.
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
 * SYS_CHAN_RECV_TIMEOUT(63) — retired in Phase 13/Track G with the KChannel
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
 *   Requires the debug control capability (IRIS_BOOTCAP_DEBUG_CONTROL).
 *   max_bytes must be > 0 and ≤ KLOG_BUF_SIZE (4096).
 */
#define SYS_KLOG_DRAIN 65

/*
 * I/O port sub-delegation — modern/conforming (iris_error_t).
 *
 * SYS_IOPORT_RESTRICT — RETIRED (Stage 4).  Number permanently reserved;
 * returns IRIS_ERR_NOT_SUPPORTED.  It published a narrowed KIoPort as a handle
 * with no capability ancestor; SYS_CAP_CREATE_IOPORT publishes into a CSpace
 * slot as an MDB child of the authorising bootstrap cap instead.
 */
#define SYS_IOPORT_RESTRICT  43

/*
 * SYS_WAIT_ANY(44) — retired in Phase 13/Track G (zero callers).  Permanently
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
 * SYS_CHAN_RECV_NB(34) — retired in Phase 13/Track G with the KChannel object.
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
/* SYS_PROCESS_KILL — RETIRED (Stage 7 Step 13).  Number permanently reserved;
 * returns IRIS_ERR_NOT_SUPPORTED.  Stopping an execution is SYS_TCB_EXIT (100)
 * on a thread you hold; reclaiming a process that never started is deleting
 * the last capability to it. */
#define SYS_PROCESS_KILL  35

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

/* Boot capability kinds — ONE CAPABILITY, ONE AUTHORITY (Stage 5 Step 2).
 *
 * These are not bits to combine: each value names a whole capability, the
 * kernel matches it by exact equality, and a capability carrying more than one
 * cannot be constructed.  The predecessor was a single object with a
 * permission mask, delegated whole and narrowed by cloning a weaker copy
 * (SYS_BOOTCAP_RESTRICT, retired with it).
 *
 * Each is published by the kernel into its own slot of the root task's CNode
 * and described in the BootInfo region (<iris/root_bootinfo.h>). */
#define IRIS_BOOTCAP_NONE           0u
#define IRIS_BOOTCAP_PROC_CONTROL   (1u << 0)  /* SYS_PROCESS_CREATE */
/* (1u << 1) was IRIS_BOOTCAP_HW_ACCESS — one bit for both IRQ and ioport
 * creation.  Split into the two control capabilities below; not reused. */
#define IRIS_BOOTCAP_DEBUG_CONTROL  (1u << 2)  /* klog drain / sched info / poweroff */
#define IRIS_BOOTCAP_FB_CONTROL     (1u << 3)  /* SYS_FRAMEBUFFER_VMO (one-shot) */
#define IRIS_BOOTCAP_IRQ_CONTROL    (1u << 4)  /* SYS_CAP_CREATE_IRQCAP */
#define IRIS_BOOTCAP_IOPORT_CONTROL (1u << 5)  /* SYS_CAP_CREATE_IOPORT */
#define IRIS_BOOTCAP_INITRD_CONTROL (1u << 6)  /* SYS_INITRD_COUNT / SYS_INITRD_VMO */

/*
 * SYS_BOOTCAP_RESTRICT (45) — RETIRED (Stage 5 Step 2).  Number permanently
 * reserved; returns IRIS_ERR_NOT_SUPPORTED.
 *
 * It derived a weaker CLONE of a boot capability, the only way to give up part
 * of your authority while one object carried several authorities at once.
 * Each boot capability carries exactly one now, so there is nothing to narrow:
 * a holder that wants less deletes the slot holding what it no longer needs,
 * and a granter that wants it back revokes through the CDT.
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
 * SYS_EXCEPTION_HANDLER — RETIRED (Stage 7 Step 12).  Use
 * SYS_TCB_SET_FAULT_HANDLER (126); the number returns NOT_SUPPORTED.
 *
 * It armed a PROCESS, so every thread in it shared one mailbox, one
 * notification and one set of signal bits, and a handler could only tell two
 * executions apart by reading an id out of the fault record.  The replacement
 * arms the thread, named by capability.  Everything else — the required `dest`
 * mailbox in arg3, resolved in the REGISTRANT's CSpace, the TCB capability
 * published RIGHT_READ|RIGHT_WRITE before the signal, the NOT_FOUND on a dead
 * target, the re-registration rule that carries an outstanding fault across a
 * handover, kill-on-no-handler — is unchanged and documented there.
 */
#define SYS_EXCEPTION_HANDLER  47

/*
 * SYS_PROCESS_FAULT_INFO — RETIRED (Stage 7 Step 12).  Use SYS_TCB_FAULT_INFO
 * (123); the number returns NOT_SUPPORTED.
 *
 * The record has lived on the thread since Step 6, and since Step 12 so has
 * the handler registration — which means every principal that used to need the
 * process-scoped read holds a capability to the thread instead, including the
 * supervisor Step 8 kept this syscall for.  One question, asked of the object
 * that took the fault.
 */
#define SYS_PROCESS_FAULT_INFO 105

/*
 * Exception resume — modern/conforming (iris_error_t).
 *
 * SYS_EXCEPTION_RESUME(tcb_cptr, action) → 0 or negative iris_error_t
 *   tcb_cptr: the FAULTING THREAD, as a capability, with RIGHT_WRITE — the
 *             one SYS_EXCEPTION_HANDLER's mailbox was filled with.
 *   action:   0 = resume the thread at the faulting RIP; 1 = kill it.
 *   The thread must be in BLOCKED_FAULT; otherwise IRIS_ERR_NOT_FOUND.
 *
 *   Stage 7 Step 7: this took (proc_h, task_id, action).  Authority came from
 *   the process capability and the id was checked against it, so the number
 *   conferred nothing — but it SELECTED a kernel object, which charter
 *   §3.4/§3.5 forbid, and a supervisor could not hold, delegate or revoke
 *   "that thread" the way it holds everything else.  RIGHT_WRITE on the thread
 *   is now the whole authority: deciding whether an execution continues is a
 *   property of that execution.
 *
 *   Phase 25 (additive): action 2 = resume, action 3 = kill, each with a fault
 *   generation check — bits [63:32] of the action argument must equal the
 *   fault_seq the caller read at FAULT_OFF_SEQ.  A generation of 0 is
 *   INVALID_ARG; a mismatch (the thread refaulted since, or the caller replays
 *   a stale record) is NOT_FOUND with no side effect.
 */
#define SYS_EXCEPTION_RESUME   66

/*
 * Frame size query — modern/conforming (iris_error_t).
 *
 * SYS_FRAME_SIZE(frame_cptr) → uint64_t byte size or negative iris_error_t
 *   frame_cptr: KOBJ_FRAME with RIGHT_READ.
 *   Returns the byte size of the frame as it was retyped.
 *
 * Ledger D-5: this was SYS_VMO_SIZE and asked a KVmo.  The number and the
 * question are unchanged — how much memory does this capability name — and
 * the answer matters for the same reason it always did: SYS_FRAME_MAP covers
 * the WHOLE frame (D-10), so a caller supplying its own paging levels has to
 * know how many 2 MiB regions the map will touch.  A frame carries its size,
 * so this reads it off the object rather than making the holder remember.
 */
#define SYS_FRAME_SIZE 67

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
 *   Requires the debug control capability (IRIS_BOOTCAP_DEBUG_CONTROL).
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
 * SYS_PROCESS_EXIT_CODE (71) — RETIRED (Stage 7 Step 10).  Number permanently
 * reserved; answers IRIS_ERR_NOT_SUPPORTED.  The code belongs to the execution
 * that produced it: SYS_TCB_EXIT_CODE reads it off that thread, with
 * RIGHT_READ on the thread as the whole authority.
 */
#define SYS_PROCESS_EXIT_CODE 71

/*
 * SYS_WAIT_ANY_TIMEOUT(72) — retired in Phase 13/Track G (zero callers).
 * Permanently reserved: the dispatch falls through to IRIS_ERR_NOT_SUPPORTED.
 */
#define SYS_WAIT_ANY_TIMEOUT 72  /* RETIRED — reserved, returns IRIS_ERR_NOT_SUPPORTED */

/*
 * SYS_THREAD_CREATE (48) — RETIRED (Stage 5 Step 4).  Number permanently
 * reserved; returns IRIS_ERR_NOT_SUPPORTED.
 *
 * It carved a thread from the kernel's static task pool and returned a global
 * thread id: no capability authorised it, no Untyped paid for the storage, and
 * the identity it returned was an index into a kernel array.  Threads are
 * retyped from an Untyped and configured with CSpace/VSpace capabilities now
 * (SYS_TCB_CONFIGURE / SYS_TCB_WRITE_REGS / SYS_TCB_RESUME), and what comes
 * back is a capability in a slot.
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
 *   Requires the debug control capability (IRIS_BOOTCAP_DEBUG_CONTROL),
 *   named as a CPtr — never searched for.
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
 * SYS_ENDPOINT_CREATE — RETIRED (Phase S1) → IRIS_ERR_NOT_SUPPORTED.
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
 * SYS_CNODE_CREATE — RETIRED (Phase S1) → IRIS_ERR_NOT_SUPPORTED.
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
 * SYS_SC_CREATE — RETIRED (Phase S2) → IRIS_ERR_NOT_SUPPORTED.
 *   SchedulingContexts are created via SYS_UNTYPED_RETYPE2 (KOBJ_SCHED_CONTEXT,
 *   Untyped storage, cap in CSpace) and configured with SYS_SC_CONFIGURE.
 *   Number permanently reserved.
 *
 * SYS_SC_CONFIGURE(sc_h, budget_ticks, period_ticks) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on sc_h.
 *   budget_ticks: ticks the bound task may run per period (1 .. period_ticks-1).
 *   period_ticks: length of the period in scheduler ticks (> budget_ticks).
 *   Resets remaining_budget to budget_ticks immediately.
 *
 * SYS_THREAD_SET_SC(sc_h) → 0 or negative iris_error_t
 *   LEGACY FROZEN (Phase S2): self-bind of the calling thread.  It may NOT take
 *   new consumers — the canonical binding path is SYS_SC_BIND(sc, tcb) by
 *   CPtr.  Kept for existing code; one-to-one enforced (BUSY if sc_h is
 *   already bound to another task).  Pass 0 to unbind.
 *
 * SYS_SC_BIND(sc_cptr, tcb_cptr) → 0 or negative iris_error_t   (Phase S2)
 *   Explicitly binds a SchedulingContext to a TCB, both by CPtr, both live,
 *   one-to-one (BUSY if either is already bound to another).  The SC must be
 *   configured (SC_CONFIGURE).  tcb_cptr == 0 unbinds the SC.  Requires
 *   RIGHT_WRITE on both.  It is the canonical binding path for building tasks
 *   from user space (SYS_THREAD_SET_SC is the self-bind).
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
 *   LEGACY single-object retype returning a handle (Phase S1: TRANSITIONAL).
 *   Restricted to the non-migrated types: KOBJ_UNTYPED (obj_arg = sub-region
 *   bytes, page-aligned), KOBJ_FRAME (obj_arg = bytes, page-aligned) and
 *   KOBJ_SCHED_CONTEXT.  The migrated family (ENDPOINT / NOTIFICATION /
 *   REPLY / CNODE) returns IRIS_ERR_NOT_SUPPORTED here — those objects can
 *   only be born via SYS_UNTYPED_RETYPE2 (never through a handle: S20).
 */
#define SYS_UNTYPED_INFO   86
#define SYS_UNTYPED_RETYPE 87
#define SYS_UNTYPED_RESET  88

/* Phase S1: userland-visible object-type codes for SYS_UNTYPED_RETYPE(2).
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
#define IRIS_KOBJ_PAGE_TABLE    16u
#define IRIS_KOBJ_VSPACE        14u  /* Stage 6-pure: an address space the holder retypes */

/*
 * Block 6 — CNode slot operations (Ph82-84).
 *
 * SYS_CNODE_MOVE(cnode_h, slot_idx, src_h) → 0 or negative iris_error_t
 *   Requires RIGHT_WRITE on cnode_h.
 *   Moves the capability from the caller's HT handle src_h into CNode slot slot_idx.
 *   src_h is consumed (removed from HT) — seL4 Move semantics.
 *   If slot_idx was occupied, the old capability is released.
 *
 * SYS_CNODE_FETCH — RETIRED (Stage 4).  Number permanently reserved; returns
 * IRIS_ERR_NOT_SUPPORTED.  It copied a slot's capability into a HANDLE, with
 * no MDB edge back to the slot, so revoking the slot left the copy alive.
 * SYS_CSPACE_MINT is the slot-to-slot form and records the derivation.
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
 *   Reply-cap transfer (Phase 7.1 ABI extension): the reply MAY carry one
 *   capability in msg.attached_handle / msg.attached_rights, with the same
 *   staging semantics as SYS_EP_SEND (server handle needs RIGHT_TRANSFER and
 *   is consumed; rights are reduced by msg.attached_rights). The cap is
 *   installed in the EP_CALL caller's handle table; the caller observes the
 *   new handle id in msg.attached_handle after EP_CALL returns. On errors
 *   before staging (bad kreply_h, unreadable msg, stage validation failure)
 *   the server handle is NOT consumed; on IRIS_ERR_NOT_FOUND (KReply already
 *   invoked) the staged cap is destroyed and the handle IS consumed.
 *   Before Phase 7.1 the attached_handle field was ignored on replies.
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
 * SYS_PROC_CSPACE_MINT (104) — RETIRED (Stage 7 Step 9).  Number permanently
 * reserved; answers IRIS_ERR_NOT_SUPPORTED.
 *
 * It minted into a CSpace named by the PROCESS that owned it, and the kernel
 * read that process's root CNode out of it — so a caller reached a capability
 * namespace it did not hold by naming something else that pointed at it.
 *
 * SYS_CSPACE_MINT is the whole replacement.  It has taken a destination CNode
 * since Phase S3, dest_cnode 0 meaning the caller's own root; minting into a
 * child is the same call with the child's root CNode as the destination, which
 * a spawner HAS because it retyped it (Stage 6-pure Step 5).  A spawner that
 * means to keep delegating keeps that capability; one that does not holds no
 * authority over its child's namespace at all — a distinction the
 * process-shaped form could not express.
 */
#define SYS_PROC_CSPACE_MINT 104

/*
 * Block 9 — Frame capabilities (Phase 5 / 5.1).
 *
 * SYS_FRAME_MAP(frame_cptr, vspace_cptr, user_va, flags) → 0 or negative iris_error_t
 *   frame_cptr:  KOBJ_FRAME with RIGHT_READ (+ RIGHT_WRITE if flags bit 0 set).
 *   vspace_cptr: KOBJ_VSPACE with RIGHT_WRITE to install the PTE.  Phase 25:
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
 *   vspace_cptr: KOBJ_VSPACE with RIGHT_WRITE (dual resolver since Phase 25).
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
 * SYS_VSPACE_SELF(dest) → handle_id, or 0 when dest names a slot, or negative
 *   iris_error_t   (Phase 19; Stage 4 destination slot)
 *   dest == 0 → legacy: the cap is published as a handle.
 *   dest != 0 → RETYPE2 packing (CNode in the low 32 bits, 0 = own root;
 *   slot index in the high 32).  The cap is installed in that slot and the
 *   call returns 0.  The handle leg dies with the handle namespace.
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
 * SYS_PROCESS_VSPACE (107) — RETIRED (Stage 7 Step 15).  Number permanently
 * reserved; answers IRIS_ERR_NOT_SUPPORTED.
 *
 * It was the map-into-target authority for a user pager: a supervisor holding
 * RIGHT_MANAGE on a process could take a capability to that process's address
 * space and mint it onward.  Phase 25 introduced it to make that authority a
 * first-class, delegable object instead of a process-cap side effect — which
 * was the right direction and stopped one step short, because the kernel still
 * produced the VSpace by reading `child->vspace` out of a KProcess.  The
 * supervisor reached an object it did not hold by naming a different one, the
 * same shape Step 9 removed for CSpaces.
 *
 * The spawner already has it: since Stage 6-pure Step 4 the loader RETYPES the
 * child's address space and holds it through the whole spawn.  It hands it
 * over now (svc_load_minted_ws's `keep_vspace_dest`), opt-in, because keeping
 * one keeps that address space and every page table in it alive past the
 * child's death.  For the caller's own address space, SYS_VSPACE_SELF — which
 * this syscall's HANDLE_INVALID case was always documented as equivalent to.
 */
#define SYS_PROCESS_VSPACE 107

/*
 * SYS_VMO_MAP_PAGE(vmo_cptr, vspace_cptr, target_va, offset_flags)
 *                                          → 0 or negative iris_error_t   (Phase 26)
 *
 * Maps exactly ONE page of a memory object at a chosen byte offset into a
 * VSpace at a chosen VA.  This is the page-granular, offset-addressed
 * primitive a VMO-backed user pager uses to resolve a fault: it is to
 * SYS_FRAME_MAP what a VMO page is to a raw frame — same authority shape
 * (the VSpace WRITE cap is the map-into-target authority, no process MANAGE),
 * composing directly with SYS_PROCESS_VSPACE (Phase 25).
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
 * SYS_VMO_CREATE_FOR(size, charge_target, dest, budget_cptr) → 0 or iris_error_t
 *
 * Like SYS_VMO_CREATE, but the VMO OBJECT QUOTA is charged to `charge_target` —
 * a CPtr to a KProcess the caller holds RIGHT_MANAGE on — instead of to the
 * caller.  The capability lands in the CALLER's CSpace (holder), while the
 * target is the OWNER.  This lets a loader create a child's image VMOs counted
 * against the CHILD's resource domain, so the loader's own quota stays flat
 * regardless of how many children it launches (Phase 29 root-cause fix for
 * caller-charged accounting).
 *
 *   budget_cptr: Stage 7 Step 14 — REQUIRED, and it is where the MEMORY comes
 *                from, resolved in the CALLER's CSpace.  It used to be the
 *                payer's own default budget, so a caller spent an Untyped it
 *                did not hold and could not see.  A loader carving a child's
 *                image out of the child's pool holds that pool and names it.
 *                That quota and memory can therefore name different domains is
 *                KVMO's owner/payer split, which retires with the object.
 *
 *   Returns IRIS_ERR_ACCESS_DENIED — missing RIGHT_MANAGE on charge_target,
 *                                    or RIGHT_WRITE on budget_cptr.
 *   Returns IRIS_ERR_WRONG_TYPE    — charge_target is not a KProcess.
 *   Returns IRIS_ERR_BAD_HANDLE    — charge_target is torn down.
 *   Returns IRIS_ERR_INVALID_ARG   — no budget named, or it is not an Untyped.
 *   Returns IRIS_ERR_NO_MEMORY     — object alloc or the target's quota is full.
 */
#define SYS_VMO_CREATE_FOR 109

/*
 * SYS_RESOURCE_INFO(proc_h, out_ptr) → 0 or iris_error_t (Phase 29)
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
 * Phase S1 — seL4 Architectural Convergence.
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
 *   KOBJ_TCB (Phase S2 Step 0): the created TCB is a canonical, INACTIVE
 *   object — storage inside the untyped, capability in CSpace, observable
 *   (TCB_GET_INFO: state = SUSPENDED, task_id = 0) and delegable, but NOT
 *   runnable: it has no kstack, no registry slot and no process.  Execution
 *   syscalls (TCB_SUSPEND/RESUME/EXIT, SC_BIND) refuse it with
 *   IRIS_ERR_NOT_SUPPORTED until TCB_CONFIGURE lands (roadmap Step 5/6);
 *   TCB_SET_PRIORITY works (stored for later, seL4-style inactive TCB).
 *   Deleting the last capability returns the zeroed block to the untyped.
 *
 * SYS_UNTYPED_QUERY(kind|version<<16|size<<32, buf_uptr, ut) → 0 or error
 *   Read-only, versioned instrumentation (never authority).  Phase S2 C.1:
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

/*
 * Phase S3 — CSpace-only derivation surface (native MDB/CDT).
 * Source authority comes EXCLUSIVELY from the caller's CSpace (CPtr < 1024
 * resolved to a slot); a handle value in a source argument is INVALID_ARG.
 * See docs/architecture/cspace-cdt-mdb.md.
 *
 * SYS_CSPACE_MINT(src_cptr, dest_cnode|slot<<32, rights|badge<<32)
 *     → 0 or negative iris_error_t
 *   Copy (rights == RIGHT_SAME_RIGHTS) or mint (reduced rights, central
 *   badge rule) of the capability in src_cptr into dest slot (dest_cnode 0
 *   = caller's root CNode).  The new cap is an MDB CHILD of the source slot.
 *   Requires RIGHT_DUPLICATE on the source.  Exclusive: occupied dest →
 *   IRIS_ERR_ALREADY_EXISTS.  On any error nothing changes.
 *
 *   Stage 7 Step 9: `dest_cnode` is how a capability reaches ANOTHER task's
 *   CSpace — name that task's root CNode.  It is the whole of what the two
 *   retired process-shaped mints did, minus the part where the kernel read a
 *   CSpace root out of a process the caller named instead of the CSpace it
 *   wanted.  Cross-task or not is a fact about which capability is in
 *   dest_cnode, not about which syscall is called.
 *
 * SYS_CSPACE_REVOKE(cptr) → number of caps destroyed, or negative error
 *   Deletes the ENTIRE MDB descendant subtree of the named slot — across
 *   CNodes and processes — in deterministic order.  The invoked capability
 *   and its siblings survive.  delete(cap) ≠ revoke(cap).
 *
 * SYS_CSPACE_MINT_INTO (116) — RETIRED (Stage 7 Step 9).  Number permanently
 * reserved; answers IRIS_ERR_NOT_SUPPORTED.  Same reasoning as
 * SYS_PROC_CSPACE_MINT: SYS_CSPACE_MINT with the destination CNode named by
 * the capability the caller holds.
 */
#define SYS_CSPACE_MINT      114
#define SYS_CSPACE_REVOKE    115
#define SYS_CSPACE_MINT_INTO 116

/*
 * Phase S4 (Stage 4) — CSpace-native capability introspection.  CPtr only; a
 * handle value is INVALID_ARG with no fallback (charter §3.6/§3.7).  These
 * replace SYS_HANDLE_TYPE (52) and SYS_HANDLE_SAME_OBJECT (53), which retire
 * with the handle namespace.
 *
 * SYS_CAP_IDENTIFY(cptr) → kobject_type_t or negative iris_error_t
 *   The type of the capability in the named slot.  An empty slot is
 *   IRIS_ERR_NOT_FOUND — this does NOT enumerate a CSpace, the caller must
 *   already name the slot.  No right is required and none is conferred.
 *
 * SYS_CAP_SAME_OBJECT(cptr_a, cptr_b) → 1 / 0, or negative iris_error_t
 *   Whether both slots name the same KObject.  Identity only: rights and
 *   badge are not compared.
 */
#define SYS_CAP_IDENTIFY     117
#define SYS_CAP_SAME_OBJECT  118

/*
 * Stage 5 Step 4 — execution for a TCB retyped from an Untyped.
 *
 * RETYPE2(KOBJ_TCB) has produced cap-complete but INACTIVE threads since
 * Phase S2: no registry slot, no kernel stack, no address space, refused by
 * every execution syscall.  The operation that gives them those was missing
 * because ITS ARGUMENTS ARE CAPABILITIES — a CSpace root and a VSpace — and
 * those only became addressable as capabilities in Stages 3-5.
 *
 * SYS_CSPACE_SELF(dest) → 0 or negative iris_error_t
 *   Publishes a capability to the CALLER'S OWN root CNode into `dest`
 *   (RETYPE2 packing: CNode in the low 32 bits, slot in the high 32).
 *   The CNode counterpart of SYS_TCB_SELF: self-introspection, no delegation
 *   implied, and no way to name anyone else's CSpace.  It exists because
 *   SYS_TCB_CONFIGURE takes the CSpace as a capability, and a process that
 *   could not name its own root CNode would have had to be handed a
 *   convention instead of an argument.
 *
 * SYS_TCB_CONFIGURE(tcb_cptr, cspace_cptr, vspace_cptr, proc_cptr) → 0 or iris_error_t
 *   tcb_cptr:    KOBJ_TCB with RIGHT_WRITE; must be unconfigured.
 *   cspace_cptr: KOBJ_CNODE — the root CNode the thread resolves CPtrs in.
 *   vspace_cptr: KOBJ_VSPACE — the address space it runs in.
 *   proc_cptr:   Stage 7 Step 1 — the process the thread JOINS, in arg3 (r10).
 *                KOBJ_PROCESS with RIGHT_MANAGE.  0 means the caller's own,
 *                which is what a thread creating a sibling wants; a spawner
 *                names its child, which it can do because it retyped that
 *                child's CSpace and VSpace and holds both.
 *   cspace_cptr and vspace_cptr must BE that process's root CNode and address
 *   space, checked by object identity rather than by convention — naming the
 *   pair is what makes the signature honest about which CSpace and VSpace the
 *   thread runs in.  The thread is left SUSPENDED: configuring is not starting.
 *
 * SYS_TCB_WRITE_REGS(tcb_cptr, entry, sp, arg) → 0 or iris_error_t
 *   Sets where a configured thread starts.  Refused once it has been made
 *   runnable: rewriting the entry frame of a thread that has run would
 *   corrupt the kernel stack it is standing on.
 *   SYS_TCB_RESUME refuses a configured thread that has NOT been through this
 *   (NOT_SUPPORTED): a thread with no entry frame has no kernel stack pointer
 *   to switch to, and making it runnable would take the CPU onto a null stack.
 */
#define SYS_CSPACE_SELF      119
#define SYS_TCB_CONFIGURE    120
#define SYS_TCB_WRITE_REGS   121
/*
 * SYS_VSPACE_MAP_TABLE(pt_cptr, vspace_cptr, vaddr) → 0 or negative iris_error_t
 *
 * Stage 6-pure Step 1 — seL4's seL4_X86_PageTable_Map.
 *
 * Installs a page table the caller RETYPED from its own Untyped
 * (IRIS_KOBJ_PAGE_TABLE, obj_arg 4096) at whichever paging level is the first
 * one missing for `vaddr` in the named address space.  The kernel picks the
 * level because the level is a property of the walk, not of the object — the
 * holder decides only that the table exists and where it goes.
 *
 *   pt_cptr:     KOBJ_PAGE_TABLE, RIGHT_WRITE.  Refused if already installed:
 *                one region cannot be two parts of a walk.
 *   vspace_cptr: KOBJ_VSPACE, RIGHT_WRITE.
 *   vaddr:       any address the table should serve; only the index bits for
 *                the level being filled matter.
 *
 * IRIS_ERR_ALREADY_EXISTS  the walk for `vaddr` is already complete — nothing
 *                          is missing, so nothing was installed.  The table is
 *                          untouched and can be installed somewhere else.
 * IRIS_ERR_BUSY            this table is already part of a walk.  Distinct
 *                          from the above on purpose: a client loop must tell
 *                          "this object is spent, retype another" from "this
 *                          level is already there, stop".
 * IRIS_ERR_INVALID_ARG     a huge-page leaf covers `vaddr`, or the address is
 *                          not one this VSpace maps.
 *
 * The VSpace holds a reference to every table installed in it and returns them
 * at teardown, so a holder cannot RESET the region a live walk is standing on.
 */
#define SYS_VSPACE_MAP_TABLE 122
/*
 * SYS_TCB_FAULT_INFO(tcb_cptr, out_uptr) → 0 or negative iris_error_t
 *
 * Stage 7 Step 8: the fault record read off the THREAD that took it, with
 * RIGHT_READ on that thread as the whole authority.  Replaces
 * SYS_PROCESS_FAULT_INFO (71, now NOT_SUPPORTED), which asked a process and
 * answered with whichever of its threads faulted last — and which was the one
 * remaining reason a fault handler needed a PROCESS capability at all.
 *
 * Layout is unchanged (<iris/fault_proto.h>, FAULT_MSG_LEN bytes).  A thread
 * with no pending fault answers IRIS_ERR_WOULD_BLOCK, which is what a handler
 * polling the mailbox capability for delivery wants.
 */
#define SYS_TCB_FAULT_INFO   123
/*
 * SYS_TCB_WATCH(tcb_cptr, notif_cptr, signal_bits) → 0 or iris_error_t
 *   Be told when this THREAD dies: signal_bits are OR'd into notif_cptr when
 *   it terminates, however it terminates.  RIGHT_READ on the thread (learning
 *   that something died confers nothing over it) and RIGHT_WRITE on the
 *   notification.  Arming an already-dead thread fires immediately, so a
 *   supervisor that lost the race still gets the answer.
 *
 *   Stage 7 Step 10, replacing SYS_PROCESS_WATCH: a supervisor HAS the thread
 *   it started — it retyped the TCB and configured it — and needed authority
 *   over a PROCESS to learn about that execution.  One watcher per thread, not
 *   an array: a second watcher is a second capability, not a second slot.
 *
 * SYS_TCB_EXIT_CODE(tcb_cptr) → the code, or IRIS_ERR_WOULD_BLOCK
 *   The code a thread exited with, read off that thread.  WOULD_BLOCK while it
 *   is still running.
 */
#define SYS_TCB_WATCH        124
#define SYS_TCB_EXIT_CODE    125
/*
 * SYS_TCB_SET_FAULT_HANDLER(tcb_cptr, notif_cptr, signal_bits, dest)
 *   Arm THIS THREAD's faults.  Stage 7 Step 12, replacing SYS_EXCEPTION_HANDLER
 *   (63): the handler, the mailbox and the fault generation are properties of
 *   an execution, and a supervisor arming a thread's faults already holds that
 *   thread.  `dest` is the mailbox, cnode|slot<<32, resolved in the
 *   REGISTRANT's CSpace — see SYS_EXCEPTION_HANDLER's retirement note for why
 *   it is named rather than assumed.  RIGHT_WRITE on the thread.
 */
#define SYS_TCB_SET_FAULT_HANDLER 126

/*
 * SYS_CSPACE_SET_GUARD(cptr, guard, guard_bits) → 0 or iris_error_t
 *
 * Stage 8-cap / ledger D-2 — install a GUARD on a CNode capability.
 *
 *   cptr        a CPtr, resolved in the caller's own CSpace, addressing the
 *               SLOT that holds the CNode capability.  Holding the slot is the
 *               authority: a guard changes how CPtrs resolve THROUGH that
 *               capability, which is a change to the holder's own address
 *               space and to nobody else's.
 *   guard       the value the guard bits must match.
 *   guard_bits  its width, 0..31.  Zero REMOVES the guard, which is the state
 *               every capability starts in and the behaviour IRIS had before
 *               guards existed.
 *
 * A guard is capability-local, exactly as in seL4: two capabilities to the
 * same CNode can carry different guards, so one holder can see a sparse CSpace
 * without changing anybody else's view.  Within one level a CPtr reads
 * [guard][index] from MSB to LSB, the same relative order seL4 resolves in.
 *
 * Errors: WRONG_TYPE (the slot does not hold a CNode — a guard on anything
 * else has no meaning and would make the slot lie about how it resolves),
 * NOT_FOUND (empty slot), INVALID_ARG (guard does not fit guard_bits, or
 * guard_bits + the target's radix would not fit the 31-bit CPtr space).
 *
 * Not yet expressible: a guard on the ROOT CNode capability.  A thread reaches
 * its root through a structural pointer rather than a slot, so there is no
 * capability to carry the guard.  That is the remaining half of D-2.
 */
#define SYS_CSPACE_SET_GUARD 127

/*
 * SYS_TCB_SET_TIMEOUT_HANDLER(tcb_c, notif_c, signal_bits, dest) → 0 or error
 *
 * Stage 8-mcs — arm a thread's TIMEOUT fault handler.
 *
 * Identical arguments and identical authority to SYS_TCB_SET_FAULT_HANDLER
 * (126): RIGHT_WRITE on the thread, a notification with RIGHT_WRITE, non-zero
 * signal bits, and a mailbox in the `cnode|slot<<32` packing every publishing
 * syscall uses, resolved in the REGISTRANT's CSpace.  It is the same mechanism
 * — a fault delivered by publishing the thread's capability and signalling.
 *
 * It is a SEPARATE registration on purpose.  The principal that answers "this
 * thread ran out of budget" is a temporal supervisor; the principal that
 * answers "this thread touched an unmapped page" is a pager.  One handler for
 * both would give the pager temporal authority over every thread it serves and
 * give the scheduler the power to resume a thread out of a page fault.  seL4
 * splits seL4_TCB_SetTimeoutEndpoint from the fault endpoint for this reason.
 *
 * When armed, budget exhaustion suspends the thread in TASK_BLOCKED_FAULT and
 * tells the handler, with fault vector IRIS_FAULT_VECTOR_TIMEOUT and no
 * rip/error/cr2 — there is no address involved.  Answered like any fault, with
 * SYS_EXCEPTION_RESUME carrying the generation the handler observed, so the
 * supervisor decides the policy the kernel deliberately does not have.
 *
 * Unarmed is the default and the pre-Stage-8 behaviour: the thread blocks
 * until its period refills the budget and nobody is told.
 */
#define SYS_TCB_SET_TIMEOUT_HANDLER 128

/*
 * SYS_REPLY_RECV(reply_cptr, msg_uptr, ep_cptr) → 0 or iris_error_t
 *
 * Stage 8-mcs — seL4's `seL4_ReplyRecv`: answer the outstanding call and wait
 * for the next one, with NO scheduling point in between.
 *
 * The atomicity is not an optimisation, it is what makes a PASSIVE server
 * work.  A passive server runs on time donated by whoever called it, and
 * SYS_REPLY gives that time back.  Doing reply and recv as two syscalls
 * therefore leaves the server, between them, runnable with no scheduling
 * context at all — and a thread with no SC is not charged, so it runs
 * unbudgeted.  That is the exact hole donation exists to close, reopened
 * one instruction after it was closed.  Here the server goes from "running on
 * donated time" straight to "blocked waiting", and is never in between.
 *
 *   reply_cptr  the KOBJ_REPLY holding the caller, with RIGHT_WRITE.  It is
 *               consumed by the reply and RE-STAGED for the next call, so a
 *               server keeps one reply object for its whole life.
 *   msg_uptr    carries the reply out and the next request back in — one
 *               buffer, as in seL4, which is why the operation is one call.
 *   ep_cptr     the endpoint to wait on, with RIGHT_READ.
 *
 * The reply half happens first and is not undone if the receive half fails:
 * the client has been answered, and reporting otherwise would be a lie.  An
 * error therefore means "answered, but not now waiting".
 *
 * REPLY_RECV does NOT carry a capability on the reply.  The buffer arrives
 * holding the kernel's own echo of the staged reply CPtr in attached_handle,
 * and passing that on would ask the kernel to transfer the reply object
 * itself; the field is cleared rather than left as a trap every server would
 * spring.  A server that means to send a capability is doing two things and
 * says so with SYS_REPLY and SYS_EP_RECV.
 */
#define SYS_REPLY_RECV 129

/*
 * SYS_TCB_SET_IPC_BUFFER(tcb_cptr, frame_cptr, uvaddr) → 0 or -iris_error_t
 *
 * Ledger D-4.  Give a thread an IPC BUFFER of its own: a frame it retyped, at
 * a size it chose, mapped where it chose, that the kernel uses instead of the
 * 256 bytes of staging every TCB carries today.
 *
 *   tcb_cptr    the thread to configure — needs RIGHT_WRITE
 *   frame_cptr  a KOBJ_FRAME cap with RIGHT_READ|RIGHT_WRITE, at least one
 *               page; IRIS_CPTR_NULL unregisters
 *   uvaddr      where the owner mapped that frame; page-aligned, user range,
 *               and 0 exactly when unregistering
 *
 * With buffers registered on BOTH ends, an endpoint transfer copies frame to
 * frame through the kernel's own window: no user pointer is named, so none
 * can be revoked between the check and the copy, and `msg.buf_uptr` is
 * ignored.  A thread with no registered buffer keeps the staging path, so
 * mixed systems work and the two ends need not agree.
 *
 * Errors: INVALID_ARG (not a TCB, not a frame, frame smaller than a page,
 * uvaddr not a page-aligned user address, or an address given while
 * unregistering), ACCESS_DENIED (missing rights on either capability).
 */
#define SYS_TCB_SET_IPC_BUFFER 130

/*
 * SYS_IOPORT_CONTROL_NARROW(auth_cptr, first | last<<16, budget_cptr, dest_slot)
 *   → 0 or negative iris_error_t
 *
 * Derive an I/O-port CONTROL capability over a SUB-RANGE of one you hold.
 *
 * This is what replaced the kernel's hardcoded port whitelist.  That table
 * said which ports anybody could ever claim — PS/2, two serial ports, QEMU's
 * ACPI block — and it was wrong in two ways at once.  The kernel had no basis
 * for the list (which ports exist is a fact about a machine; who may claim
 * them is a fact about who is trusted), and it applied to every holder
 * equally, so it could not say the only useful thing: that init may claim a
 * serial port and svcmgr may not.
 *
 * Boot issues the root task one control capability over the whole port space.
 * A supervisor narrows it for each delegate, and `SYS_CAP_CREATE_IOPORT`
 * checks a request against the authority the caller actually holds.
 * Confinement becomes something a supervisor decides and can prove, instead of
 * something the kernel asserts on everyone's behalf.
 *
 *   auth_cptr   an IRIS_BOOTCAP_IOPORT_CONTROL capability (RIGHT_DUPLICATE:
 *               this creates a copy of an authority, which is what that right
 *               governs everywhere else)
 *   first/last  the sub-range, inclusive, packed into arg1 — must be
 *               non-inverted and CONTAINED in the authority's own range, so a
 *               narrowing can only ever narrow
 *   budget_cptr the KUntyped the object is carved from.  Required, like every
 *               other device capability since Stage 7 Step 14: a narrowed
 *               control capability is memory, and a syscall that let ring 3
 *               spend the KERNEL's would open a charter M3 hole in the same
 *               change that closed a policy one
 *   dest_slot   destination slot in the caller's root CNode
 *
 * The result is an MDB CHILD of the slot that authorised it, like every other
 * derived capability, so revoking the parent reaches it.
 *
 * Errors: INVALID_ARG (inverted range, no destination), ACCESS_DENIED (not an
 * ioport control capability, missing RIGHT_DUPLICATE, or a range that is not
 * contained in the caller's).
 */
#define SYS_IOPORT_CONTROL_NARROW 131

/*
 * SYS_UNTYPED_SET_DEVICE_BUDGET(device_ut_cptr, ram_ut_cptr)
 *   → 0 or negative iris_error_t
 *
 * Name the RAM Untyped that pays for the object HEADERS of everything retyped
 * out of a DEVICE Untyped (ledger D-9).
 *
 * A device region is MMIO.  The kernel can hand it out and a driver can map
 * it, but nothing can be STORED in it: a `struct KFrame` written into a
 * framebuffer is pixels, and read back it is whatever the display controller
 * left there.  A RAM Untyped carves its objects' headers out of its own top
 * end; a device one cannot, and the only answer that does not put the kernel
 * back in the business of allocating on somebody's behalf is that the HOLDER
 * names the memory.
 *
 * Both arguments need RIGHT_WRITE.  Set ONCE: a pairing that could move would
 * let a holder point the device Untyped at a second budget and RESET the
 * first, reclaiming a region while the headers describing live device frames
 * were still in it.  The RAM Untyped is retained while paired, so its own
 * RESET already refuses for as long as any header carved from it is alive.
 *
 * A device Untyped with no budget REFUSES to retype (INVALID_ARG) rather than
 * falling back to kernel memory, which is the honest failure: the kernel does
 * not know whose memory to spend and will not guess.
 *
 * Errors: INVALID_ARG (either is not an Untyped, the first is not a device
 * region, the second IS one, or they are the same), ACCESS_DENIED (missing
 * RIGHT_WRITE), ALREADY_EXISTS (already paired).
 */
#define SYS_UNTYPED_SET_DEVICE_BUDGET 132

/*
 * SYS_FRAMEBUFFER_INFO(auth_cptr, info_uptr) → 0 or negative iris_error_t
 *
 * The framebuffer's GEOMETRY: width, height, stride, bpp and the physical
 * region it occupies.  Nothing else — no object is created and nothing is
 * consumed, so it can be asked twice.
 *
 * Its predecessor, `SYS_FRAMEBUFFER_VMO`, answered this question AND
 * fabricated a KVMO over the region in the same call, which is why the
 * geometry could only be learned by accepting a kernel-made object.  Since
 * Stage 6 the region is published as a DEVICE Untyped (ledger D-9) and a
 * driver retypes a frame from it, so the two halves are separable and only
 * one of them is still the kernel's business: the geometry is a fact about the
 * hardware that boot discovered, and a capability is not.
 *
 *   auth_cptr  the framebuffer control capability (IRIS_BOOTCAP_FB_CONTROL)
 *   info_uptr  where to write `struct iris_fb_params`
 *
 * Errors: ACCESS_DENIED (not the framebuffer control capability), NOT_FOUND
 * (the machine has no framebuffer), INVALID_ARG (unwritable buffer).
 */
#define SYS_FRAMEBUFFER_INFO 133

/*
 * SYS_INITRD_FRAME(auth_cptr, index, dest_cnode|slot<<32, budget_cptr)
 *   → image size in bytes, or negative iris_error_t
 *
 * A boot image, as a FRAME.
 *
 * Its predecessor `SYS_INITRD_VMO` handed out a KVMO — one of the three object
 * types seL4 has no equivalent for, and the reason the loader and vfs both had
 * to speak a second memory ABI to read a file the kernel already had.  seL4
 * has no VMO and no initrd object: the root task is given its boot image as
 * memory it can map, and everything above that is its own arrangement.
 *
 * The frame is a contiguous, page-aligned region carved from `budget_cptr`
 * with the image copied into it and the tail zeroed, published RIGHT_READ as
 * an MDB child of the slot that authorised the read — so revoking the initrd
 * authority reaches the images it produced.  One `SYS_FRAME_MAP` maps the
 * whole thing (a frame maps as a whole, ledger D-10), which is what makes the
 * VMO's page-at-a-time machinery unnecessary here.
 *
 * The SIZE is the return value rather than a second syscall: a caller that has
 * to ask how big the thing it was just given is has been given two things.
 *
 * Errors: ACCESS_DENIED (not the initrd control capability), NOT_FOUND (no
 * such index), INVALID_ARG (no destination, no budget, or a budget that is not
 * an Untyped), NO_MEMORY.
 */
#define SYS_INITRD_FRAME 134

#define IRIS_UNTYPED_QUERY_VERSION 1u
#define IRIS_UNTYPED_QUERY_GLOBAL  1u
#define IRIS_UNTYPED_QUERY_ONE     2u
#define IRIS_UNTYPED_QUERY_OBJECTS 3u
#define IRIS_UNTYPED_QUERY_TASKOBJ 4u  /* Phase S2: TCB/SC gauges + CDT counters */

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
    /*
     * Stage 7-mem: the GLOBAL half of what SYS_RESOURCE_INFO reported.
     *
     * That syscall answered "how much has this PROCESS spent", which is a
     * question the Untyped it was given answers better and will answer after
     * KProcess is gone.  But three of its fields were never per-process: the
     * kernel slab's occupancy and the global charge/rollback counters are
     * facts about the kernel, and the drift tests that end most of the suite
     * read them to prove nothing leaked.  Appended here — prefix-compatible,
     * like every other growth of this struct — so they survive the retirement
     * of the syscall that happened to carry them.
     */
    uint32_t kslab_used_bytes;
    uint32_t kslab_total_bytes;
    uint32_t kslab_failed_allocs;   /* named to avoid the purity gate's
                                    * textual `kslab_alloc` match — the gate
                                    * counts mentions, deliberately */
    uint32_t global_failed_charges;
    uint32_t global_rollbacks;
    /*
     * Stage 9-evt Step 1 — how many times a syscall has been RE-EXECUTED
     * because its handler parked the thread and asked to be re-entered
     * (ledger D-1).
     *
     * Instrumentation, never authority, like everything else in this struct.
     * It is here because the restart path is otherwise invisible: a
     * restartable sleep and a stack-parked sleep look identical from ring 3,
     * and the only way to assert that a blocking path was actually converted
     * is to watch the counter move.
     */
    uint32_t syscall_restarts;
    /*
     * Stage 9-evt Step 2 — syscalls that resumed on a FRESH kernel stack,
     * their original frame abandoned (ledger D-1).
     *
     * Separate from `syscall_restarts` because a restart that yielded through
     * its own frame and one that threw it away are indistinguishable from
     * outside, and only the second is what D-1 is about.
     */
    uint32_t syscall_abandons;
    /*
     * Stage 9-evt Step 3 — ring-3 kernel entries whose user context was saved
     * into the interrupted thread's TCB rather than left on a kernel stack
     * (ledger D-1).
     *
     * Visible from ring 3 because the step it belongs to is otherwise
     * invisible: with per-thread kernel stacks still in place the save and the
     * restore are an identity, and a path that does nothing observable is a
     * path that rots.  The counter is the one thing that proves it runs.
     */
    uint32_t irq_ctx_saves;
    /*
     * Stage 8-cap — threads currently holding a registered IPC buffer frame
     * (ledger D-4).  A LIVE count, not a total.
     *
     * Here because the migration off the kernel's 256-byte staging fails
     * silently by design: a service whose registration is refused keeps
     * working on the old path and nothing notices.  It happened on the first
     * service tried.
     */
    uint32_t ipc_buffers;
    /*
     * Stage 9-evt step 3 — free pages in the kernel's physical allocator.
     *
     * Here because the claim step 3 makes is about MEMORY and was otherwise
     * unobservable: every thread used to own two pages of kernel stack plus a
     * guard page, taken from this reserve at creation, so kernel memory grew
     * with the thread count and nothing from ring 3 could see it.  A thread
     * owns no kernel stack now, and this is what lets a test say so.
     */
    uint32_t kernel_free_pages;
    /*
     * Stage 9-evt — 1 once the kernel's boot arena is SEALED.
     *
     * seL4 has no kernel heap: its boot code carves the root task's initial
     * objects from a statically-known region and describes everything else as
     * Untyped, after which the kernel allocates nothing.  IRIS's `kslab` is
     * that region, and this says the door is shut — the kernel's slab
     * allocator panics from here on.  Reported because a property nothing can observe is a property
     * that stops being true without anyone noticing.
     */
    uint32_t kernel_heap_sealed;
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

/* Phase S2 — task-object gauges + CSpace-native derivation counters. */
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
    /* Phase S2 Step C — KTCB registry (references, not payload). */
    uint32_t tcb_registry_active;
    uint32_t tcb_registry_hwm;
    uint32_t tcb_registry_exhaustions;
    uint32_t tcb_registry_generation_mismatch;
    /* Phase S3 — native MDB/CDT gauges (prefix-compatible append, C.1). */
    uint32_t mdb_nodes_live;         /* occupied slots participating in the MDB */
    uint32_t mdb_nodes_hwm;
    uint32_t mdb_legacy_roots;       /* live LEGACY_ROOT caps (must → 0, Etapas 2-4) */
    uint32_t mdb_orphan_promotions;  /* children promoted to root by root-delete */
    uint32_t mdb_reparents;          /* children adopted by grandparent on delete */
    uint32_t mdb_revoked_nodes;      /* caps destroyed by revoke */
    uint32_t mdb_moves;
    uint32_t mdb_max_depth;
};
#endif /* !__ASSEMBLER__ */

/*
 * Block 8 — TCB capabilities (Ph96-101).
 *
 * Each user thread receives a KTcb at creation time; handles are installed
 * in the owning process's handle table automatically.
 *
 * SYS_TCB_SELF(dest) → handle_id, or 0 when dest names a slot, or negative
 *   iris_error_t   (Stage 4 destination slot in arg0, RETYPE2 packing —
 *   see SYS_VSPACE_SELF)
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
#define IRIS_HANDLE_TYPE_VSPACE         14u  /* Phase 4: KVSpace — virtual address space */
#define IRIS_HANDLE_TYPE_FRAME          15u  /* Phase 5: KFrame  — physical memory frame */
#define IRIS_HANDLE_TYPE_PAGE_TABLE     16u  /* Stage 6-pure: a retyped paging level */

#ifndef __ASSEMBLER__
/* struct iris_resource_info DELETED (Stage 7-mem) with SYS_RESOURCE_INFO.
 * Per-process accounting is gone: a VMO's cost is the Untyped it was carved
 * from.  The three global gauges it carried are appended to
 * struct iris_untyped_query_global. */
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
