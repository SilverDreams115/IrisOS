/*
 * it_priv.h — the iris_test suite's own interface to itself.
 *
 * The suite was one translation unit of 26,570 lines.  It is ten now, split
 * by area at test-number boundaries, and this header is what they share: the
 * includes, the macros — the CSpace slot map above all, which is the one
 * piece of global state a test can corrupt for every other test — the types,
 * the small syscall wrappers, and a declaration for everything that outlives
 * its own file.
 *
 * The declarations are exhaustive rather than curated, and that is deliberate:
 * everything here was `static` in one file, so the split had to preserve the
 * program exactly.  Narrowing this interface is a separate change, and one
 * that can now be made a piece at a time, because there is an interface.
 */
#ifndef IRIS_TEST_IT_PRIV_H
#define IRIS_TEST_IT_PRIV_H

#include <stdint.h>
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/error.h>
#include "../common/iris_vspace.h"
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/fault_proto.h>
#include <iris/ipc_msg.h>
#include <iris/user_ctx.h>
#include <iris/ipc_recv_slot.h>
#include <iris/endpoint_proto.h>
#include "../common/iris_timer.h"
#include <iris/fb_info.h>
#include "../common/iris_ipc_buffer.h"
#include <iris/vfs_ep_proto.h>
#include <iris/kbd_ep_proto.h>
#include <iris/console_ep_proto.h>
#include "../common/svc_loader.h"

/*
 * Stage 6-pure Step 2: the suite supplies its own paging levels.
 *
 * The kernel stopped creating page tables, so a map whose walk is incomplete
 * answers IRIS_ERR_MISSING_TABLE and expects the holder to retype a level and
 * retry.  That belongs at the syscall boundary rather than at each of the
 * hundred-odd map sites in this file: it is one rule about the address space,
 * not a hundred decisions, and this is where a seL4 client library puts it.
 *
 * Only the four mapping syscalls are eligible, and each says where its target
 * address space and virtual address are — SYS_VMO_MAP maps into the caller's
 * own, the rest name theirs.  Anything else that ever returned MISSING_TABLE
 * would be a kernel bug, so it is passed through unchanged.
 */
/* Leaves of the suite's own object CNode, not root slots: the four root
 * SCRATCH slots have a contract ("empty between tests") that a permanent
 * occupant would break, and the rotating object pool only ever hands out
 * leaves 1..IT_OBJ_SLOT_SPAN, so anything above that is ours alone. */
#define LP_SLOT_BUDGET  16u   /* mirrors lifecycle_probe's own map */
/* Leaves 252/253: 201/202 were IT_FAULT_LEAF(0)/(1), the mailbox each fault
 * delivers a TCB into — and this fixup DELETES its slot before retyping a
 * level into it.  The same class as the leaf-index delete T319 now guards:
 * a slot range is not free because the constant naming it reads scratch. */
#define IT_PT_SCRATCH    IT_OBJ_CPTR(252)  /* the level's capability */
#define IT_PT_VS_SCRATCH IT_OBJ_CPTR(253)  /* a child's VSpace, for MAP_INTO */
/* The suite's OWN address space, as a capability.  Ledger D-5: SYS_VMO_MAP
 * mapped into the caller's address space implicitly and SYS_FRAME_MAP names
 * it, so every map site needs this — including the ones that run long before
 * the VM tests do. */
#define IT_VS       ((long)IRIS_CPTR_TEST_VSPACE)

/* ── Phase S1: object-creation helpers ───────────────────────────────────────
 *
 * SYS_ENDPOINT_CREATE / SYS_NOTIFY_CREATE / SYS_CNODE_CREATE are RETIRED:
 * every kernel object the suite fabricates is retyped from its delegated
 * untyped (IRIS_CPTR_TEST_UNTYPED, slot 55) via SYS_UNTYPED_RETYPE2.  The
 * new capability lands in a scratch CSpace slot (rotating pool 100..219,
 * atomic — creation happens from helper threads too), is materialized to a
 * handle (SYS_CSPACE_RESOLVE — the sanctioned CSpace→handle bridge) and the
 * scratch slot is deleted, so the handle-based test flows keep exercising
 * the handle-table semantics unchanged while every BIRTH is Untyped+CSpace.
 */
/* 64..87, below the fz pool (100..239), split between the two factories.
 *
 * They CANNOT share a pool.  The materialising factory only BORROWS its slot —
 * it resolves and deletes immediately — while the slot factory LEAVES the
 * capability there for the test's duration.  One rotation across both means a
 * borrow eventually lands on a live object and deletes it: a fault with no
 * local symptom, because the test that fails is not the one that caused it. */
/* The held pool cannot live in the root CNode: it is full, and eight slots is
 * nowhere near enough for a suite that fabricates from worker threads too.
 * Root slot 80 holds a 256-slot SECOND-LEVEL CNode instead, its leaves
 * addressed (leaf << 8) | 80 — the multi-level CSpace the model always
 * described and the ten-bit CPtr window made unreachable.  Usable only since
 * handles moved to the top of the word. */
#define IT_OBJ_CNODE_SLOT   80u
#define IT_OBJ_SLOT_SPAN   200u
#define IT_OBJ_CPTR(leaf)  ((uint32_t)(((leaf) << 8) | IT_OBJ_CNODE_SLOT))
/*
 * Stage 7 Step 9 — a child's ROOT CSPACE, kept so the suite can go on
 * delegating into it after the spawn.
 *
 * Minting into a child used to name its PROCESS, out of which the kernel read
 * `child->cspace_root`; a spawner reached a CSpace it did not hold by naming
 * something else.  It holds it now, and these are the leaves it holds it in.
 *
 * Leaves 1..3, and the reason is arithmetic: a mint SOURCE must be a root CPtr
 * (< 1024), and a two-level CPtr is `leaf << 8 | root_slot`, so only leaves
 * 1..3 of a 256-slot root's child CNode are addressable as one.  The rotating
 * object pool therefore starts at 4 — three is enough, because the most
 * children the suite has live and delegable at once is two (T183, T184).
 */
/* Mint into a slot of MY OWN root CSpace.  SYS_CSPACE_MINT's dest_cnode of 0
 * has meant "the caller's root" since Phase S3 — which is why minting into
 * yourself never needed a capability to your own process, and why the
 * process-shaped variants had nothing to offer this case. */
#define IT_MINT_SELF(slot)   ((long)((uint64_t)(slot) << 32))
/*
 * Stage 7 Step 10 — the suite's child table.
 *
 * Observing a death names the THREAD that dies (SYS_TCB_WATCH), so a
 * supervisor keeps the TCB it retyped for each child.  This is the table a
 * process server keeps, in the smallest form that serves the suite: the
 * process capability the tests already pass around, mapped to the thread it
 * was started with.  When SYS_PROCESS_KILL and its siblings retire, this is
 * what the remaining call sites will already be reading.
 */
/*
 * A CNODE OF ITS OWN, because the table has to hold as many children as the
 * suite holds at once and the object CNode cannot spare that many leaves.
 *
 * Stage 7 Step 13 is what forced this: while SYS_PROCESS_KILL existed, a test
 * that spawned more children than the table could hold still killed them all —
 * it named the PROCESS, and every process capability is in the root CSpace.
 * Killing names the thread now, so a child the table has evicted is a child
 * nothing can stop, and T240 holds 48 at once.  48 leaves is more than the
 * object CNode has left, so the child threads get their own second level: 64
 * slots at root slot 84, leaves 1..48, addressed exactly like every other
 * second-level capability.
 */
#define IT_CHILD_CN_SLOT     84u
#define IT_CHILD_CN_SLOTS   128u
#define IT_CHILD_MAX         48u
/*
 * Stage 7 Step 15: two leaves per child, paired by construction — the thread at
 * 1..48 and the ADDRESS SPACE at 49..96.  No second table field, because the
 * pairing IS the arithmetic: a child's VSpace leaf is its thread's plus
 * IT_CHILD_MAX, so one recorded number names both.
 */
#define IT_CHILD_TCB_LEAF(k) (1u + (uint32_t)(k))
#define IT_CHILD_VS_LEAF(k)  (1u + IT_CHILD_MAX + (uint32_t)(k))
struct it_child { uint32_t proc; uint32_t leaf; };
#define IT_CHILD_CN_DEST_(leaf) \
    ((uint64_t)IT_CHILD_CN_SLOT | ((uint64_t)(leaf) << 32))
#define IT_CHILD_TCB_DEST(leaf) IT_CHILD_CN_DEST_(leaf)
#define IT_CHILD_TCB_CPTR(leaf) \
    ((uint32_t)(((leaf) << 8) | IT_CHILD_CN_SLOT))

/* task_state_t ABI values observed through iris_tcb_info.state (mirrors
 * kernel/include/iris/task.h — asserted stable by these tests). */
#define IT_TASK_SUSPENDED   10u
#define IT_TASK_TERMINATED  11u
#define IT_TASK_DEAD        12u

/* ...and into a slot of a CHILD's root CSpace, named by the capability the
 * spawn kept (IT_CHILD_CN_CPTR).  Same syscall, different destination CNode:
 * that a child's CSpace is somebody else's is a fact about which capability
 * you hold, not about which syscall you call. */
#define IT_MINT_INTO(cn, slot) \
    ((long)((uint64_t)(cn) | ((uint64_t)(slot) << 32)))
#define IT_CHILD_CN_LEAF(i)  (1u + (uint32_t)(i))
#define IT_CHILD_CN_CPTR(i)  ((long)IT_OBJ_CPTR(IT_CHILD_CN_LEAF(i)))
#define IT_CHILD_CN_DEST(i) \
    ((uint64_t)IT_OBJ_CNODE_SLOT | ((uint64_t)IT_CHILD_CN_LEAF(i) << 32))
#define IT_TCB_CNODE_SLOT 67u
#define IT_TCB_SLOTS      128u
#define IT_TCB_CPTR(leaf) ((uint32_t)(((leaf) << 8) | IT_TCB_CNODE_SLOT))

#define IT_OBJ_POOL_FIRST    4u
/*
 * Ledger A-22 — the fault REPLY objects.
 *
 * A fault is a CALL on an endpoint, so serving one means receiving with reply
 * authority staged, and the bound reply capability IS "may resume that
 * thread".  The suite supervises its own targets, so its reply objects live in
 * its own second-level object CNode — leaves ABOVE the rotating pool
 * (1..IT_OBJ_SLOT_SPAN), so an outstanding fault's authority is never recycled
 * out from under a test, and indexed so concurrent targets each get one.
 *
 * These leaves used to be the fault MAILBOX: the CNode slot the kernel minted
 * the faulting THREAD's capability into on every fault, which SYS_EXCEPTION_
 * RESUME then named.  A reply capability replaces both, and holds strictly
 * less — resuming one call rather than everything a TCB capability permits.
 */
#define IT_FAULT_LEAF(i)   (IT_OBJ_SLOT_SPAN + 1u + (uint32_t)(i))
#define IT_FAULT_CPTR(i)   ((long)IT_OBJ_CPTR(IT_FAULT_LEAF(i)))

/*
 * ...and the reply objects for faults somebody ELSE handles.
 *
 * A pager serves targets whose faults this suite armed, so the reply authority
 * has to be somewhere the PAGER can reach.  A CNode retyped here, filled with
 * KReply objects and minted into the pager is that place.
 *
 * A FRESH CNode PER PAGER, retyped through one slot rather than parked in
 * several.  Two pagers must not share one — T183 and T190 run two at once —
 * but the root CNode has almost no unassigned slots left, and a mint source
 * must be a root CPtr.  Retyping a new one through the same slot gives each
 * pager a distinct OBJECT: the mint the previous pager holds keeps its CNode
 * alive after the slot has moved on.
 */
#define IT_PGR_MBOX_SLOT   83u
#define IT_PGR_MBOX_SLOTS  32u
#define IT_PGR_MBOX_DEST(leaf) \
    ((long)((uint64_t)IT_PGR_MBOX_SLOT | ((uint64_t)(leaf) << 32)))
/* Fixed slots for capabilities that outlive a test (fuzz worker control). */
#define IT_FZ_CTL_SLOT      81u
/* Stage 4: the loader's workspace CNode.  svc_load_minted_ws publishes the
 * child's process capability, its ELF/segment/stack VMOs and its own working
 * caps into leaves of this CNode instead of returning handles — the legacy
 * arity (no workspace) exists only for callers that had nowhere to put them,
 * and this suite was the last one.  Slot 82 is free: the S1 scratch pool ends
 * at 79 and the object CNode + fuzz control take 80/81. */
#define IT_LOADER_WS_SLOT   82u
#define IT_LOADER_WS \
    SVC_LOADER_WS((uint32_t)IRIS_CPTR_TEST_UNTYPED, IT_LOADER_WS_SLOT)

/* Step 4: the same fabrication, but the capability STAYS in its slot — which
 * is the seL4 shape, the capability IS the slot.  Used by every test whose
 * subject is an authority property rather than the handle namespace itself.
 * Same rotating pool and the same contract: delete before use, never hold a
 * slot across a test boundary. */
/* Fabricate into a leaf of the second-level CNode and LEAVE it there — the
 * capability IS the slot, which is the seL4 shape.  Returns the full CPtr. */
/* Fill a svc_mint source from a capability value, choosing the field by
 * namespace: src_cptr wins over src_h in svc_load_minted and mints
 * slot-to-slot, so the child's cap becomes an MDB child of ours.  Assigning a
 * CPtr to src_h instead silently produces a mint the kernel cannot resolve. */
#define IT_MINT_SRC(m, v)                                                     \
    do {                                                                      \
        uint32_t _v = (uint32_t)(v);                                          \
        if (_v != 0u && (_v & HANDLE_TAG) == 0u) (m).src_cptr = _v;           \
        else                                     (m).src_h    = (handle_id_t)_v; \
    } while (0)

/* Stage 5 Step 4: threads are retyped from an Untyped and configured with
 * CSpace/VSpace capabilities — defined next to the VSpace helpers it needs. */
/* A thread argument asking to be told which thread you are: whoever creates
 * a thread holds its TCB, and hands it over in the entry register. */
#define IT_THREAD_ARG_SELF_TCB 0xFFFFFFFFFFFFFFFFULL

/* ── Phase S4 (Step 2): CSpace-sourced cap transfer ───────────────────────
 * The IPC transfer SOURCE is a CSpace slot, never a handle (charter §3.6/A6).
 * it_xfer_slot mints the cap behind src_h into `slot` of iris_test's own root
 * CNode and returns the CPtr to hand to EP_SEND / EP_CALL / SYS_REPLY.
 * Requires RIGHT_DUPLICATE on src_h (the mint) and grants RIGHT_TRANSFER on
 * the slot (the transfer itself).  Ledger A-29: the transfer is a COPY, so
 * the slot SURVIVES the delivery holding the parent of what the receiver got;
 * a sender giving the capability away calls it_xfer_release afterwards.
 * The slot is deleted up front so re-entry (and any stale occupant) is clean.
 * 247..250 sits ABOVE every reserved pool — S1 scratch (64..87), the fixed
 * reply-object slots (88..97) and the fuzzing pool (100..239) — and inside the
 * 256-slot root CNode.  Overlapping any of those silently deletes a live
 * object at mint time (it cost a hang in T087 during Step 2 bring-up). */
/* ── Phase S4: iris_test root-CNode slot map (256 slots) ───────────────────
 * Reserved elsewhere, do NOT reuse: 1..15 well-known bootstrap · 25..31 test
 * fixtures · 36..43 + 48..51 per-test slots · 44..47 T100 lookup (computed
 * 44+i) · 52..59 file-grant sessions and CPtr constants · 64..87 S1 scratch ·
 * 88..97 fixed reply objects · 100..239 fz monotonic pool.
 * Also reserved: 239/240 (T253 partial-fill probes) and 246..253 (T253's
 * failing 8-CNode capacity batch — a slot left OCCUPIED there turns its
 * expected NO_MEMORY into ALREADY_EXISTS).  The transfer pool at 247..250
 * sits inside that range and is safe ONLY because a committed delivery
 * consumes its slot and every negative test deletes its own.
 * A mint into an occupied slot DELETES the occupant, so an overlap silently
 * destroys a live object — that cost two hangs during bring-up. */
/* The only slots genuinely unassigned in this process (verified by enumerating
 * every IRIS_CPTR_ and BOOT_CPTR_ constant, every _SLOT define and every
 * range-reserved pool): 29, 43, 63, 99, 254, 255. */
/* Stage 5 Step 2: slot 99 is IRIS_CPTR_FB_CONTROL, the framebuffer control
 * capability.  T134's guaranteed-EMPTY probe moved to a scratch slot it
 * deletes itself, which is a stronger guarantee than a slot everyone was
 * asked to leave alone. */
#define IT_SERIAL_SLOT 255u    /* this process's serial KIoPort */
/* Shared SCRATCH pool.  Slots are scarce (the CPtr namespace is capped at
 * <1024 and the root CNode has 256 slots, so only these were unassigned), and
 * the tests that need them run SEQUENTIALLY and clean up, so one pool serves
 * both the device-capability tests and the CDT derivation tests.  Contract:
 * ALWAYS delete before minting (every helper below does), and never hold a
 * scratch slot across a test boundary. */
#define IT_SCRATCH_0    29u
#define IT_SCRATCH_1    43u
#define IT_SCRATCH_2    63u
#define IT_SCRATCH_3   254u

#define IT_DEV_SLOT_A  IT_SCRATCH_0   /* device caps (rotating pair) */
#define IT_DEV_SLOT_B  IT_SCRATCH_1
#define IT_DEV_MINT_A  IT_SCRATCH_2   /* derived device caps (native CDT) */
#define IT_DEV_MINT_B  IT_SCRATCH_3

/* Stage 5: a narrowed I/O-port CONTROL capability, derived by the suite from
 * its own.  Its own slot rather than one of the device-cap scratch pair,
 * because the narrowing tests use those as DESTINATIONS and a source that is
 * also a destination gets deleted out from under itself. */
#define IT_IOCTL_NARROW 251u

#define IT_XFER_SLOT_A  247u
#define IT_XFER_SLOT_B  248u
#define IT_XFER_SLOT_C  249u
#define IT_XFER_SLOT_D  250u

/* Stage 4: naming iris_test's OWN root CNode.  This used to probe the handle
 * table for the first CNODE-typed generation-1 id, which only worked because
 * the kernel published every process's root CNode as its first handle.  The
 * root is structural now and lives in no handle table, so the operations that
 * needed it (DELETE a CSpace slot before re-minting) pass arg0 == 0 — the
 * "my own root CNode" convention.  It is a constant, never HANDLE_INVALID, so
 * the old "no root found" guards are gone with the probe. */
#define T28_OWN_ROOT_CNODE 0u

/* Drop-in replacement for the pre-S4 "dup a handle, attach it" idiom: mints
 * into a rotating transfer slot (88..95) and returns the CPtr to attach.
 * The rotation keeps sequential/nested transfers from colliding. */
#define IT_XFER_SLOT_SPAN 4u   /* 247..250 */

/* ── T004-T007 retired (Phase 13/Track F) ───────────────────────────────
 * The KChannel-specific tests (loopback, NB-recv-empty, recv-timeout,
 * seal) are superseded by endpoint/notification equivalents:
 *   T004 → T015 (EP_SEND/RECV)      T005 → T014 (EP_NB_RECV empty)
 *   T006 → T010 (NOTIFY_WAIT_TIMEOUT) T007 → T019 (endpoint close).      */

/* ── T008: VMO create + map + rw + unmap ────────────────────────────────── */

#define T008_VMO_ADDR  0x8050000000ULL
#define T008_VMO_SIZE  4096U

/* ── T017 — RETIRED with the kernel futex (charter P2) ─────────────────
 * Its subject was SYS_FUTEX_WAIT's timeout.  A futex is a synchronization
 * PRODUCT and it was in the kernel — a hash table, a kernel-invented ceiling
 * of 256 waiters, and a blocking wait keyed on a raw user address instead of a
 * capability.  seL4 has none; one is built in user space from a shared FRAME
 * for the word and a NOTIFICATION for the sleep.
 *
 * The property that outlives it — a blocking wait that returns TIMED_OUT
 * rather than hanging — is T010's, on SYS_NOTIFY_WAIT_TIMEOUT, which is the
 * mechanism a user-space futex would sleep on.
 *
 * The Stage 4 rule, unchanged: a test whose SUBJECT is the retired mechanism
 * dies with it; one asserting a property that survives is rewritten. */

/* ── T024: SYS_REPLY transfers an attached cap to the EP_CALL caller ────── */

#define T024_GOT_SLOT IT_OBJ_CPTR(245u)   /* outside the rotating pool */
#define IT_EP_IO_CAP ((uint32_t)VFS_EP_DATA_MAX)

/* Stage 4: leaves of the second-level CNode reserved for capabilities the
 * suite LOOKS UP and holds for the rest of the run.  Outside the rotating
 * fabrication pool (leaves 1..200), so a rotation can never reclaim one. */
#define IT_LOOKUP_VFS  IT_OBJ_CPTR(240u)
#define IT_LOOKUP_KBD  IT_OBJ_CPTR(241u)
#define IT_LOOKUP_CON  IT_OBJ_CPTR(242u)
#define IT_LOOKUP_TMP  IT_OBJ_CPTR(243u)

/* ── Ring-3 spawn/kill lifecycle harness (T075+) ────────────────────────────
 *
 * iris_test acts as the PARENT: using only its own IRIS_CPTR_PROC_CONTROL
 * KBootstrapCap it spawns the minimal `lifecycle_probe` child (svc_load), hands
 * it exactly one command endpoint (minted into the child's CPtr slot
 * LP_CPTR_CMD_EP), and observes the child's lifecycle via SYS_PROCESS_WATCH /
 * SYS_PROCESS_EXIT_CODE / SYS_PROCESS_KILL — all capability-scoped, no global
 * authority.  These constants must match services/lifecycle_probe/main.c. */
#define LP_CPTR_CMD_EP  3u
#define LP_EXIT_MARKER  0x1E57

/* ── T076: child mapping teardown ───────────────────────────────────────────
 * Map a parent-owned VMO into the child's address space, let the child block
 * with the mapping live, then kill the child.  The kernel's teardown must reap
 * the child's address space (auto-unmapping the VMO) without freeing the VMO
 * itself — the parent still owns it.  Verified by re-mapping the same VMO into
 * the PARENT afterwards and reading/writing it: no panic, no stale mapping, no
 * corrupted refcount.  (LP_MAP_VA = USER_VMO_BASE, the same known-good VA T008
 * maps at; child and parent live in different address spaces.) */
#define LP_MAP_VA  0x8050000000ULL

/* ── Bootstrap ──────────────────────────────────────────────────────────── */

/*
 * Receives the SPAWN_CAP bootstrap cap from init (serial/test loading).
 * Timeout-bounded so a missing message degrades to HANDLE_INVALID — the
 * dependent tests then FAIL loudly instead of hanging boot or being
 * silently skipped.  (Phase 8: the discovery endpoint no longer arrives
 * here; it is the well-known slot IRIS_CPTR_SVCMGR_EP.)
 */
/* it_recv_bootstrap retired — Phase 13/Track I: the spawn cap is the
 * IRIS_CPTR_PROC_CONTROL pre-start mint, not a bootstrap KChannel message. */

/* ── T079: VMO map by CPtr (A1 Increment 1) ─────────────────────────────────
 * SYS_VMO_MAP now resolves the VMO through the dual resolver: a CSpace slot
 * (< 1024) must work exactly like a handle (>= 1024).  init mints our OWN
 * process cap (RIGHT_WRITE) at IRIS_CPTR_TEST_PROC; we use it to
 * SYS_PROC_CSPACE_MINT a runtime-created VMO into our own slots, map by CPtr
 * and prove the mapping is real (write, then read the same pages back through
 * a second raw-handle mapping — which also re-proves the old handle path).
 * Failure paths: empty slot (clean error), wrong type (slot 30 is a
 * KNotification → WRONG_TYPE), insufficient rights (read-only mint + writable
 * map flags → ACCESS_DENIED). */

/* Dynamic slots for this fixture, and they must be slots NOBODY ELSE writes.
 * They were 16/17/18: 18 became IRIS_CPTR_OWN_VSPACE when D-6 delegated a
 * service its own address space, so "never minted — must not resolve" stopped
 * being true and this test kept passing because a VSpace answers WRONG_TYPE,
 * which is also negative.  A slot's emptiness is a claim about the whole
 * system, and it expires. */
#define T079_SLOT_RW    16L               /* dynamic slot: frame, READ|WRITE */
#define T079_SLOT_RO    17L               /* dynamic slot: frame, READ only  */
#define T079_SLOT_EMPTY 20L               /* never minted — must not resolve */
#define T079_VA_CPTR    0x8060000000ULL
#define T079_VA_HANDLE  0x8061000000ULL

/* ── T080: VMO remaining syscalls by CPtr (A1 Increment 1b) ─────────────────
 * SYS_FRAME_SIZE / SYS_FRAME_MAP / SYS_PROC_CSPACE_MINT resolve their VMO
 * argument through the dual resolver (the target/destination process stays
 * handle-only).  Reuses the T079 fixture: the self-proc cap at
 * IRIS_CPTR_TEST_PROC mints a runtime VMO into own slots — 19 (READ|WRITE|
 * DUPLICATE) and 20 (READ only) — and the lifecycle_probe child is the
 * SHARE/MAP_INTO target.  Proofs per syscall:
 *   SIZE:     CPtr returns the real size; empty slot / wrong type fail;
 *             raw handle still works.
 *   SHARE:    CPtr shares into the child; READ-only slot → ACCESS_DENIED
 *             (needs READ|DUPLICATE); wrong type fails; handle still works.
 *   MAP_INTO: CPtr maps into the child; re-mapping the same VA → BUSY proves
 *             PTEs were really installed; READ-only slot + writable flags →
 *             ACCESS_DENIED; empty slot / wrong type fail.  (Handle-path
 *             MAP_INTO stays covered by T076.) */

/* 19/20 before: 19 became IRIS_CPTR_OWN_TCB (D-6) and the exclusive mint into
 * an occupied slot fails, which is how this was found.  21/22 was the next
 * guess and it collides with T081 — the low range is full, so these live in
 * the S1 scratch window (64..87) at two slots nothing else names. */
#define T080_SLOT_RWD   68L               /* dynamic slot: frame, READ|WRITE|DUP */
#define T080_SLOT_RO    69L               /* dynamic slot: frame, READ only      */
/* Destination slots in the spawned child's root CNode.  lifecycle_probe
 * children only receive the LP_CPTR_CMD_EP=3 mint, so 60..62 are empty —
 * the same window T097/T098 use. */
#define T080_DST_SLOT   60L
#define T080_DST_SLOT2  61L
#define T080_VMO_SIZE   8192U             /* 2 pages: distinct from other tests */

/* ── T081: lifecycle syscalls by CPtr (A1 Increment 2a) ────────────────────
 * The capability argument of the lifecycle syscalls resolves through CSpace.
 * Spawn a lifecycle_probe child and mint capabilities to it into our own
 * CSpace at two rights levels — slot 21 (READ|WRITE|MANAGE|DUPLICATE) and slot
 * 22 (READ only) — then drive the whole lifecycle through them: alive, WATCH,
 * EXIT_CODE (alive → WOULD_BLOCK), FAULT_INFO (no fault → WOULD_BLOCK), kill,
 * watch fires, dead, EXIT_CODE (dead → code), kill again (idempotent 0).
 * Authority is not relaxed: killing through the READ-only slot →
 * ACCESS_DENIED, and minting INTO the child via a slot without RIGHT_WRITE →
 * ACCESS_DENIED.  Failure paths: empty slot and wrong-type slot fail cleanly.
 *
 * Stage 7 Step 13: the object the lifecycle half of this hangs off is the
 * THREAD.  Killing is SYS_TCB_EXIT and liveness is SYS_TCB_GET_INFO's state,
 * so the two rights levels are minted from the child's thread; the CSpace half
 * (minting INTO the child) still goes through the child's ROOT CNODE, because
 * that is a different object and a different authority.  What used to make
 * this one test — "the process capability answers all of it" — is precisely
 * what Stage 7 took apart. */

#define T081_SLOT_PROC  21L               /* child TCB: READ|WRITE|MANAGE|DUP  */
#define T081_SLOT_RO    22L               /* child TCB: READ only              */

/* ── T082: Process target by CPtr for VMO/handle operations (A1 Inc 2a) ─────
 * The destination-process argument of SYS_FRAME_MAP and
 * SYS_PROC_CSPACE_MINT resolves through the dual resolver, so a fully
 * CPtr-based delegation works: VMO by CPtr (slot 23) + process by CPtr
 * (slot 24).  Re-mapping the same VA → BUSY proves the PTEs were really
 * installed.  Authority is not relaxed: the self-proc cap (slot 25, WRITE
 * only — no MANAGE) is rejected as MAP_INTO/SHARE target with ACCESS_DENIED;
 * a wrong-type slot and an empty slot fail cleanly; and the pure handle path
 * (both args as handles) still works. */

#define T082_SLOT_VMO   23L               /* VMO: READ|WRITE|DUPLICATE  */
#define T082_SLOT_PROC  24L               /* child2 proc: full authority */
#define T082_MAP_VA2    (LP_MAP_VA + 0x10000ULL)

/* ── T083: TCB and SchedContext by CPtr (A1 Increment 2b) ───────────────────
 * The TCB / SchedContext syscalls now resolve their cap through the dual
 * resolver.  A helper thread publishes its own TCB handle (SYS_TCB_SELF) and
 * spins a progress counter; we mint that TCB into own slots 32 (READ|WRITE|
 * DUP) and 33 (READ only) and drive it by CPtr: GET_INFO (task_id real,
 * priority round-trip), SET_PRIORITY, SUSPEND (counter freezes), RESUME
 * (counter advances), and finally TCB_EXIT on the helper — non-self, so it is
 * safe to run (self TCB_EXIT would tear down the harness thread; the self
 * branch shares the same resolver + rights code, so runtime-testing it adds
 * nothing).  A SchedContext is minted into slots 34 (rw) / 35 (ro):
 * SC_CONFIGURE by CPtr, THREAD_SET_SC binds the CALLING thread to the SC by
 * CPtr and 0 unbinds (THREAD_SET_SC takes no TCB argument — it always
 * operates on the caller).  Authority is not relaxed: WRITE ops through the
 * READ-only slots → ACCESS_DENIED (THREAD_SET_SC has no rights check today;
 * unchanged).  Wrong type → INVALID_ARG (this family's historical code, kept
 * by the migration); empty slot fails cleanly.  Slots 32..39: the 16..29
 * dynamic pool is exhausted by T079-T082 (slots are mint-once). */

#define T083_SLOT_TCB     32L             /* helper TCB: READ|WRITE|DUP */
#define T083_SLOT_TCB_RO  33L             /* helper TCB: READ only      */
#define T083_SLOT_SC      34L             /* SchedContext: READ|WRITE|DUP */
#define T083_SLOT_SC_RO   35L             /* SchedContext: READ only    */

/* ── T084: IPC receive-slot — basic endpoint cap delivery (A1.5) ────────────
 * A receiver declares an empty own-CSpace slot in `msg.recv_slot` (A-33: its
 * own argument register, where it used to share a field with two other
 * meanings): a capability the sender attaches lands IN THAT SLOT and the
 * receiver reads the CPtr back in `msg.got_cap`.  A sender thread EP_SENDs the same endpoint cap twice (WRITE,
 * TRANSFER-consumed dups): recv #1 declares slot 36 → attached_handle == 36
 * and EP_NB_SEND by that CPtr resolves (WOULD_BLOCK = resolution + rights
 * OK, no receiver on that ep); recv #2 declares nothing → legacy handle
 * >= 1024, exactly as before A1.5. */

#define T084_SLOT  36u

/* ── T085: receive-slot rights reduction (A1.5) ─────────────────────────────
 * The slot receives rights_reduce(sender_rights, requested) — never more.
 * A notification cap (R|W|WAIT|DUP|TRANSFER at creation) is transferred
 * WRITE-only into slot 37: NOTIFY_SIGNAL by CPtr works and the ORIGINAL
 * handle observes the signalled bits (same kernel object — the invocation
 * is real), while NOTIFY_WAIT by CPtr fails ACCESS_DENIED (RIGHT_WAIT was
 * reduced away). */

#define T085_SLOT  37u

/* ── T086: receive-slot occupied / invalid — fail-fast atomicity (A1.5) ─────
 * A bad declaration must fail BEFORE the endpoint is touched: the queued
 * sender stays blocked with its staged cap intact and nothing is consumed.
 * With a sender queued (attached notification cap):
 *   - declaring slot 36 (occupied since T084) → ALREADY_EXISTS (the
 *     canonical occupied-slot error, same as SYS_PROC_CSPACE_MINT);
 *   - declaring slot 300 (< 1024 but beyond the 256-slot root CNode) →
 *     INVALID_ARG;  EP_NB_RECV validates identically;
 *   - sender still blocked after both failures (result flag untouched);
 *   - a good declaration (slot 42) then receives the SAME cap intact and
 *     invokes it by CPtr — the failed attempts consumed nothing. */

#define T086_SLOT  42u

/* ── T087: EP_CALL receive-slot keeps the reply cap ephemeral (A1.5) ────────
 * Both call-direction transfers use receive-slots while the KReply stays a
 * one-shot HANDLE:
 *   - the server's EP_RECV declares slot 38 → the cap the caller attaches
 *     via EP_CALL attached_cap lands there (CPtr);
 *   - the caller's EP_CALL declares slot 39 in attached_handle (previously
 *     a forced-zero field; >= 1024 still fails INVALID_ARG) → the cap the
 *     REPLY transfers lands there;
 *   - the reply cap itself is delivered to the server as a handle >= 1024
 *     (asserted) and keeps the T074 one-shot contract: first SYS_REPLY ok,
 *     second → NOT_FOUND.  receive-slot never converts a reply cap. */

#define T087_SRV_SLOT    38u
#define T087_REPLY_SLOT  39u

/* ── T088: death cleanup with receive-slot (A1.5) ───────────────────────────
 * A declared receive-slot must leave NO trace when the receive never
 * completes:
 *   A. receiver thread killed while blocked with slot 40 declared → slot 40
 *      has no ghost cap (SYS_CSPACE_RESOLVE fails), the endpoint is clean
 *      (EP_NB_SEND → WOULD_BLOCK, the T077 probe);
 *   B. the SAME slot 40 then serves a real transfer: a fresh receiver
 *      declares it and blocks FIRST, the sender delivers from its own
 *      context (send-side routed path) → cap lands as CPtr 40, invocable —
 *      no partially-installed state survived sub-case A;
 *   C. endpoint closed while a receiver waits with slot 41 declared → the
 *      receiver wakes with CLOSED and slot 41 stays empty. */

#define T088_SLOT_A  40u
#define T088_SLOT_C  41u

/* ── T090: LOOKUP into a client reply receive-slot (A1.6) ───────────────────
 * The looked-up cap lands in the CLIENT's CSpace as a CPtr; an occupied
 * reply-slot fails fast (ALREADY_EXISTS, endpoint untouched) and legacy
 * lookup keeps working after it; a failed lookup (NOT_FOUND) with a declared
 * slot leaves the slot empty. */
#define T090_SLOT   48u
#define T090_SLOT_B 49u

/* ── T091: vfs.ep session by CPtr receive-slot (A1.6) ───────────────────────
 * The real in-tree client flow init now uses at boot: look up "vfs.ep" into
 * a declared reply-slot and drive a REAL VFS operation through the CPtr. */
#define T091_SLOT 50u

/* Extended-word indices (see syscall_diag.c layout). */
#define IT_SI_LIVE      0u
#define IT_SI_HWM       1u
#define IT_SI_INSERTS   2u
#define IT_SI_REMOVES   3u
#define IT_SI_GHWM      4u
#define IT_SI_MAX       5u
#define IT_SI_SLOTDEL   6u
#define IT_SI_HANDDEL   7u
#define IT_SI_TOCTOU    8u
#define IT_SI_REPLY     9u
#define IT_SI_RESOLVE  10u
#define IT_SI_PROCLIVE 11u   /* Phase 16: KProcess objects live */
#define IT_SI_REAPHWM  12u   /* Phase 16: deferred-reap queue depth hwm */

/* Phase 17 ext2 scheduler-hardening words (offsets 96..108, 4 uint32).  A
 * pre-Phase-17 kernel clamps SYS_SCHED_INFO to 96 bytes and leaves these zero —
 * they are additive and never required by a legacy assert. */
#define IT_S2_RQHWM   0u   /* run-queue depth high-water */
#define IT_S2_DUPENQ  1u   /* duplicate-enqueue guard trips (invariant S4) */
#define IT_S2_SCLIVE  2u   /* live KSchedContext objects (invariants S8/S9) */
#define IT_S2_YIELD   3u   /* task_yield() entries (monotonic progress) */

/* Phase 18 ext3 authority words (offsets 112..128, 5 uint32 live per-type
 * counts).  A pre-Phase-18 kernel clamps SYS_SCHED_INFO to 112 bytes and leaves
 * these zero — additive, never required by a legacy assert. */
#define IT_S3_UNTYPED 0u   /* KUntyped objects live */
#define IT_S3_FRAME   1u   /* KFrame objects live   */
#define IT_S3_EP      2u   /* KEndpoint objects live */
#define IT_S3_NOTIF   3u   /* KNotification objects live */
#define IT_S3_CNODE   4u   /* KCNode objects live   */

/* KOBJ type codes for SYS_UNTYPED_RETYPE — must match the kobject_type_t order
 * in kernel/new_core/include/iris/nc/kobject.h (that enum is __KERNEL__-only). */
#define IT_KOBJ_NOTIFICATION   2u
#define IT_KOBJ_ENDPOINT       8u
#define IT_KOBJ_CNODE          9u
#define IT_KOBJ_SCHED_CONTEXT 10u
#define IT_KOBJ_UNTYPED       11u
#define IT_KOBJ_FRAME         15u
#define IT_KOBJ_VSPACE        14u

/* Phase 19 ext4 VM/VSpace words (offsets 136..152, 5 uint32). */
#define IT_S4_VSLIVE  0u   /* KVSpace objects live */
#define IT_S4_MAPLIVE 1u   /* KFrameMapping nodes live */
#define IT_S4_MAPOK   2u   /* successful maps (cumulative) */
#define IT_S4_UNMAPOK 3u   /* explicit unmaps (cumulative) */
#define IT_S4_TLB     4u   /* local invlpg count (cumulative) */

/* Phase 20 ext5 fault-model words (offsets 160..176, 5 uint32).  A pre-Phase-20
 * kernel clamps SYS_SCHED_INFO to 160 bytes and leaves these zero — additive,
 * never required by a legacy assert. */
#define IT_S5_DELIVER 0u   /* faults handed to a registered handler (cumulative) */
#define IT_S5_NOHAND  1u   /* faults with no handler → task killed (cumulative)  */
#define IT_S5_RESUME  2u   /* SYS_EXCEPTION_RESUME action 0 (cumulative)         */
#define IT_S5_KILL    3u   /* SYS_EXCEPTION_RESUME action 1 (cumulative)         */
#define IT_S5_CLEAN   4u   /* pending-fault records cleared (cumulative)         */

/* ── Stage 5 Step 4: a thread born from an Untyped ────────────────────────
 *
 * it_thread_create replaces SYS_THREAD_CREATE, which carved a thread out of
 * the kernel's static task pool: no capability was involved, no Untyped paid
 * for the storage, and the resulting thread existed because the kernel had a
 * free slot rather than because the caller held the authority and the memory.
 *
 * The sequence is the seL4 one, and every step names a capability:
 *   RETYPE2(KOBJ_TCB)     — storage carved from the suite's own Untyped;
 *   SYS_CSPACE_SELF       — a capability to the CSpace the thread will use;
 *   SYS_TCB_CONFIGURE     — CSpace + VSpace, given as capabilities;
 *   SYS_TCB_WRITE_REGS    — where it starts;
 *   SYS_TCB_RESUME        — and only then does it run.
 *
 * Returns the TCB CPtr (in a leaf of the suite's second-level CNode), or a
 * negative error.  The thread's storage returns to the Untyped when the last
 * capability to it goes and its execution has ended — the rotating leaf pool
 * does that on reuse.
 */

/* Leaf 255 of the object CNode: OUTSIDE the rotating pool (leaves 1..200), so
 * the capability the suite holds to its own CSpace is not recycled out from
 * under it a hundred fabrications later — which is precisely what happened
 * during bring-up, and showed up as thread creation failing with NOT_FOUND in
 * the second half of the run. */
#define IT_CSPACE_LEAF 255u

#define T133_VA     0x8070000000ULL
#define T134_VA     0x8071000000ULL
#define T135_VA_X   0x8072000000ULL
#define T135_VA_Y   0x8073000000ULL
#define T137_VA     0x8074000000ULL
#define T138_VA     0x8075000000ULL
#define T139_VA_BASE 0x8078000000ULL
#define IT_MAP_W    1ULL   /* SYS_FRAME_MAP flags: bit0 = WRITABLE */

/*
 * The frames come from a DEDICATED sub-untyped and land in DEDICATED slots,
 * not the suite's rotating object pool.
 *
 * The pool rotates by DELETING the slot it is about to reuse, and several
 * tests REVOKE the untyped everything else is carved from.  Either would take
 * a live IPC buffer's capability away from a running thread — the kernel keeps
 * its own reference so the object survives, but a buffer whose capability has
 * been revoked is a buffer nobody can account for, and the first version of
 * this panicked the kernel on an active-reference underflow within three tests.
 *
 * A pool of its own is the right shape regardless: an IPC buffer outlives
 * every operation in a test, so borrowing a slot that rotates is borrowing
 * something with a shorter life than the thing put in it.
 */
/* A CNode of its own too, for the same reason the untyped is: the object CNode
 * hands out leaves from a rotating pool and names two dozen fixed ones, and a
 * capability that must outlive every test in the suite does not belong in
 * either.  Root slot 66 is free; leaves 0..127 inside it are all ours. */
#define IT_IPCBUF_CNODE_SLOT 66u
#define IT_IPCBUF_CPTR(leaf) ((uint32_t)(((leaf) << 8) | IT_IPCBUF_CNODE_SLOT))
#define IT_IPCBUF_MAX        96u

/* ── T094: receive-slot TOCTOU degradation is RETIRED (Phase S4, Step 2) ────
 * A receiver declares slot 51 and blocks; before the sender delivers, the
 * process fills slot 51 itself (self-mint via the own-process cap).
 *
 * Until Step 2 this took a DOCUMENTED fallback: the cap was materialized as
 * a handle >= 1024.  That was the last CPtr→handle degradation in the kernel
 * (charter §3.7, the single tolerated exception) and it is now GONE: the
 * delivery FAILS CLOSED.  The message still arrives, carrying NO capability;
 * the slot keeps exactly the cap that won the race; and — because nothing was
 * delivered — the sender's SOURCE slot is not consumed, so no authority is
 * created or destroyed by the race.  This test is the retirement guard:
 * iris_ipc_stat_toctou_fallbacks must stay at a structural 0. */
#define T094_SLOT 51L

/* ── A1.8: legacy handle producer cleanup (T097–T098) ───────────────────────
 * SYS_HANDLE_TRANSFER, SYS_HANDLE_INSERT and SYS_VMO_SHARE are all retired
 * (NOT_SUPPORTED).  The ONLY cross-process placement is SYS_PROC_CSPACE_MINT
 * into a destination CSpace slot.
 * Destination child slots 60..62 (lifecycle_probe children only receive the
 * LP_CPTR_CMD_EP=3 mint, so these are guaranteed empty). */

#define T097_DST_SLOT   60L
#define T097_DST_SLOT2  61L
#define T097_DST_SLOT3  62L

/* ── A1.9: multi-child receive-slot stress (T099–T102) ──────────────────────
 * Receive-slots across REAL process boundaries: lifecycle_probe children
 * declare parent-chosen slots (LP_CMD_RSLOT_RECV mode), receive transferred
 * caps in their own CSpace, invoke them by CPtr, and report the landing
 * discriminator via their exit code.  Child-side slot: 40 (children only
 * receive the LP_CPTR_CMD_EP=3 mint).  Parent-side lookup slots: 43..47. */

#define LP_CMD_RSLOT_RECV      0x1099u   /* must match lifecycle_probe */
#define LP_EXIT_RECV_ERR_BASE  0x0B00L
#define T099_CHILD_SLOT        40u

/* Phase 16: send a bare command label to a child (no payload). */
#define LP_CMD_SEND_BLOCK  0x109Au   /* must match lifecycle_probe */
#define LP_CMD_CALL_BLOCK  0x109Bu   /* must match lifecycle_probe */

/* Monotonic fresh-slot allocator (slots are never deleted — see header). */
#define FZ_SLOT_BASE  100u
#define FZ_SLOT_LIMIT 240u

/* ── Command-driven persistent workers ──────────────────────────────────────
 * One worker thread per index, driven over its own control endpoint.  A
 * command is one blocking EP_SEND on the ctl ep (rendezvous == the worker is
 * ready); the worker runs the op against g_fz_data_ep, publishes results in
 * its result slots and re-blocks on ctl.  Workers are started/stopped per
 * test; each leaves the process KTcb handle behind by design (Ph96) — the
 * per-test live delta documents it. */
#define FZ_OP_EXIT       0u
#define FZ_OP_RECV       1u   /* w1 = receive-slot declaration (0 = legacy) */
#define FZ_OP_SEND_CAP   2u   /* w1 = handle (0 = none), w2 = rights, w3 = label */
#define FZ_OP_CALL       3u   /* w1 = attached_cap (0 = none), w2 = rights, w3 = reply slot */

/* ── T107: randomized receive-slot IPC stress ───────────────────────────────
 * 48 PRNG-driven iterations over one endpoint and one worker mixing
 * EP_SEND / EP_NB_SEND / EP_RECV with notification AND endpoint caps into
 * fresh slots, occupied slots, invalid slots, legacy slot-0 and rights-
 * degraded staging.  Invariants: I1-I8, I11, I12, I16-I18. */
#define T107_SEED  0xA1110107u
#define T107_ITERS 48u

/* ── T108: randomized close/cancel staged-cap stress ────────────────────────
 * 16 PRNG-driven rounds; each round creates a FRESH endpoint, parks one or
 * two workers on it (blocking send with staged cap, EP_CALL with staged cap,
 * declared-slot recv, legacy recv) and closes the endpoint mid-flight.
 * Every waiter must wake with CLOSED, every staged source cap must survive
 * with its owner (release exactly once — a double release would corrupt the
 * books), a canceled declared slot must stay empty and reusable, and no
 * KReply may ever be minted (no call rendezvouses).  Thread/process death
 * cancellation is T101/T111 territory — this test owns endpoint close.
 * Invariants: I5-I8, I14, I16, I17 (+ I4 via the reused empty slot). */
#define T108_SEED   0xA1110108u
#define T108_ROUNDS 16u

/* ── T109: randomized reply one-shot + attached cap stress ──────────────────
 * 20 PRNG-driven EP_CALLs from one worker; the main thread serves each one
 * and ALWAYS attempts a second reply.  Mix: plain reply + second reply WITH
 * a cap (the A1.10 T105 case), cap reply into a declared fresh slot, cap
 * reply into legacy slot 0, and a call declaring an OCCUPIED reply slot
 * (fail-fast before any send — the server never even sees a message).
 * The reply-caps counter must balance EXACTLY: one KReply per rendezvous,
 * none for the fail-fast rounds.  Caller-death/close during a call is
 * T108/T111 territory.  Invariants: I3-I7, I9, I10, I12, I16-I18. */
#define T109_SEED  0xA1110109u
#define T109_ITERS 20u

/* ── T110: svcmgr register/lookup receive-slot stress ───────────────────────
 * 18 PRNG-driven rounds over THREE temporary service names whose expected
 * registration state is tracked exactly: register / re-register (BUSY, the
 * rejected cap closed by svcmgr) / slot lookup (fresh slot, invocable CPtr)
 * / legacy lookup (handle >= 1024) / NOT_FOUND lookup with a declared slot
 * (the SAME reusable slot every time — it must stay empty) / occupied-slot
 * lookup (fail-fast, then legacy still works) / unregister + full
 * re-register cycle (svcmgr's CSpace pool frees and reuses, no ghost from
 * the previous generation).  The reply-caps counter balances EXACTLY: one
 * KReply per svcmgr rendezvous PLUS one per served lookup — svcmgr_log()
 * emits one console_ep_write EP_CALL per LOOKUP_NAME (OK and NOT_FOUND;
 * REGISTER/UNREGISTER do not log) — and none for the fail-fast occupied
 * rounds, which never reach svcmgr at all.
 * Invariants: I1, I3, I4, I11-I13, I16-I18. */
#define T110_SEED  0xA1110110u
#define T110_ITERS 18u

/* ── T111: cross-process receive-slot fuzz with lifecycle_probe ─────────────
 * Six lifecycle_probe children, one scenario each: a coverage-forced prefix
 * (kinds 0-3, every class runs exactly once regardless of the seed) plus a
 * PRNG-chosen tail — deterministic AND fully covered.  Kinds:
 *   0 rslot+notification : cap lands in the CHILD's CSpace at the declared
 *     CPtr (exit code == slot), child invokes it ACROSS the process
 *     boundary (signal observed by the parent);
 *   1 rslot+endpoint cap : same landing proof with an endpoint cap; the
 *     child's death then releases its CSpace ref — the parent's endpoint
 *     stays fully usable;
 *   2 kill before delivery: child killed while blocked with a declared
 *     slot — no dead waiter remains (NB probe), the sender's cap survives
 *     the attempted delivery;
 *   3 slot 0 (no destination): Stage 4 retired handle materialisation, so
 *     the child receives the message WITHOUT the capability, exits 0, and no
 *     cross-boundary signal can occur — the parent proves that by timing out
 *     on the notification instead of blocking on it.
 * Parent books balance EXACTLY (no thread helpers here — delta 0).
 * Invariants: I1, I5, I6, I11, I12, I15-I18. */
#define T111_SEED 0xA1110111u
#define T111_TAIL 2u

/* ── T112: spawn/exit churn — deferred-reap slot-reuse regression ───────────
 * 24 back-to-back spawn → natural-exit → IMMEDIATE respawn cycles.  A task
 * that exits by itself cannot reap its own address space (it is still
 * running on it), so it parks TASK_DEAD in the deferred reap queue; the
 * immediate respawn races the reaper for that task slot.  Before the A1.11
 * fix the slot allocator treated dead-but-unreaped slots as free: the reuse
 * wiped t->process, the reaper's TASK_DEAD guard then skipped the stale
 * entry silently, and every lost race leaked the child KProcess + address
 * space (~20 pages) — this loop, plus the earlier suite spawns, reliably
 * drove SYS_PROCESS_CREATE into NO_MEMORY.  Locks: spawn never fails under
 * churn, exit codes intact, parent books balance, high-water bounded.
 * Invariants: I15, I16, I17. */
#define T112_CYCLES 24u

/* ── T114: reap-queue pressure and slot reuse ───────────────────────────────
 * Keeps four children live at once and churns 40 replacements, alternating
 * self-exit and external kill, each replacement respawning IMMEDIATELY into
 * the just-freed task slot (the A1.11 race, now sustained and concurrent).
 * Locks: spawn never returns NO_MEMORY, natural-exit codes stay intact, and
 * at the end task-live / process-live return to baseline with the deferred
 * reap queue never approaching its size bound.  Invariants: I15-I17. */
#define T114_LIVE   4u
#define T114_CHURN 40u

/* ── T116: process death with live CSpace caps and a shared VMO ─────────────
 * A child is handed live authority — an endpoint and a notification minted
 * into its CSpace, plus a VMO cap shared into its handle table — and then
 * killed.  The child's teardown must release its refs WITHOUT destroying the
 * shared objects: the parent's endpoint/notification/VMO stay fully usable,
 * and handle/process books return to baseline (the child's CSpace root CNode,
 * KTcb and address space are all reaped).  mapped_count is not observable
 * from ring 3 (documented gap); the observable is object survival + exact
 * book balance.  Invariants: I1, I15, I16. */
#define T116_EP_SLOT 40u
#define T116_N_SLOT  41u
#define T116_VMO_SLOT 42u

/* ── T118: scheduler live count under lifecycle churn ───────────────────────
 * Interleaves process self-exit, process external-kill and in-process thread
 * self-exit, then verifies the scheduler's live TASK count and the live
 * PROCESS count both return exactly to baseline — no zombie counted alive, no
 * double-decrement, no task pending reap left occupying a slot — with the
 * deferred reap queue staying well within its bound.  (Each thread leaves a
 * documented +1 KTcb HANDLE by design — Ph96 — so handle-live is not asserted
 * flat here; task-live and process-live are the invariants.)
 * Invariants: I15, I16, I17. */
#define T118_ROUNDS 10u

/* ── Phase 17: scheduler / Scheduling-Context hardening (T119–T124) ──────────
 *
 * These tests harden the scheduler as the microkernel's source of truth about
 * which task is alive, runnable, blocked, dead or pending-reap.  They lean on
 * the Phase 17 additive instrumentation exposed by SYS_SCHED_INFO's ext2 tier
 * (it_sched_ext2): run-queue high-water, the duplicate-enqueue guard counter
 * (invariant S4), the live KSchedContext count (S8/S9) and the monotonic
 * task_yield() counter (progress / no-lost-task).  Combined with the existing
 * task-live (it_task_live), process-live (IT_SI_PROCLIVE) and reap-hwm
 * (IT_SI_REAPHWM) words, they lock the scheduler invariants S1–S16 documented
 * in docs/architecture/scheduler-hardening.md.
 *
 * In-process worker threads are retyped TCBs (Stage 5 Step 4), created with
 * task id (not a handle) and leaves one KTcb HANDLE in this process's table by
 * design (Ph96, exactly as T118 notes).  So these tests assert TASK-live and
 * PROCESS-live return to baseline, never handle-live — the KTcb handle id is
 * never surfaced to ring 3 and cannot be closed.  Thread counts are budgeted to
 * stay far below HANDLE_TABLE_MAX. */

#define SH_NWORK 4u
/* Local mirror of kernel TASK_MAX (256) for plausibility bounds — iris_test
 * does not include <iris/task.h>.  Used only as an upper sanity bound. */
#define TASK_MAX_HINT 256u

#define SH_MODE_YIELD_BLOCK 0u  /* yield, block on ep recv, yield, exit  (T119) */
#define SH_MODE_CHURN       1u  /* iters yields, block on ep recv, exit  (T120) */
#define SH_MODE_SPIN        2u  /* iters yields, exit                    (T122) */
#define SH_MODE_SC          3u  /* bind g_sh_sc, yield, exit             (T123) */

/* ── T119: task state transition stress ─────────────────────────────────────
 * Each round drives in-process worker threads through the full runnable →
 * blocked (EP_RECV) → runnable (rendezvous wakeup) → dead (self-exit) cycle,
 * then interleaves a lifecycle_probe child that is externally KILLED (a
 * runnable/blocked task torn down from outside).  After the churn every
 * scheduler book returns to baseline: no zombie counted alive, no dead task
 * left occupying a slot, the deferred reaper drained, no KSchedContext leaked,
 * and the yield counter advanced (tasks actually reached the scheduler).
 * Invariants: S1, S2, S3, S5, S6, S7, S15. */
#define T119_ROUNDS 4u

/* ── T120: run-queue churn and duplicate-enqueue stress ─────────────────────
 * SH_NWORK workers each run a FIXED-length yield loop (deterministic: exactly
 * T120_ITERS iterations), so all of them are concurrently runnable and the
 * O(1) priority run queue is churned hard.  Every worker then blocks on a
 * shared endpoint (proving it reached the end of its loop) before being
 * released to exit.  Verifies:
 *   - no lost runnable task: every worker's progress counter is EXACTLY
 *     T120_ITERS (S12 — yield never drops a runnable task; a corrupted queue
 *     would strand a worker and time out the wait);
 *   - no dead worker advances: progress is frozen at T120_ITERS, never above;
 *   - the run-queue depth high-water is plausible (≥2 concurrent, ≤ TASK_MAX);
 *   - the duplicate-enqueue guard (S4) engaged only a bounded number of times
 *     (pure-yield churn creates no wakeup races, so the delta stays tiny);
 *   - task-live returns to baseline after reap.
 * Invariants: S4, S6, S12. */
#define T120_ITERS 80u

/* ── T122: yield / quantum / preemption accounting ──────────────────────────
 * SH_NWORK equal-priority cooperative workers each yield exactly T122_ITERS
 * times.  With no blocking, fairness under the round-robin-within-priority run
 * queue means every worker must complete its full quota — there is no
 * starvation in this workload.  This test documents, honestly, what the
 * scheduler guarantees TODAY:
 *   - it is cooperative-first: a task advances by calling task_yield (or by
 *     consuming its TASK_DEFAULT_SLICE quantum, after which scheduler_tick sets
 *     need_resched);
 *   - tick-driven preemption exists (priority + quantum) but under the QEMU TCG
 *     headless target no timer IRQs are delivered while a task spins in ring 0,
 *     so forward progress here is carried by explicit yields — which is exactly
 *     what this test measures;
 *   - what it does NOT yet guarantee: strict fairness weights, per-task CPU
 *     accounting beyond the SchedContext budget, or preemption of a ring-0
 *     spinner (see docs/architecture/scheduler-hardening.md "Limits").
 * Asserts: every worker completed T122_ITERS (no starvation), the global yield
 * counter advanced by ≥ SH_NWORK*T122_ITERS, and context switches advanced.
 * Invariants: S5, S12. */
#define T122_ITERS 60u

/* ── Phase 18: untyped / retype / revoke authority hardening (T125–T131) ──────
 *
 * These tests exercise the memory-authority surface — SYS_UNTYPED_RETYPE2,
 * SYS_UNTYPED_RESET, SYS_CAP_DERIVE, SYS_CAP_REVOKE — end to end from ring 3.
 * They lean on one boot KUntyped forwarded down the boot chain (userboot → init
 * → iris_test) into IRIS_CPTR_TEST_UNTYPED, plus the Phase 18 additive
 * instrumentation (it_sched_ext3: live per-type object counts) and the existing
 * handle-live word.
 *
 * Two independent, strong "authority died" observables anchor these tests:
 *   1. the live per-type object count (it_sched_ext3) returns to baseline —
 *      the object was destroyed, not leaked;
 *   2. SYS_UNTYPED_RESET succeeds — it is gated on child_count == 0, so success
 *      proves every object/sub-untyped carved from the region released its
 *      parent reference (U17/U18).
 *
 * The authority model (see docs/architecture/untyped-retype-revoke-hardening.md):
 *   - retype installs the new object as a fresh handle-table entry (a derivation
 *     ROOT), and tracks the untyped→child link via child_count for RESET gating;
 *   - SYS_CAP_REVOKE walks the handle-table derivation tree (SYS_CAP_DERIVE
 *     children), NOT CSpace CNode slots and NOT untyped child_count — its scope
 *     is documented and asserted here.
 */
/* Phase S1: the authority tests need a region whose child_count they fully
 * control (RESET is gated on child_count == 0).  The suite-wide creation
 * helpers (it_ep_create & friends) now also carve from the delegated untyped
 * at slot 55 and some fixtures live for the whole run, so these tests operate
 * on their OWN sub-untyped, carved lazily from slot 55 on first use. */
/* Leaf 244 of the second-level CNode, deliberately OUTSIDE the rotating
 * fabrication pool (leaves 1..200): this untyped lives for the whole run, and
 * a rotation that wrapped onto it would delete the region every other test
 * carves from. */
#define IT_AUTH_UT_CPTR IT_OBJ_CPTR(244u)
#define IT_UT (it_auth_ut())

/* ── T131: deterministic untyped/revoke stress ──────────────────────────────
 * A fixed-seed PRNG drives many rounds of retype / derive / mint / revoke /
 * delete / close against the shared untyped, mixing endpoints, notifications
 * and CNodes and interleaving forced failures (bad type, bad size, stale
 * revoke, occupied CNode slot).  After the churn the region must RESET clean
 * and every live per-type count and the handle-live count return to baseline.
 * Prints seed/iteration only on failure.
 * Invariants: U4, U5, U8, U9, U17, U18, U19. */
#define T131_SEED   0x18C0DE18u
#define T131_ROUNDS 24u

/* ── T132: self-VSpace authority from ring 3 ────────────────────────────────
 * The self-VSpace cap (SYS_VSPACE_SELF, minted into IRIS_CPTR_TEST_VSPACE)
 * grants exactly the authority to map into the caller's own address space:
 *   - a valid map through it succeeds;
 *   - a wrong-type cap (the untyped) in the VSpace slot is rejected WRONG_TYPE;
 *   - a VSpace cap lacking RIGHT_WRITE is rejected ACCESS_DENIED (no fallback);
 *   - there is no way to name another process's VSpace.
 * Invariants: V1, V3, V4, V20. */
#define IT_VS_RO 57L   /* read-only self-VSpace cap (missing-rights fixture) */

/* ── T136: VSpace cleanup on process death ──────────────────────────────────
 * A spawned child owns a VSpace with live bootstrap KFrame mappings (its text
 * and stack).  Killing it (external) and letting one self-exit must run VSpace
 * cleanup exactly once: the child's KVSpace is destroyed and every one of its
 * mappings is swept, so live-VSpace and live-mapping counts return to baseline
 * — with no interaction bug against the deferred reaper, and process/task
 * counters back to baseline.  Connects Phase 16 (death) + Phase 19 (VSpace).
 * Invariants: V15, V16, V17, V18. */
#define T136_ROUNDS 4u

/* ── T139: deterministic VSpace mapping stress ──────────────────────────────
 * A fixed-seed PRNG drives many rounds of retype/map/(derive+revoke)/unmap/
 * close over a reserved VA window, interleaving forced failures (unaligned VA,
 * occupied VA).  After the churn every VM counter and the object/handle books
 * return to baseline.  Prints seed/iteration only on failure.
 * Invariants: V9, V10, V13, V15, V17, V18. */
#define T139_SEED   0x19C0DE19u
#define T139_ROUNDS 40u


/* ── Phase 20: fault endpoint / exception delivery model (T140–T147) ──────────
 *
 * User faults are authority events: registering a fault endpoint requires
 * RIGHT_MANAGE on the target process; delivery records the fault in the
 * KProcess and signals the registered KNotification while the faulting task is
 * suspended in TASK_BLOCKED_FAULT; SYS_PROCESS_FAULT_INFO (RIGHT_READ) reads
 * the record; SYS_EXCEPTION_RESUME (RIGHT_MANAGE) resolves it — action 0
 * re-executes the faulting instruction, action 1 kills the task.  Kernel
 * faults are never deliverable (idt.c panics).  lifecycle_probe children are
 * the fault fixtures: FAULT_READ/WRITE take a target VA in words[0] (0 = the
 * child's own ASLR-biased code address), FAULT_EXEC calls into its NX stack.
 *
 * Observables: the ext5 SYS_SCHED_INFO tier (delivery/nohandler/resume/kill/
 * cleanup counters) plus the usual task/handle/object/mapping books. */

#define LP_CMD_FAULT_READ   0x109Cu  /* must match lifecycle_probe */
#define LP_CMD_FAULT_WRITE  0x109Du
#define LP_CMD_FAULT_EXEC   0x109Eu

#define T14X_BAD_VA   0x8090000000ULL          /* canonical user VA, never mapped */
#define T14X_KERN_VA  0xFFFF800000001000ULL    /* kernel half — user access faults */

/* #PF error-code bits (Intel SDM). */
#define PF_ERR_P 0x01u
#define PF_ERR_W 0x02u
#define PF_ERR_U 0x04u
#define PF_ERR_I 0x10u

struct it_fault {
    uint32_t vector, task_id, error;
    uint32_t seq;              /* Phase 25: fault generation (FAULT_OFF_SEQ) */
    uint64_t rip, cr2;
};

/*
 * Ledger A-22 — the record is what ARRIVED, and `leaf` says which fault.
 *
 * This used to be SYS_TCB_FAULT_INFO on the faulting thread: a handler that
 * had been signalled came back to the kernel with a capability to the thread
 * to find out what had happened to it, which is the only reason every fault
 * had to mint one into a mailbox.  The record now travels in the message, so
 * "read the fault" is reading what the receive already delivered.
 *
 * IRIS_ERR_WOULD_BLOCK when no fault is outstanding on that leaf — the same
 * answer the syscall gave, and the same thing it means: nothing to serve.
 */
#define IT_FAULT_LEAVES 16u

/* ── T147: deterministic fault endpoint stress ──────────────────────────────
 * A fixed-seed PRNG drives rounds of spawn → mixed fault kind (invalid-VA
 * read, own-text write, kernel-range read, NX exec) → mixed resolution
 * (resume-refault-kill / resume-kill / PROCESS_KILL / close-notif-then-kill),
 * with occasional non-faulting children (own-text read) interleaved.  Every
 * wait is bounded; two suspended-in-fault children coexist at one point every
 * round (the shared-IST regression fixture: both keep live frames while
 * blocked).  After the churn the fault counters and every book return to
 * baseline: no hung faulted tasks, no KReply/waiter drift, no live_task/proc
 * drift, no mapping drift, no stale fault state.  Prints seed/iteration only
 * on failure.  Invariants: F6, F11, F13, F15, F16, F17, F18, F19, F20. */
#define T147_SEED   0x20C0DE20u
#define T147_ROUNDS 10u

/* ── Phase 21: cross-syscall fuzzing / hostile argument surface (T148–T155) ───
 *
 * These tests do not exercise a feature — they subject the WHOLE syscall
 * surface to deterministic adversarial pressure and prove it fails clean:
 * unknown/retired numbers, wrong-type caps, stale/empty/boundary handles,
 * hostile user pointers, reduced rights, forced mid-syscall failures, and
 * mixed cross-family sequences.  The bar is not "no crash" — it is "every
 * hostile input fails clean with zero drift in any live count".
 *
 * The anchor is a full-surface snapshot: every live-object gauge the kernel
 * exposes (task, process, handle, untyped/frame/endpoint/notification/cnode,
 * VSpace, mapping) plus the KReply balance and the pending-fault flag.  A
 * hostile op that leaks a ref, plants a ghost slot/PTE, or strands a waiter
 * moves at least one of these, so equality-to-baseline after the churn is a
 * strong, cheap invariant.  Cumulative counters (map/unmap/TLB/fault-delivery)
 * are checked as directional deltas only where a test deliberately triggers
 * the event.
 *
 * Determinism: xorshift32 (fz_rand) seeded per test; failures print
 * `FZ <test> seed=<seed> iter=<i> op=<op>` and nothing on success.  Every
 * blocking interaction rendezvouses through a control endpoint or a bounded
 * NOTIFY_WAIT_TIMEOUT — no long sleeps.  Invariants X1–X24 live in
 * docs/architecture/syscall-fuzzing.md. */

/* Full-surface live snapshot. */
struct it_snap {
    uint32_t task;                 /* live scheduler tasks */
    uint32_t hlive, ghwm, hmax;    /* handle-table live / global hwm / max */
    uint32_t proclive, reply;      /* live processes / reply-caps-created (cumulative) */
    uint32_t ut, fr, ep, no, cn;   /* live untyped/frame/endpoint/notif/cnode */
    uint32_t vs, map;              /* live VSpace / mapping nodes */
    uint32_t fdeliver, fclean;     /* cumulative fault delivery / cleanup */
    uint8_t  ok;
};
#define IT_FZ_BAD_H_N ((int)(sizeof(it_fz_bad_handles) / sizeof(it_fz_bad_handles[0])))

/* ── T151: cross-family hostile sequence fuzz ───────────────────────────────
 * A seeded PRNG drives a mixed stream of operations across CSpace/cap,
 * untyped, frame/VSpace, notification, endpoint and fault families against a
 * tiny in-test model of what SHOULD be live.  Half the ops are deliberately
 * malformed (wrong slot, wrong type, bad rights, bogus VA).  A malformed op
 * must never alter the model; a well-formed one advances it.  After every
 * batch the full snapshot must equal the pre-batch baseline once the churn is
 * unwound.  Invariants: X2–X5, X8–X14, X22–X24. */
#define T151_SEED   0x21C0DE51u
#define T151_ROUNDS 24u

/* ── T153: blocking syscall cancellation fuzz ───────────────────────────────
 * A lifecycle_probe child is parked in each blocking primitive, then torn down
 * by every cancellation route, and the books must balance every time.  This
 * re-proves, under one roof and a seeded resolution mix, the cancellation
 * contracts that Phase 16/20 established: EP_RECV / EP_SEND / EP_CALL and the
 * fault-pending state all wake-or-die on process kill / endpoint close /
 * handler drop, leaving no dead waiter, no KReply, no live-count drift.
 * Invariants: X11, X14, X15, X16, X20, X21. */
#define T153_SEED   0x21C0DE53u
#define T153_ROUNDS 8u

/* ── T155: deterministic full syscall stress ────────────────────────────────
 * The grand finale: a seeded PRNG runs many rounds combining object create/
 * destroy, map/unmap, notify, endpoint rendezvous, cap derive/revoke, child
 * spawn/kill and a controlled fault, within fixed slot/process/mapping
 * budgets and an aggressive end-of-round cleanup.  Every gauge must return to
 * baseline each round; the run prints seed/iteration only on failure.  This is
 * the whole surface under sustained deterministic hostility.
 * Invariants: X9–X23 (full set). */
#define T155_SEED   0x21C0DE55u
#define T155_ROUNDS 16u

/* ── Phase 22: service authority minimization (T156–T163) ────────────────────
 *
 * The kernel mechanism fails clean under hostility (Phase 21); the next risk is
 * a service holding authority it does not need.  These tests lock the
 * least-authority contract: delivery carries exactly the declared caps and no
 * more (T156/T162), svcmgr is a registry that never amplifies rights for
 * ordinary clients (T157), the productive services expose no ambient authority
 * (T158/T159), init's handoff is auditable (T160), and service teardown leaves
 * no ghost registration (T161/T163).  Invariants A1–A16 live in
 * docs/architecture/service-authority-minimization.md.
 *
 * The anchor for delivery is a self-report from a lifecycle_probe child: it
 * resolves its well-known CPtr slots and exits with a bitmask, so the parent
 * asserts the child sees EXACTLY the caps it was minted — no phantom
 * authority, and removing a cap removes it from the child. */

#define LP_CMD_REPORT_SLOTS  0x10A0u   /* must match lifecycle_probe */

/* ── T163: deterministic service authority stress ───────────────────────────
 * A seeded PRNG drives register / lookup / unregister / endpoint-close churn
 * on the dynamic registry, interleaving malformed ops (unregister of a stale
 * id, lookup of a missing name, register of a reserved name).  No op amplifies
 * authority or leaves a ghost; the active-slot gauge and the full snapshot
 * return to baseline.  Invariants: A11, A12, A14, plus X-style no-drift.
 * Prints seed/iteration only on failure. */
#define T163_SEED   0x22C0DE63u
#define T163_ROUNDS 12u

/* ── Phase 23: device / driver isolation hardening (T164–T171) ───────────────
 *
 * The service authority is minimized (Phase 22); the next risk is a driver
 * reaching hardware it was never granted.  These tests prove containment: an
 * I/O-port cap bounds access to exactly its range (T164), a compromised
 * driver stand-in cannot escalate past its device caps (T165/T166), IRQ
 * route/ack require the matching cap and iris_test itself cannot route
 * (T167), framebuffer authority is isolated (T168), device caps derive
 * monotonically (T169), driver death releases device authority (T170), and a
 * seeded device fuzz never crosses a cap (T171).  Invariants D1–D20 live in
 * docs/architecture/device-driver-isolation.md.
 *
 * Safe test hardware: COM2 (0x2F8, whitelisted, unwired in headless QEMU) is
 * the dummy port; IRQ 5 (unused) is the dummy line.  iris_test's spawn cap
 * carries HW_ACCESS (it already mints the 0x3F8 serial cap at startup), so it
 * can create device caps to exercise the authority paths — but it holds NO
 * proc cap with RIGHT_ROUTE, so it cannot itself route an IRQ (a containment
 * property asserted directly in T167). */

#define LP_CMD_DEV_PROBE     0x10A1u   /* must match lifecycle_probe */
#define IT_COM2_BASE   0x2F8L
#define IT_COM2_COUNT  8L

/* ── T171: deterministic device authority fuzz ──────────────────────────────
 * A seeded PRNG drives a mix of ioport create (whitelisted / non-whitelisted),
 * in/out at valid and out-of-range offsets, derive/revoke, IRQ-cap create and
 * route/ack failure paths, and a compromised-driver probe.  No access crosses a
 * cap, no probe escalates (mask 0), and every gauge — including the IRQ-route
 * count — returns to baseline.  Prints seed/iteration only on failure.
 * Invariants: D1–D3, D5–D10, D14, D15, D17–D19. */
#define T171_SEED   0x23C0DE71u
#define T171_ROUNDS 14u
/* The narrowed authority the fuzz probes against: COM2 only, so every base in
 * `bad_bases` is outside it by construction. */
#define T171_NARROW_SLOT IT_IOCTL_NARROW

/* ── Phase 24: service restart / supervision model (T172–T180) ───────────────
 *
 * Drivers are contained (Phase 23); the next structural risk is what happens
 * when a service or driver DIES.  These tests lock the supervision contract:
 * every service has an explicit policy (T172), a restarted service comes back
 * with a new generation and only its declared authority (T173/T177), a stale
 * generation cannot touch the new instance (T174), a crash-loop stops at its
 * limit and leaves the service degraded (T175), a client blocked on a dying
 * service wakes with an error (T176), a critical service's loss is an explicit
 * documented state (T178), and death mid-register leaves no ghost (T179/T180).
 * Invariants R1–R24 live in docs/architecture/service-supervision-model.md.
 *
 * Two supervision surfaces are exercised: svcmgr's real catalog policy (via the
 * STATUS oracle, now carrying the explicit policy words) and iris_test acting
 * as a supervisor over lifecycle_probe children it owns — a supervisor is just
 * a process holding proc caps plus a policy, so this tests the kernel primitives
 * (watch, kill, reap, endpoint cleanup) every supervisor depends on, plus the
 * policy logic, without spending the real services' restart budgets. */

/* Supervision-policy classes (must match service_catalog.h). */
#define IT_SUP_CRITICAL_RESTART     1u
#define IT_SUP_OPTIONAL_RESTART     2u
#define IT_SUP_OPTIONAL_NO_RESTART  3u
#define IT_SUP_CRITICAL_NO_RESTART  4u

/* ── T175: crash-loop limit and degraded state ──────────────────────────────
 * iris_test supervises a dummy service (a probe it spawns and immediately kills,
 * modelling a service that dies right after start) under an explicit restart
 * limit.  The supervisor respawns exactly `limit` times, then STOPS and marks
 * the service degraded — no infinite loop, no leak per attempt, live counts back
 * to baseline.  This is the policy the real catalog applies (restart_count <
 * restart_limit), driven deterministically.  Invariants: R13, R14, R15, R19. */
#define T175_LIMIT 3u

/* ── T180: deterministic supervision stress ─────────────────────────────────
 * A seeded PRNG drives a mix of supervised-service operations against probe
 * children and the dynamic registry: spawn, register, lookup, kill, fault-
 * crash, restart (respawn a new generation), unregister, stale unregister,
 * endpoint close, and a driver-like instance with a device cap.  No stale
 * generation acts, no registry ghost survives, no client stays stuck, and every
 * gauge — registry slots and the full live snapshot — returns to baseline.
 * Prints seed/iteration only on failure.  Invariants: R2, R4, R9, R12, R18–R24. */
#define T180_SEED   0x24C0DE80u
#define T180_ROUNDS 12u

/* ── Phase 25: VM policy / user pager groundwork (T181–T190) ─────────────────
 *
 * Services are supervised (Phase 24); the next structural boundary is MEMORY
 * POLICY.  These tests lock the first capability-mediated user-pager model:
 * an authorized ring-3 component resolves page faults of a SPECIFIC process
 * with explicit, attenuable authority — and nothing more.
 *
 * The pager authority manifest (all minted pre-start by the supervisor):
 *   target process cap  READ|MANAGE  — fault info + resolution, this target
 *   target VSpace cap   WRITE        — map-into-target (SYS_PROCESS_VSPACE)
 *   frame cap           READ[/WRITE] — the one page it may install
 *   fault notification  WAIT         — the delivery wake-up
 *
 * Four Phase 25 additive kernel extensions are exercised here:
 *   SYS_PROCESS_VSPACE(107)  MANAGE holder → target VSpace cap;
 *   FAULT_OFF_SEQ            per-process fault generation in the record;
 *   EXCEPTION_RESUME 2/3     seq-checked resume/kill (stale → NOT_FOUND);
 *   FRAME_MAP/UNMAP VSpace   dual resolver (handle works; A1 migration
 *                            completed, radix-masking hazard closed).
 *
 * Invariants P1–P24 live in docs/architecture/user-pager-vm-policy.md.
 * Constants must match services/lifecycle_probe/main.c. */

#define LP_CMD_PAGER_SERVE  0x10A2u
#define LP_CMD_PAGER_XPROBE 0x10A3u
#define LP_PGR_SLOT_XPROC   8u
#define LP_PGR_SLOT_XVS     9u
#define LP_PGR_SLOT_TPROC   12u
#define LP_PGR_SLOT_TVS     13u
#define LP_PGR_SLOT_FRAME   14u
#define LP_PGR_SLOT_FAULT_EP 15u   /* A-22: the fault ENDPOINT the pager serves */
/* Stage 7 Step 7: the fault mailbox CNode (see lifecycle_probe/main.c). */
#define LP_PGR_SLOT_FAULTCN 17u
#define LP_EXIT_PGR_OK      0x0D00L

/* Fault VAs: canonical, page-aligned, inside the user-private window, clear
 * of every earlier suite VA (T13x self-maps end at 0x8078…, T14X_BAD_VA is
 * 0x8090…).  T25_SELF_VA is the parent's own scratch mapping for preparing /
 * inspecting frame contents. */
#define T25_VA_A    0x8092000000ULL
#define T25_VA_B    0x8093000000ULL
#define T25_VA_C    0x8094000000ULL
#define T25_VA_D    0x8095000000ULL
#define T25_SELF_VA 0x807A000000ULL
#define T25_PATTERN 0x5AA51234u
#define T25_WMARK   0xFA017E57u   /* the store LP_CMD_FAULT_WRITE retires */

/* A supervised pager target: command endpoint, proc cap, VSpace cap (via
 * SYS_PROCESS_VSPACE), fault-handler notification, exit watch. */
struct t25_tgt {
    handle_id_t cmd, proc, vs, notif, watch;
    /* Stage 7 Step 7: the mailbox leaf this target's faults deliver into.
     * Allocated per spawn out of the suite's own mailbox, so two targets
     * blocked in a fault at once (T183, T184) never overwrite each other's
     * capability — which is the whole reason a fault mailbox is per-target and
     * not per-supervisor. */
    uint32_t    fault_leaf;
};

/* ── T189: pager restart preserves least authority ──────────────────────────
 * The Phase 24 ↔ Phase 25 junction.  A crashing pager is supervised under an
 * explicit restart limit: two generations die before resolving and the
 * budget is spent — the pager service is DEGRADED, the loop STOPS (P17).
 * The fault meanwhile stays pending and resolvable.  A post-crash generation
 * spawned from the same declaration reports EXACTLY the declared manifest
 * (nothing accumulated across generations, no stale endpoint, no peer/device/
 * KDEBUG authority), and the serving generation completes the resolution.
 * Invariants: P15, P16, P17, P21, P22. */
#define T189_LIMIT 2u

/* ── T190: deterministic user pager stress ──────────────────────────────────
 * Seeded mixed-operation rounds over the whole Phase 25 surface.  Every round
 * holds TWO concurrent pending faults (the shared-IST regression stays
 * covered under pager traffic) and resolves them through a seed-chosen path:
 * external pager map+resume / kill, pager death + supervisor takeover,
 * target death mid-fault, refault + stale-generation rejection, unauthorized
 * caps denied, occupied-VA and RO-PTE enforcement.  After every round: no
 * pending fault, no zombie, and the live books (task/proc/handle/VSpace/
 * mapping) exactly at the pre-suite baseline.  Prints seed/round/op only on
 * failure.  Invariants: P9–P23 under load. */
#define T190_SEED   0x25AF9B31u
#define T190_ROUNDS 6u

/* ── Phase 26: granted-memory policy (T191–T200) ─────────────────────────────
 *
 * Phase 25 fixed the pager AUTHORITY contract; the page source was a raw frame.
 * Phase 26 made the source a first-class MEMORY OBJECT — a KVmo defended by
 * policy: logical range, size, offsets, rights, mappings, cleanup.
 *
 * Ledger D-5 took the object away again, and the reason is worth stating
 * because it is not that the policy was wrong.  Every rule the VMO enforced
 * was a real rule.  It enforced them over a REGION THE KERNEL OWNED, allocating
 * its pages lazily on a schedule the holder did not choose and range-checking
 * an offset into it — and a microkernel does not own memory on anybody's
 * behalf.  A client that wants page N mapped grants the FRAME for page N, and
 * then: which pages a pager may install is the set of capabilities it holds,
 * writability is RIGHT_WRITE on the page, revocation is deleting one slot, and
 * "offset past the end" is an empty slot.  Same rules, no object.
 *
 * So these tests keep their subjects and change their fixture.  The two that
 * were ABOUT the object — its size contract and its offset range check — are
 * rewritten to assert what replaced them (T191, T192); the eight that were
 * about paging survive with a run of one-page frames where the VMO was.
 *
 * Invariants M1–M30 live in docs/architecture/memory-object-vmo-policy.md.
 * Reuses the Phase 25 t25_* pager harness verbatim, and more of it than
 * before: subaction 4 mapped a VMO page at an offset, and with the offset gone
 * it IS subaction 1 — the supervisor mints the page it means the pager to
 * install, and the pager installs the page it holds. */

#define T26_TVA_A    0x8096000000ULL   /* target fault VAs (clear of T25 range) */
#define T26_TVA_B    0x8097000000ULL
#define T26_SELF_VA  0x807B000000ULL   /* parent scratch for page inspect */
#define T26_PAT0     0xA1B2C3D4u
#define T26_PAT1     0x11223344u
#define T26_WMARK    0xFA017E57u   /* == the probe's LP_CMD_FAULT_WRITE store */

/*
 * Ledger D-5 — a GRANT is a run of one-page frames, one capability per page.
 *
 * These tests used to hand a pager ONE capability to a four-page KVmo and tell
 * it, per fault, which offset to source the page from.  The kernel then owned
 * the region, allocated its pages on a schedule nobody chose, and range-checked
 * an offset — three jobs a microkernel does not have.
 *
 * A client that wants page N of its memory mapped grants the FRAME for page N.
 * Which pages a pager may install is then the set of capabilities it holds;
 * whether it may install one writable is RIGHT_WRITE on that frame; revoking
 * one page is deleting one capability.  The offset argument disappears because
 * the question it asked — which page — is answered by which capability.
 *
 * The run lives in the rotating object pool, T26_GRANT_PAGES consecutive
 * leaves, so T26_PAGE is leaf arithmetic on the CPtr.  Same pool contract as
 * every other fixture: delete before use, never hold across a test boundary.
 */
#define T26_GRANT_PAGES 4u
#define T26_PAGE(v, n)  ((handle_id_t)((uint64_t)(v) + ((uint64_t)(n) << 8)))
/* The byte offset a test names, as the page capability it names. */
#define T26_AT(v, off)  T26_PAGE((v), (uint32_t)((off) >> 12))

/* ── T200: deterministic memory-object stress ────────────────────────────────
 * Seeded mixed-operation rounds over the whole Phase 26 surface: create VMOs,
 * derive reduced caps, map at offsets, VMO-back a fault, resolve or refault,
 * kill target, kill/restart pager, revoke (close) with mapping live, unmap,
 * inject failures.  After every round: no pending fault, no zombie, VMO/frame/
 * mapping/handle/process books at baseline.  Prints seed/round/op on failure.
 * Invariants: M13–M29 under load. */
#define T200_SEED   0x26C0FFEEu
#define T200_ROUNDS 6u

/* ── Phase 27: Service Pager Integration (T201–T210) ──────────────────────────
 *
 * Phase 25/26 made the pager a MODEL and a page SOURCE.  Phase 27 makes it a
 * SERVICE: a distinct supervised image ("pager", initrd index 10 — NOT
 * iris_test, NOT lifecycle_probe), registered in svcmgr, driven request/reply
 * over a control endpoint, resolving faults strictly inside a capability
 * manifest of target + VMO grants.
 *
 * iris_test acts as the SUPERVISOR (the Phase 24 probe-supervisor model, now
 * over a real named service): it mints the manifest, spawns the service,
 * registers "pager.ep", drives it, watches it, and restarts it under an
 * explicit policy.  Constants below MUST match services/pager/pager_proto.h.
 *
 * Invariants G1–G30 live in docs/architecture/service-pager-integration.md. */

#define PGR_SLOT_CTRL_EP    LP_CPTR_CMD_EP   /* slot 3: the pager's control endpoint */
/* Phase 28.1 manifest layout (must match services/pager/pager_proto.h): ONE
 * shared fault notification at slot 5 for ALL targets (bit i = target i),
 * targets at 20 + i*2 (proc/vs only — the per-target notification column is
 * gone; that is what makes 16 concurrent targets cost ONE notification
 * against the supervisor's quota instead of 16). */
#define PGR_SLOT_FAULT_EP 5u
/* Stage 7 Step 7: the fault mailbox CNode (see services/pager/pager_proto.h). */
#define PGR_SLOT_FAULT_CN    14u
#define PGR_TGT_BASE        20u
#define PGR_TGT_STRIDE      2u
#define PGR_TSLOT_PROC(i)   (PGR_TGT_BASE + (i) * PGR_TGT_STRIDE + 0u)
#define PGR_TSLOT_VS(i)     (PGR_TGT_BASE + (i) * PGR_TGT_STRIDE + 1u)
/* Ledger D-5: a grant is a run of one-page frames, mirroring pager_proto.h. */
#define PGR_GRANT_PAGES     4u
#define PGR_GRANT_BASE      86u
#define PGR_PSLOT(j, p)     (PGR_GRANT_BASE + (j) * PGR_GRANT_PAGES + (p))
#define PGR_REPORT_GRANT    (1u << 22)
/* The pager service is the lifecycle_probe image in persistent service mode
 * (Phase 27): a real, separate, supervised image (NOT iris_test).  The control
 * endpoint lands in the probe's command slot; the manifest slots above match
 * services/lifecycle_probe/main.c's LP_PS_* layout. */
#define PGR_OP_PING         1u
#define PGR_OP_REPORT       2u
#define PGR_OP_MAP_RESUME   3u
#define PGR_OP_KILL         4u
#define PGR_OP_SHUTDOWN     5u
#define PGR_PACK(op, tidx, vidx, flags) \
    ((uint64_t)(op) | ((uint64_t)(tidx) << 8) | ((uint64_t)(vidx) << 16) | \
     ((uint64_t)((flags) & 0x3u) << 24))

/* The pager service's declared supervision policy (applied by this supervisor;
 * OPTIONAL_RESTART — a lost pager degrades paging for its targets but is not
 * fatal to the system). */
#define PGR_SUP_CLASS       IT_SUP_OPTIONAL_RESTART
#define PGR_RESTART_LIMIT   3u

/* Phase 27 fault/scratch VAs (clear of the T25/T26 windows). */
#define T27_VA_A    0x8098000000ULL
#define T27_VA_B    0x8099000000ULL
#define T27_PAT     0x27C0DE27u
#define T27_WMARK   0xFA017E57u   /* == the probe's LP_CMD_FAULT_WRITE store */

struct t27_pager {
    handle_id_t ctrl_ep;   /* supervisor's WRITE cap to call the pager */
    handle_id_t proc;      /* pager process cap (watch / kill) */
    long        reg_id;    /* svcmgr "pager.ep" registration id, or -1 */
    uint32_t    generation;/* supervisor-tracked restart generation */
};

/* ── T206: pager crash-loop containment ─────────────────────────────────────
 * A pager that dies right after start is respawned exactly `limit` times, then
 * the supervisor STOPS and marks the service degraded — no infinite loop, no
 * leak per attempt.  The target's fault meanwhile stays pending and observable;
 * the supervisor resolves it with its own authority.  Mirrors the Phase 24
 * crash-loop policy, now for the pager service.  Invariants: G18, G19, G23. */
#define T206_LIMIT PGR_RESTART_LIMIT

/* ── T210: deterministic pager-service stress ───────────────────────────────
 * Seeded mixed-operation rounds over the whole service surface: VMO-backed
 * read/write resolve, pager death + supervisor takeover, target death
 * mid-fault, unauthorized VMO/offset denial, restart, crash + resolve.  After
 * every round: no pending fault, no zombie, no registry ghost, and the live
 * books (process/task/handle/VSpace/mapping/VMO) at baseline.  Prints
 * seed/round/op on failure.  Invariants: G10–G27 under load. */
#define T210_SEED   0x27F00D27u
#define T210_ROUNDS 6u

/* ── Phase 28 Bloque A: boot-growth hardening (T211–T216) ─────────────────────
 *
 * The Phase 27 "wedge on the 10th image" was NOT a memory/alignment/allocator
 * bug: it was an over-strict boot assertion.  userboot required the kernel
 * initrd count to EQUAL a hardcoded catalog size and, on any mismatch, exited
 * before loading init — so adding ANY image (even a 256-byte blob, with
 * __data_start/__kernel_end byte-identical) left the system with no userland.
 * The fix relaxed the check to `count >= SL_CATALOG_COUNT` and made the
 * remaining genuine shortage emit a serial diagnostic instead of vanishing.
 * There is no fixed-size image array anywhere in the boot path (the kernel
 * g_initrd[] is sizeof-derived), so image growth is unbounded by construction.
 *
 * These tests exercise the boot-growth MECHANISMS from ring 3 (the boot itself
 * already proved it survives >SL_CATALOG_COUNT images by running this suite):
 * the initrd count/query surface, per-image size/mapping, loader failure
 * atomicity, explicit failure over silent wedge, the promoted pager binary,
 * and a seeded load matrix.  Invariants documented in
 * docs/architecture/boot-image-growth.md. */

#define T2_MIN_IMAGES   11u   /* SL_CATALOG_COUNT — every named index resolves */
#define T2_BADELF_IDX   11u   /* the invalid-ELF fixture image */

/* ── T216: deterministic boot-growth stress ─────────────────────────────────
 * A seeded matrix of load operations over the grown initrd: query counts, map
 * random in-range images, load valid vs invalid images, out-of-range queries.
 * Every op either succeeds or fails EXPLICITLY; the loader never wedges and
 * never drifts.  Boot progressed to run this (with the fixture images present),
 * so the growth itself is the standing proof; this hammers the mechanisms. */
#define T216_SEED   0x28B007A4u
#define T216_ROUNDS 12u

/* ── Phase 28 Bloque B: file-backed memory (T217–T230) ────────────────────────
 *
 * The pager (its own binary) now backs faults from files: a supervisor
 * registers a backing (identity + generation) and validated regions, and the
 * pager resolves faults by reading the file via VFS READ_AT, filling a VMO page
 * (RO cache or private-writable pool), mapping it, and seq-resuming.  These
 * tests drive the whole subsystem end to end and verify content through the
 * TARGET's own read (file → VFS → pager → VMO → target mapping → target byte).
 * Constants/structs MUST match services/pager/pager_proto.h. */

#define FBK_OP_PING               1u
#define FBK_OP_SHUTDOWN           5u
#define FBK_OP_MAP_REGION         6u
#define FBK_OP_REGISTER_BACKING   7u
#define FBK_OP_REGISTER_REGION    8u
#define FBK_OP_UNREGISTER_REGION  9u
#define FBK_OP_REVOKE_BACKING    10u
#define FBK_OP_DIAG              11u
#define FBK_OP_TARGET_RESET      12u
#define FBK_SLOT_VFS_EP    4u
#define FBK_SLOT_NOTIF     5u     /* A-22: the ONE shared fault ENDPOINT */
#define FBK_VMO_CACHE_SLOT  16u   /* pager slot 16 = VMO grant 0 (cache) */
#define FBK_VMO_PRIV_SLOT   17u   /* pager slot 17 = VMO grant 1 (private) */
#define FBK_MAX_BACKINGS   4u
#define FBK_MAX_REGIONS    16u    /* Phase 28.1: one region per possible target */
#define FBK_MAX_TARGETS    16u
#define FBK_CACHE_CAP      8u
#define FBK_PRIV_CAP       8u
#define FBK_MODE_RO         0u
#define FBK_MODE_PRIVATE    1u
#define FBK_MODE_SHARED_W   2u
#define FBK_PROT_R  1u
#define FBK_PROT_W  2u
#define FBK_PROT_X  4u
/* err markers (pager returns them negated). */
#define FBK_ERR_NOFAULT    0x63u
#define FBK_ERR_NO_REGION  0x64u
#define FBK_ERR_STALE_GEN  0x65u
#define FBK_ERR_SHORT_READ 0x66u
#define FBK_ERR_CACHE_FULL 0x67u
#define FBK_ERR_MODE       0x68u
#define FBK_ERR_ACCESS     0x69u
#define FBK_ERR_RANGE      0x6Au
#define FBK_ERR_NOBACK     0x6Bu
#define FBK_ERR_PRIVFULL   0x6Cu
#define FBK_ERR_GRANT      0x6Du   /* Phase 28.1: VFS denied the grant */
/* The productive pager's grant session (badge IRIS_BADGE_FILEGRANT_S(0)). */
#define FBK_SESSION        0u

/* File fixtures (must match the services/filebk .dat files + gen_fixtures.py). */
#define FBK_FILE_NAME    "fbk.dat"
#define FBK_FILE_SIZE    20480u          /* 5 pages */
#define FBK2_FILE_NAME   "fbk2.dat"
#define FBK2_FILE_SIZE   12288u          /* 3 pages */
#define ELFSEG_FILE_NAME "elfseg.dat"
#define ELFSEG_FILE_SIZE 16384u          /* 4 pages */
#define SMALL_FILE_NAME  "small.dat"
#define SMALL_FILE_SIZE  100u            /* sub-page */
#define LP_CMD_FAULT_READ_SEQ 0x109Fu   /* must match lifecycle_probe */
#define LP_CMD_FAULT_WRITE_M  0x109Du   /* mirror of LP_CMD_FAULT_WRITE */
#define LP_CMD_FAULT_READ_OFFS_M 0x10A5u   /* mirror of lifecycle_probe */

/* Phase 28 region VAs (clear of all prior windows). */
#define T28_VA_A   0x809A000000ULL
#define T28_VA_B   0x809B000000ULL
#define T28_VA_C   0x809C000000ULL

struct pgr_backing_req {   /* == services/pager/pager_proto.h */
    uint32_t backing_idx; uint32_t grant_idx;
    uint64_t backing_id; uint64_t generation; uint64_t file_size;
};
struct pgr_region_req {
    uint32_t region_idx; uint32_t target_idx; uint32_t backing_idx;
    uint32_t prot; uint32_t mode; uint32_t reserved;
    uint64_t start_va; uint64_t memory_length; uint64_t file_offset;
    uint64_t file_length; uint64_t backing_generation;
};
struct pgr_diag {
    uint32_t backing_live, region_count, cache_capacity, cache_entries,
             cache_hit, cache_miss, cache_evict, page_fill, page_fill_fail,
             generation_stale, grant_revoke, private_pages,
             notif_waits, notif_wakeups, pending_mask, target_resets,
             grant_denied;
};

struct t28_fbk {
    handle_id_t ctrl_ep;   /* supervisor's control cap */
    handle_id_t proc;      /* pager process cap */
    handle_id_t vfs_cap;   /* UNBADGED dup vfs cap (mint source; name-op client) */
    handle_id_t admin;     /* the grant-ADMIN cap (badge IRIS_BADGE_FILEGRANT_ADMIN) */
};

/* A VFS-issued file grant as seen by the supervisor (GRANT_OPEN reply). */
struct t28_grant { uint32_t idx; uint64_t bid; uint64_t gen; };

/* The pager-request staging buffer is the thread's own IPC buffer (D-4): a
 * static array here would be a foreign pointer, which the kernel refuses. */
#define g_t28_buf g_ep_io_buf


/* Self-mint a SESSION-badged vfs cap (badge IRIS_BADGE_FILEGRANT_S(s)) into
 * iris_test's own CSpace and materialize it.  This is byte-identical to the
 * cap a pager of session s holds — the hostile-client stand-in for the
 * compromised-pager tests.  Slots T28_FG_SLOT(s) are dedicated to the grant
 * sessions; the slot is DELETED before minting so re-entry (and any prior
 * occupant) is handled cleanly, then resolved to a fresh handle per call
 * (caller closes it). */
/* Slots 52..54 are free (between T091's 50/51 and TEST_UNTYPED at 55); the
 * harness therefore probes at most 3 distinct sessions.  The tests only need
 * sessions 0 (the productive pager) and 1 (the cross-session attacker). */
#define T28_FG_SLOT(s)  (52u + (s))
#define T28_FG_SESSIONS 3u

/* ── multi-target harness (T237/T238) ────────────────────────────────────────
 * A lightweight target group that shares ONE fault notification and ONE
 * exit-watch notification across ALL targets — so N targets cost the
 * supervisor exactly 2 notifications, never 2*N.  This is what lets 16 targets
 * be registered and faulted simultaneously without touching the per-process
 * notification quota.  The pager likewise holds ONE fault notification (slot
 * 5) for all of them, waking on bit (1<<i). */
#define T28_MT_MAX 16u
struct t28_multi {
    handle_id_t fault_notif;     /* A-22: shared fault ENDPOINT, badge i+1 */
    handle_id_t exit_notif;      /* shared: bit i set when target i exits */
    handle_id_t cmd[T28_MT_MAX];
    handle_id_t proc[T28_MT_MAX];
    handle_id_t vs[T28_MT_MAX];
    uint32_t    n;
};

/* SYS_UNTYPED_QUERY wrappers (versioned, read-only instrumentation). */
struct it_utq_global {
    uint32_t version, struct_size, live_untypeds, _pad0;
    uint64_t retype_count, retype_failures, reset_count;
    uint64_t reclaimed_bytes, reuse_count, overlap_denials;
    /* Stage 7-mem: the global gauges, moved here from SYS_RESOURCE_INFO —
     * facts about the KERNEL, not about a process, and the drift checks that
     * end most tests read them to prove nothing leaked. */
    uint32_t kslab_used_bytes, kslab_total_bytes, kslab_failed_allocs;
    uint32_t global_failed_charges, global_rollbacks;
    /* Stage 9-evt: syscall re-executions, and those that resumed on a FRESH
     * kernel stack with their original frame abandoned (ledger D-1). */
    uint32_t syscall_restarts;
    uint32_t syscall_abandons;
    /* Stage 9-evt step 3: ring-3 kernel entries whose user context was saved
     * into the interrupted thread's TCB instead of left on a kernel stack. */
    uint32_t irq_ctx_saves;
    /* Stage 8-cap: threads holding a registered IPC buffer frame (D-4). */
    uint32_t ipc_buffers;
    /* Stage 9-evt step 3: free pages in the kernel's physical allocator. */
    uint32_t kernel_free_pages;
    /* Stage 9-evt: 1 once the kernel's boot arena is sealed. */
    uint32_t kernel_heap_sealed;
    uint32_t _pad1;
    /* Ledger A-32: calls that still came through the NUMBERED door.  Must be
     * falling while the invocation ABI is adopted, and zero when it closes. */
    uint64_t syscall_numbered_calls;
};
struct it_utq_one {
    uint32_t version, struct_size;
    uint64_t phys_base, total_bytes, used_bytes, generation;
    uint32_t child_count, is_device;
};
struct it_utq_objects {
    uint32_t version, struct_size;
    uint32_t endpoints_live, notifications_live, replies_live, cnodes_live;
};
/*
 * The MDB/CDT gauge block, mirrored so the suite can OBSERVE it.
 *
 * `mdb_legacy_roots` is documented in the ABI as "must → 0": a LEGACY_ROOT is
 * a capability sitting in a CSpace with no parent in the derivation tree, so
 * revoking anything can never reach it.  Charter A9 — every derived capability
 * is traceable to its ancestor — is a claim about exactly this number, and
 * until now nothing read it.  T305 does.
 */
struct it_utq_mdb {
    uint32_t version, struct_size;
    /* Phase S2 Step C — TCB/SC blocks, skipped over to reach the MDB gauges. */
    uint32_t tcb_live, tcb_hwm, tcb_retyped, tcb_destroyed;
    uint32_t sc_live, sc_hwm, sc_retyped, sc_destroyed;
    uint32_t cdt_derivation_count, cdt_derivation_hwm, cdt_revoke_count,
             cdt_delete_count, cdt_cross_cnode_descendants,
             cdt_ipc_transfer_count, legacy_handle_derivation_migrated;
    uint32_t tcb_registry_active, tcb_registry_hwm,
             tcb_registry_exhaustions, tcb_registry_generation_mismatch;
    uint32_t mdb_nodes_live, mdb_nodes_hwm, mdb_legacy_roots,
             mdb_orphan_promotions, mdb_reparents, mdb_revoked_nodes,
             mdb_moves, mdb_max_depth;
};

/* Phase S2 C.1: arg0 = kind | version<<16 | size<<32 (declared buffer size). */
#define IT_QARG(kind, sz) ((long)((uint64_t)(kind) | ((uint64_t)1u << 16) | \
                                  ((uint64_t)(uint32_t)(sz) << 32)))

/* ── T238: deterministic file-authority and multi-target stress ───────────────
 * A seeded round-robin over the whole Phase 28.1 surface: open/derive/read a
 * grant, hostile wrong-backing and wrong-name attempts, revoke + generation
 * change, and a multi-target fault batch that dies in arbitrary order.  Every
 * round: no unauthorized read, no stale success, no target mix, notification
 * and object books at baseline, handle HWM bounded, no flakes.
 * Invariants: A1–A30 under load. */
#define T238_SEED   0x281F00D5u
#define T238_ROUNDS 8u

/* ── T240: many children, and the spawner's budget does not accumulate ──────
 *
 * The supervisor spawns 1, 8, 16, 32 children, kills them selectively, and
 * ends at an exact object baseline.  Whatever stops a spawn push is hit
 * CLEANLY — the spawner's Untyped, the loader's leaf range, or TASK_MAX, all
 * of them derived from something somebody allocated.
 *
 * Stage 7-mem restated the accounting half.  It used to read the loader's own
 * `vmos_usage` and assert it did not grow ~4x per child, because the child
 * image VMOs were charged to each CHILD's resource domain (the Phase 28.1
 * caller-charged bug, fixed).  There is no per-process domain any more: what
 * an image costs comes out of the per-child BUDGET svc_loader carves, and the
 * property that replaces "not charged to the loader" is stronger and easier to
 * be wrong about — **those budgets are RECYCLED**.  A child's pool is RESET and
 * reused by the next child at the same leaf, so running the same rung twice
 * must consume nothing the second time.  A loader that leaked a region per
 * spawn would pass the old test (it charged nobody's quota) and fail this one.
 * Invariants: Q4, Q5, Q12, Q13, Q20, Q23. */
#define T240_MAX 48u

/* ── T250: deterministic resource-accounting stress ──────────────────────────
 * Seeded rounds over the whole surface: create child, create VMO (self /
 * for-child), map/unmap, dup cap, kill child, near-exhaustion, cleanup.  Every
 * round: self usage returns to baseline, global counters coherent, high-water
 * monotone, snapshot exact.  Invariants: Q1–Q35 under load. */
#define T250_SEED   0x29ACC7E5u
#define T250_ROUNDS 10u

/* ════════════════════════════════════════════════════════════════════════
 * Phase S1 — seL4 Architectural Convergence (T251–T262).
 *
 * Canonical kernel-object model + Untyped-only allocation substrate: every
 * migrated object (Endpoint / Notification / Reply / CNode) is born from an
 * explicit Untyped via SYS_UNTYPED_RETYPE2, its storage IS the retyped
 * region, its capability appears directly in CSpace, and its region becomes
 * reusable after destruction + RESET.  Legacy create paths are retired.
 * ════════════════════════════════════════════════════════════════════════ */


/* S1 scratch slots: 241..250 (fz pool ends at 239; 240 is the T125 probe). */
#define S1_SLOT_A 241u
#define S1_SLOT_B 242u
#define S1_SLOT_C 243u
#define S1_SLOT_D 244u
#define S1_SLOT_E 245u

/* ── T262: deterministic Untyped object stress ──────────────────────────────
 * A seeded PRNG drives retype (single + batch) of endpoints / notifications
 * / replies, derives, IPC probes, signal/wait, deletes, forced batch
 * failures, stale-CPtr probes and full region reuse — against an EXACT
 * shadow model of child_count, and with the live-object gauges and kslab at
 * baseline after every round.  Prints seed/iteration only on failure. */
#define T262_SEED   0x51262262u
#define T262_ROUNDS 12u

/* ════════════════════════════════════════════════════════════════════════
 * Phase S2 — Untyped Task Construction (increment 1: SchedulingContext).
 * ════════════════════════════════════════════════════════════════════════ */

/* SYS_UNTYPED_QUERY kind 4 — task-object gauges + CDT counters. */
struct it_utq_taskobj {
    uint32_t version, struct_size;
    uint32_t tcb_live, tcb_hwm, tcb_retyped, tcb_destroyed;
    uint32_t sc_live, sc_hwm, sc_retyped, sc_destroyed;
    uint32_t cdt_deriv, cdt_deriv_hwm, cdt_revoke, cdt_delete,
             cdt_cross, cdt_ipc, legacy_handle_deriv_migrated;
    /* Phase S2 Step C — must mirror kernel struct iris_untyped_query_taskobj. */
    uint32_t tcb_registry_active, tcb_registry_hwm,
             tcb_registry_exhaustions, tcb_registry_generation_mismatch;
};

/* ── T289: cross-PROCESS derivation + revoke (real second process) ────────── */
#define T289_TSLOT 200u    /* free slot in IRIS_CPTR_TEST_PROC's root CNode */

/* ── T294: a receive slot below the root CNode ───────────────────────────
 * Until Stage 4 a receive slot had to be a DIRECT index into the root CNode,
 * so a process whose root was full could not receive a capability at all —
 * and this suite's root IS full (six slots free before the second-level CNode
 * at 80 was added).  The declaration is a full CPtr now, walked by
 * cspace_resolve_dest_slot, so a cap can be delivered into a second-level
 * CNode.
 *
 * The delivered value must ALSO be classified correctly on the way out: the
 * discriminator used to be the literal 1024, which would call this CPtr
 * (250 << 8 | 80 = 64080) a handle.  Asserting that the returned value both
 * equals the declared CPtr and resolves through CSpace pins that.
 * Invariants: I1 (transfer is CSpace to CSpace), I3, A3. */
#define T294_LEAF   250u
#define T294_CPTR   IT_OBJ_CPTR(T294_LEAF)

/* ── T298: an Untyped pays for its frames' headers too (Stage 6 Step 1) ──
 * A frame retyped from an Untyped always carved its PAGE from that Untyped;
 * its kernel-side header came from the kslab heap, so a caller who paid for a
 * page was also spending kernel memory that nothing accounted, nothing bounded
 * and no capability authorised — the implicit page-table problem, one level
 * down.  The header is now a child block of the same Untyped, carved from the
 * opposite end.
 *
 * Three things follow, and each is observable from ring 3:
 *
 *   1. retyping a frame costs the page AND the header: `available` drops by
 *      more than the page, and by less than two pages (which is what a header
 *      carved from the BOTTOM would have cost, by pushing the page carve onto
 *      the next boundary);
 *   2. consecutive frames stay page-dense — N frames cost N pages plus N small
 *      headers, not 2N pages;
 *   3. the frame still works: it maps, carries data, and unmaps.
 *
 * The header being outside the frame's own page is the security half of this,
 * and it is not directly observable from here — a header inside the page would
 * be readable by this test as non-zero bytes at the start of a freshly retyped
 * frame, so the map-and-read leg below is what would catch it.
 * Invariants: O1, O2, M3, M4. */
#define T298_VA   0x8079000000ULL

/* ── T299: page tables are charged to a budget (Stage 6 Step 2) ──────────
 * Mapping user memory needs page tables, and the kernel used to take them
 * from its own PMM reserve: a process could make the kernel spend memory by
 * mapping at scattered addresses, with no budget, no capability and no
 * accounting.  Charter M3 says the kernel does not implicitly allocate user
 * memory, and a page table that maps user memory is user memory.
 *
 * Every address space now names the Untyped that pays for its levels, at
 * creation.  Asserted here:
 *
 *   1. a spawn with NO budget is refused — the kernel does not fund it;
 *   2. a spawn with a budget of the wrong type is refused too (the argument
 *      is a capability, not a number);
 *   3. mapping into a FRESH 1 GiB-distant window really does consume that
 *      budget, and by page-sized amounts (the levels), not by nothing;
 *   4. while those tables are live, the budget cannot be RESET out from under
 *      them — page tables count as children of the Untyped that paid.
 *
 * Leg 3 maps into this suite's own address space, whose budget belongs to
 * init, so it is measured on a sub-untyped handed to a child instead: the
 * child spawn below is what makes the tables appear.
 * Invariants: M1, M3, O2, O6. */
#define T299_SLOT_POOL  S1_SLOT_C
#define T299_SLOT_PROC  S1_SLOT_D

/* ── T300: user memory comes out of a named budget (Stage 6 Step 5) ──────
 * Anonymous memory was the last thing the kernel handed out for free: a
 * process asked for a VMO and got PMM pages, bounded only by a per-process
 * quota the kernel invented rather than by a capability anyone delegated.
 *
 * A VMO's pages, its page-address array and its header now come from an
 * Untyped, and WHICH Untyped is the caller's to say — a process holds several
 * (the budget its address space was built from, the pools its supervisor
 * delegated) and they are not interchangeable.  Asserted here:
 *
 *   1. creating a VMO against a named budget consumes THAT budget, by at
 *      least the size asked for;
 *   2. the pages are real: the VMO maps, reads back zero-filled, and writes;
 *   3. a budget of the wrong type is refused — the argument is a capability;
 *   4. destroying the VMO returns its children, so the budget is RESET-able
 *      again and the region really is reusable.
 * Invariants: M1, M3, O2, O6. */
#define T300_VA 0x807A000000ULL

/* ── T301: a REFUSED address-space retype leaves the budget untouched ─────
 * Building an address space takes two carves out of one budget: a page for the
 * PML4, and a block for the KVSpace header that owns it.  A budget large
 * enough for one and not the other is where a half-built object comes from,
 * and a half-built object nothing can unwind is a page belonging to two owners
 * at once.
 *
 * SYS_PROCESS_CREATE used to make those carves, and got the order wrong: the
 * page first, then the header, so a failure in between left a PML4 the cleanup
 * path believed was PMM memory and returned to the buddy allocator while its
 * Untyped still owned the region.  Stage 6-pure Step 4 moved the carves into
 * RETYPE2(KOBJ_VSPACE), where the holder makes the address space itself — so
 * this test moved with them.  What it pins is unchanged, because the shape of
 * the mistake is unchanged:
 *
 *   1. a budget too small for a whole address space REFUSES the retype;
 *   2. after the refusal the budget has NO new children — a half-built object
 *      left nothing of itself behind;
 *   3. so the region is still whole: RESET succeeds;
 *   4. and the sweep really crossed the boundary — some size built one and
 *      some was refused.
 *
 * A plain sweep of page-sized budgets is enough here, and that is worth saying
 * because the earlier version of this test needed sub-page steps to reach its
 * target at all.  These two carves differ by a page: the header is a couple of
 * hundred bytes and the PML4 is 4096, so the band where one fits and the other
 * does not is a PAGE wide rather than a header wide.  A one-page budget lands
 * in it by construction.
 * Invariants: M1, M3, O2, O6. */
#define T301_SLOT_VS    S1_SLOT_D
#define T301_MAX_PAGES  6u

/* ── T302: a page table is a capability (Stage 6-pure Step 1) ────────────
 * Stage 6 charged page tables to a budget, which answered who pays.  It left
 * the kernel deciding WHEN a table exists and WHERE it goes — carving one
 * silently on whichever map first needed it — so the holder paid for an object
 * it could not name, count, delegate or reclaim.  Ledger D-5.
 *
 * A paging level is retyped like every other object now, and installed by an
 * explicit invocation (seL4_X86_PageTable_Map).  Asserted here:
 *
 *   1. it is a first-class retypable object, and says what it is;
 *   2. its region is always exactly one page — a level is 512 entries of 8
 *      bytes and nothing else, so any other size is refused;
 *   3. installing it fills the levels of a fresh address-space window from the
 *      top down, one invocation per level, and reports the walk complete when
 *      there is nothing left to fill;
 *   4. a table is installed at most once — the same region cannot be two parts
 *      of a walk;
 *   5. the ADDRESS is authority: a kernel-half address is refused, because
 *      installing there would splice a holder's page into the kernel's own
 *      walk;
 *   6. each level costs the budget a page, and the budget knows it.
 *
 * Leg 5 is the one that matters most: the argument that looks most like a hint
 * is the one that decides which PML4 slot gets written.
 * Invariants: M1, M3, O2, O6. */
#define T302_VA      0x8090000000ULL   /* a 512 GiB window nothing else uses */
#define T302_SLOT_PT S1_SLOT_D

/* ── T304: the live-process ceiling was a number, and it is gone ─────────
 *
 * Stage 7 Step 3.  KPROCESS_MAX_LIVE refused the 65th process — a number the
 * kernel picked, of the same class as the page quota Step 2 removed and the
 * notification quota Phase S1 removed.  Since Stage 6 Step 4 a KProcess is a
 * child block of an Untyped its creator named, so what bounds how many exist
 * is how much memory somebody delegated; refusing at 64 on top of that told a
 * holder with a large budget it had run out when it had not.
 *
 * Proving a removal needs the case that used to fail, so this creates MORE
 * than 64 processes at once out of one budget.  They are never started, which
 * is what makes it cheap (about 5 KB of kernel objects each and no ELF) and
 * also what makes the second half worth asserting: a process that never ran
 * used to be unreclaimable except through SYS_PROCESS_KILL, which special-
 * cased tearing one down because nothing else ever dropped the reference
 * SYS_PROCESS_CREATE kept on its own behalf.
 *
 * Stage 7 Step 13 dropped that reference at create time, so a never-started
 * process lives exactly as long as capabilities to it do — and DELETING THE
 * CAPABILITY is the whole of the reclamation this RESET depends on.  The kill
 * is gone from the loop below, and the RESET still has to pass.
 *
 * Three claims:
 *   1. more than 64 live processes exist simultaneously;
 *   2. the eventual refusal, whenever it comes, is a clean error — the budget
 *      answering, not a wedge;
 *   3. every byte goes back: after killing them all the pool RESETs, which it
 *      can only do when child_count has returned to zero, so no KProcess,
 *      root CNode, VSpace header or PML4 leaked.
 *
 * Invariants: Q20, Q29, S13. */
#define T304_CN_SLOT   S1_SLOT_D    /* a CNode of our own for the process caps */
#define T304_VS_SLOT   S1_SLOT_A
#define T304_CS_SLOT   S1_SLOT_B
#define T304_MAX       80u          /* > 64, and 127 leaves are addressable */
#define T304_TARGET    65u          /* the first count the old ceiling refused */

/* The boot path's own roots, measured.  A-14 took it from 43 to 32 by giving
 * every object retyped from a second-level Untyped its MDB parent; A-18 took
 * it to 23 by retiring the three SELF syscalls, each of which published an
 * unparented capability every time it was called.  A-20 adds ONE back: the
 * SchedControl capability boot mints for the root task, which is a boot-path
 * root like every other authority in BootInfo — seL4's are roots too.  Every
 * delegation of it downward is a child, so it costs exactly one.  A-21 adds
 * ONE more for the same reason: ASIDControl, the authority to carve
 * address-space identifier pools.  The POOL userboot carves from it is NOT a
 * root — it is retyped from an Untyped and parented there, which is the whole
 * point of the split. */
#define IT_MDB_LEGACY_ROOT_CEILING 25u

/* ── T309: a passive server serves a LOOP on donated time (Stage 8-mcs) ───
 *
 * seL4's ReplyRecv, and the reason it is one syscall rather than two.
 *
 * A passive server runs on time its caller donated, and SYS_REPLY gives that
 * time back.  Reply and receive as two separate calls therefore leave the
 * server, between them, runnable with NO scheduling context — and a thread
 * with no SC is not charged, so it runs unbudgeted for exactly as long as it
 * takes to make the second syscall.  That is the hole donation exists to
 * close, reopened one instruction after closing it.  SYS_REPLY_RECV removes
 * the gap: the server goes from running on donated time straight to blocked,
 * and is never in between.
 *
 * What this asserts is that the loop actually works over many requests: the
 * donation is re-established on every call, the reply object is re-staged
 * without being reallocated, and each answer reaches the right caller.  A
 * server that leaked its donation would stop after one request; one that
 * failed to re-stage would fail the second EP_CALL with NOT_SUPPORTED.
 */
#define T309_ROUNDS 8

/* ── T311: revoke is PREEMPTIBLE (ledger D-8) ────────────────────────────
 *
 * SYS_CSPACE_REVOKE used to run until the invoked capability's subtree was
 * exhausted, and nothing bounded the subtree.  A ring-3 principal that could
 * build a wide derivation tree could hold the CPU for as long as that tree was
 * large: the one in-kernel operation with no latency bound.  seL4 answers this
 * with zombie capabilities and a preemption point; the ledger recorded it as
 * blocked on the event kernel, because a preemptible delete needs somewhere to
 * park a continuation.
 *
 * Stage 9-evt step 1 built that somewhere, and revoke is the first thing to
 * use it.  This test builds a derivation subtree WIDER than one slice and
 * asserts two things that together mean "preemptible" rather than "still
 * atomic, just slower":
 *
 *   1. the syscall's restart counter ADVANCES — the operation really did give
 *      the CPU up part-way and come back;
 *   2. the ANSWER is still whole — it reports every capability it destroyed,
 *      not the count from its last slice, which is the accounting mistake a
 *      sliced operation invites.
 *
 * And the subtree really is gone afterwards, because a preemption point that
 * loses work is worse than none.
 */
#define T311_COPIES 24u
#define T311_LEAF_BASE (IT_OBJ_SLOT_SPAN + 20u)   /* clear of the rotating pool */

/* Root CNode radix is 8 (256 slots); a 2-bit guard sits just above the index. */
#define T312_GUARD      0x3u
#define T312_GUARD_BITS 2u
#define T312_PROBE      ((long)IRIS_CPTR_TEST_UNTYPED)
#define T312_PROBE_G    ((long)(((uint64_t)T312_GUARD << 8) | \
                                (uint64_t)IRIS_CPTR_TEST_UNTYPED))

/* ── T313: a thread's IPC buffer is a FRAME it owns (ledger D-4) ─────────
 *
 * The bulk payload of a message was staged in 256 bytes living INSIDE every
 * TCB.  Three things were wrong with that at once, and they are exactly the
 * three the charter denies the kernel everywhere else: the user did not choose
 * the size, did not pay for the memory, and could not name it with a
 * capability.  seL4 has no such object — a thread registers an IPC BUFFER
 * frame (`seL4_TCB_SetIPCBuffer`) and the kernel transfers between the two
 * ends' frames through its own window.
 *
 * Four things are asserted, and each one is a separate claim:
 *
 *   1. registration is AUTHORITY-CHECKED — a non-TCB, a non-frame and a
 *      nonsense address are all refused, and unregistering may not smuggle an
 *      address in;
 *   2. the payload travels with NO USER POINTER NAMED.  The client sets
 *      `buf_uptr = 0` and the bytes still arrive, which is the observable
 *      difference: there is no address for the kernel to validate and none for
 *      a second thread to invalidate between the check and the copy;
 *   3. the SIZE IS THE FRAME'S.  The payload here is deliberately larger than
 *      IRIS_IPC_BUF_SIZE, so it could not have gone through the staging path
 *      at all — the one leg that cannot pass by accident;
 *   4. UNREGISTERING really goes back.  The same oversized send, with the
 *      buffer given up, is clamped to the kernel constant again.
 *
 * Leg 4 is what keeps the row honest: until the services migrate, the staging
 * path is still there, and a test that only proved the new path worked would
 * let the old one rot unobserved.
 * Invariants: M3, O2, A6. */
#define T313_CLI_VA  0x807A000000ULL
#define T313_SRV_VA  0x807B000000ULL
#define T313_LEN     600u    /* deliberately > IRIS_IPC_BUF_SIZE (256) */

/* ── T314: an interrupted thread's context lives in its TCB (D-1 step 3) ──
 *
 * The last step of the event kernel is ONE kernel stack per core, and the
 * first analysis of it missed what makes it hard.  It is not enough for
 * syscalls to stop keeping state on the stack: a timer interrupt fires while a
 * thread runs in USER mode and lands on the stack named by TSS.RSP0.  If that
 * stack is per-core and the handler then hands the CPU to another thread, the
 * outgoing thread's interrupt frame is sitting on a stack the incoming thread
 * is about to use.
 *
 * So the preemption path has to save the whole ring-3 register state into the
 * TCB on the way in and rebuild it from the TCB on the way out.  That is what
 * this asserts, and it asserts it in the only two ways ring 3 can:
 *
 *   1. the path RUNS.  With per-thread kernel stacks still in place the save
 *      and the restore are an identity, so nothing about the machine changes
 *      and only the counter can show it happened.  A step whose effect is
 *      currently invisible is a step that rots silently; the gauge is the
 *      thing that keeps it honest until the stacks go.
 *   2. the restore is FAITHFUL.  A loop that keeps known values in the
 *      callee-saved registers across thousands of preemptions and checks every
 *      one of them afterwards.  Today this cannot fail, which is exactly why
 *      it is written now: when the frame stops being the authority, a restore
 *      that transposes two registers or drops one has to fail HERE and not in
 *      a service three phases later, as a wild pointer with no explanation.
 *
 * Invariants: this is the same discipline M4 demands of a partial operation,
 * applied to the one piece of state the kernel never used to own.
 */
/* Long enough that the timer fires inside it many times over, short enough
 * that the suite does not notice.  What matters is only that the loop is
 * preempted while those registers are live. */
#define T314_SPINS 20000000ULL

/* ── T317: a frame maps as a WHOLE, not just its first page (D-10) ───────
 *
 * `SYS_UNTYPED_RETYPE2` accepted any page multiple for a frame, and
 * `SYS_FRAME_MAP` installed exactly ONE PTE: `kframe_map_page` mapped
 * `f->paddr` and never read `f->size`.  A caller who bought a 64 KiB frame
 * spent 64 KiB of its Untyped and could reach 4 KiB of it, with no error
 * anywhere — the other fifteen pages were charged, owned, and unreachable.
 * Nothing had ever asked for one, which is why it went unnoticed rather than
 * why it was acceptable.
 *
 * seL4 has frame SIZES and a map covers the frame.  A map now installs every
 * page, an unmap removes every page, and the three bulk teardown paths walk
 * the frame rather than the mapping record's first address — because a
 * cleanup that removed one page of sixteen would leave PTEs outliving the
 * object that justified them, which is the one thing an unmap exists to
 * prevent.
 *
 * Four assertions, and the last two are the ones that would have caught the
 * original defect and its likely replacements:
 *
 *   1. a multi-page frame retypes and costs its whole size;
 *   2. EVERY page of it is readable and writable after one map — the first,
 *      the last, and a middle one, each with a distinct value so a mapping
 *      that aliased them all onto one page fails here;
 *   3. one unmap removes ALL of it: re-mapping the same window succeeds,
 *      which it cannot if stale PTEs were left behind (the map refuses a VA
 *      that is already occupied);
 *   4. a map that would overlap an existing mapping anywhere in its range is
 *      refused and changes NOTHING — the check runs over every page before
 *      any PTE is installed, so a failure cannot leave half a frame mapped.
 *
 * Invariants: V1, V3, O2. */
#define T317_VA    0x807C000000ULL
#define T317_PAGES 4u
#define T317_SIZE  (T317_PAGES * 4096u)

/* ── T318: kernel memory does not scale with thread count (D-1 step 3) ───
 *
 * The claim the event kernel was for, stated as a number.
 *
 * Every thread used to own 8 KiB of kernel stack plus a guard page, taken from
 * the kernel's physical reserve when the thread was created — including a TCB
 * the caller RETYPED from its own Untyped, whose payload it paid for and whose
 * kernel stack the kernel supplied anyway.  Charter M3 says the kernel does
 * not implicitly allocate memory on somebody's behalf, and that was the
 * largest standing exception to it: 8 KiB per thread, invisible from ring 3,
 * bounded only by how many threads anyone made.
 *
 * A thread owns no kernel stack now.  Its ring-3 context lives in its TCB, its
 * first entry lives there too, a parked syscall holds nothing, and every
 * kernel entry lands on the stack of the CORE.  So: make threads, and watch
 * the kernel's reserve not move.
 *
 * The bar is deliberately loose in one direction and tight in the other.  A
 * thread still costs memory — its TCB, its stack, its page tables — but all of
 * it comes from an Untyped the CALLER named, which is a different pool from
 * the one measured here.  What this asserts is that the KERNEL's pool does not
 * pay, and the old behaviour would have shown up as 2 pages per thread exactly.
 *
 * Invariants: M3, and ledger D-1. */
#define T318_THREADS 8u

/* ── T322: an object exists exactly while a capability to it exists (D-7) ────
 * seL4's lifetime rule, stated so it can be checked rather than described.
 * There is no reference count in seL4: an object is alive because a capability
 * names it, and its memory comes back when its Untyped is reset.  IRIS reaches
 * the same answer through two counters, and the ledger's D-7 row records three
 * separate incidents where the counters and the capability graph disagreed.
 *
 * So the property is asserted directly, for EVERY type RETYPE2 can make, and
 * the shape is the same for all of them:
 *
 *   retype into slot A · copy into slot B · delete A → the region is still
 *   BUSY, because B names the object · delete B → the region resets.
 *
 * The second step is the one that matters.  A counter that over-counts leaves
 * the region BUSY forever; one that under-counts destroys the object while B
 * still names it, and the reset would succeed one step early.  Both are
 * failures of the same assertion, which is why it is written as a pair.
 *
 * A type whose object outlives its last capability for a REASON — a thread the
 * scheduler still holds, an address space with a mapping in it — is not a bug
 * and is not asserted here; those are the two the table calls out.
 * Invariants: O2, O6, M1. */
struct t322_case { uint32_t type; long arg; const char *name; };

/* ── T323: the counter and the capability graph, over many shapes (D-7) ─────
 * T322 asserts seL4's lifetime rule on the simplest shape there is — one
 * object, two capabilities.  The three incidents D-7 records were not that
 * shape: they were a slot naming its own CNode, a cycle through another, and a
 * process object emptying a CSpace pre-emptively.  What they have in common is
 * that IRIS decides liveness with a COUNTER while seL4 reads the derivation
 * tree, and a counter can be wrong in ways a tree cannot.
 *
 * So the rule is asserted over shapes a seeded generator builds: a chain of
 * copies, each derived from a randomly chosen earlier one, so the MDB gets
 * real depth and branching — and then deleted in a shuffled order.  After
 * every single delete:
 *
 *   capabilities remaining > 0  ⟺  the region refuses to RESET.
 *
 * Both directions are checked at every step, which is what makes it a test of
 * AGREEMENT rather than of survival: a counter that over-counts fails the
 * final reset, one that under-counts fails an earlier BUSY, and an order that
 * happens to hide either is one seed away from an order that does not.
 * Prints seed/round/step on failure.  Invariants: O2, O6, M1. */
#define T323_SEED   0x0D7CA9EBu
#define T323_ROUNDS 12u
#define T323_MAXCAP 6u

/* ── T325: a notification has no waiter ceiling (charter P2) ────────────────
 * `KNOTIF_WAITERS_MAX` was 4 — a fixed array inside the object, and a fifth
 * waiter got IRIS_ERR_BUSY.  The kernel was deciding how many threads may wait
 * on a notification: a number it invented, of the same class as the
 * per-process quotas Stage 7 removed.  seL4 queues waiters intrusively through
 * the TCB and has no such limit, and so did IRIS's ENDPOINTS all along — the
 * same kernel answered the same question two ways.
 *
 * Six threads, which is two more than the ceiling that used to be here, so the
 * test fails against the old code rather than merely passing against the new.
 * Each blocks, each is woken by its own signal, and all six report.
 * Invariants: I3, P2. */
#define T325_THREADS 6u

/* ── T326: there is no ceiling on live threads (charter P2) ─────────────────
 * `TASK_MAX` was 256 and it was real: the scheduler kept an index-keyed
 * identity registry, and `task_registry_alloc` returned NO_MEMORY when the
 * array filled — the kernel telling a holder with memory and a capability that
 * it may not have another thread.  seL4 decides no such thing: a TCB exists
 * because somebody retyped one.  Everything that read the registry was WALKING
 * it, so it is an intrusive list now and removal is O(1).
 *
 * Two assertions, and neither is "count to 257" — that would need 256 live
 * threads with a stack and an IPC buffer each, and would measure the suite's
 * budget rather than the kernel's rule.
 *
 *   1. the EXHAUSTION counter is zero and stays zero.  It could only ever be
 *      incremented by the code path that refused a thread, and that path is
 *      gone; a non-zero reading means somebody put a ceiling back.
 *   2. a burst of threads well past what any array-index assumption would
 *      survive runs, and the registry returns to exactly where it started.
 * Invariants: P2, S1. */
#define T326_THREADS 24u


/* ── T328: an address space has to be NAMED before a thread can enter it ────
 * Ledger A-21, and the gauge for it.
 *
 * IRIS used to hand every VSpace a hardware identifier out of a kernel-global
 * bitmap the moment it was retyped.  Nobody could name that bitmap, nobody
 * could be refused from it, and when it ran out the kernel simply stopped
 * being able to build address spaces for reasons no ring-3 program could see
 * or account for.  seL4 has never worked that way: ASIDControl carves POOLS,
 * an ASIDPool ISSUES identifiers, and `seL4_X86_ASIDPool_Assign` is the step
 * between "I retyped a page directory" and "a thread can run in it".
 *
 * Five claims, and the first is the one that makes the rest mean anything:
 *
 *  1. a freshly retyped VSpace is UNNAMED, and binding a thread to it is
 *     refused.  If this ever passes by accident the whole model is decorative.
 *  2. assigning from a pool this task HOLDS makes it bindable.
 *  3. assigning twice is refused — an identifier is what the hardware has
 *     cached translations under, so re-naming a live space would leave them
 *     reachable under a name somebody else now holds.
 *  4. the pool argument is a capability: a wrong type is not a pool, and a
 *     pool without WRITE is not authority to issue from it.
 *  5. the identifiers come BACK.  A space that is destroyed returns its name
 *     to the pool that issued it, which is the property that makes "how many
 *     address spaces may exist" a question about the capability graph instead
 *     of about a number compiled into the kernel.
 *
 * Claim 5 is asserted by exhausting nothing: the loop below builds and
 * destroys more address spaces than a pool holds identifiers, which can only
 * finish if every one was returned.
 * Invariants: A1, A5, O1. */
#define T328_ROUNDS   (IRIS_ASID_POOL_SIZE + 8u)
#define T328_SLOT_VS  S1_SLOT_E



/* ── T329: a fault is IPC, and the badge says whose (A-22) ──────────────────
 *
 * The fault path used to be three mechanisms where seL4 reuses one: a
 * NOTIFICATION was signalled, the faulting thread's capability was published
 * into a MAILBOX CNode slot the registrant had declared, the handler read the
 * record with SYS_TCB_FAULT_INFO and answered with SYS_EXCEPTION_RESUME
 * carrying a generation number.  Each of the three re-invented something IPC
 * already had — the mailbox was a hand-rolled capability delivery with its own
 * parent tracking so revoke could reach it, and the generation was a
 * hand-rolled one-shot token.
 *
 * Five claims, and the third is the one the mailbox existed for:
 *
 *  1. a fault ARRIVES as a message on an endpoint, labelled FAULT_MSG_NOTIFY,
 *     with the record in the message registers — one receive where it used to
 *     be a signal plus a syscall to fetch what the signal could not carry;
 *  2. the reply capability that comes with it resumes the thread, and is the
 *     ONLY thing that does;
 *  3. two threads armed on ONE endpoint through differently BADGED copies
 *     produce distinguishable faults.  This is what replaced the mailbox: a
 *     handler learns whose fault it is from the fault, so nothing has to be
 *     minted into anybody's CSpace when a thread faults;
 *  4. taking delivery needs RIGHT_READ on the endpoint — a handler that was
 *     given a write-only capability can arm faults and never see one;
 *  5. the two retired syscalls answer NOT_SUPPORTED, whoever calls them and
 *     whatever they hold.
 *
 * Invariants: A1, A5, F1, F9. */
#define T329_LEAF_A 5u
#define T329_LEAF_B 6u
#define T333_DST_SLOT  IT_SCRATCH_3

/* ── T334: a transfer is a COPY, and what the receiver got is a CHILD ───────
 *
 * Ledger A-29.  Sending a capability over an endpoint used to EMPTY the
 * sender's slot, and the reason that survived so long is that nothing here
 * ever asked the question — every caller in the system happened to be giving
 * the capability away, so a move and a copy-then-delete looked identical from
 * the outside.  What gave it away was not a test but a contradiction inside
 * the kernel: the delivered capability was installed as an MDB **child** of
 * the sender's slot, and then the parent was deleted.  Ancestry that only
 * means something for a copy, recorded and then thrown away.
 *
 * So this is the test that would have caught it.  Three claims:
 *
 *  1. the sender still holds what it sent, and it still works;
 *  2. the receiver's capability is a DERIVATION CHILD of the sender's slot —
 *     revoking that slot destroys the receiver's copy, and the sender's own
 *     capability survives its own revoke (children only);
 *  3. giving a capability away is still possible, and is two steps that both
 *     belong to the sender: send, then delete.  The receiver's copy outlives
 *     the sender's slot, because deleting a parent is not revoking it.
 *
 * Invariants: A1, A3, A8, O4. */
#define T334_SRC_SLOT  IT_SCRATCH_0
#define T334_DST_SLOT  IT_SCRATCH_3

/* ── T324: what the rotating object pool is still holding ──────────────────
 * The pool's contract is one sentence — delete before use, never hold a slot
 * across a test boundary — and until now nothing read it back.  The pool is
 * 196 leaves and the suite fabricates thousands of objects, so the counter
 * wraps many times per run: a capability left in a leaf gets deleted out from
 * under its owner laps later, in somebody else's test, as something that was
 * fine an hour ago suddenly resolving NOT_FOUND.  That is how four separate
 * slot collisions in this convergence presented.
 *
 * TWO assertions, and the first is the one that matters.
 *
 * (1) THE SCAN COMPLETES.  Reading a slot retains what it names, so a slot
 *     that outlived its OBJECT panics the kernel — "resurrect from refcount
 *     0" — and no test could ever have seen it, because no test reads an idle
 *     slot.  This one did, and it found one: T308's scheduling context, killed
 *     by the donation-return path handing a borrowed SC back to a lender after
 *     the borrower had already released it (fixed in kreply.c).  A capability
 *     graph and a reference count disagreeing is D-7's whole subject, and this
 *     is the assertion that makes the disagreement visible.
 *
 * (2) OCCUPANCY IS BOUNDED.  A ceiling, not zero, and the difference is
 *     honest: the count is a DEBT.  Every entry is a test that kept a leaf —
 *     mostly threads, whose TCB capability it_thread_create leaves in the pool
 *     — and paying it down means auditing each one.  The ceiling is what
 *     stops it growing while that happens, and the number is printed every run
 *     so paying it down is visible. */
/*
 * The debt, measured: 52 when this test was written, 27 once threads got a
 * CNode of their own and T308/T309 released what they made.  The ceiling has
 * headroom for run-to-run variance — the count includes capabilities to
 * threads that may or may not have been reaped by the time the scan runs — and
 * it is a DEBT, not a design: every entry is a test that kept a leaf of a pool
 * that recycles.  It goes down as they are paid.
 */
/*
 * 26 → 28 at A-30, and the reason is arithmetic rather than tolerance.  T335
 * was written using the rotating pool and moved to fixed scratch slots, which
 * removed four rotations from the run.  Held went 21 → 25 and evictions went
 * 9 → 5: 21+9 and 25+5 are the same thirty capabilities, so nothing new is
 * being kept — four that used to be recycled now sit still, which is the state
 * the pool exists to make visible.  The eviction count, the one the comment
 * below says matters more, went DOWN.
 *
 * The 2 of headroom is deliberate: a ceiling reached exactly is a ceiling that
 * gets bumped by the next person rather than explained.
 */
#define IT_POOL_HELD_CEILING 28u

/*
 * ...and the number that matters more.
 *
 * The debt at rest is harmless until the allocator comes round to it.  This is
 * the coming round: how many times, this run, a rotating leaf was recycled
 * while it still held something.  35 when it was first measured — 33 of them
 * SYS_TCB_SELF publications made inside loops and abandoned one per iteration
 * — and 5 once those released what they took.
 *
 * Not all five are defects: the pool exists to recycle what a finished test
 * abandoned, and an eviction of a genuinely abandoned capability is the pool
 * doing its job.  The allocator cannot tell the two apart from the inside, and
 * that is exactly why the number is capped rather than explained: the ones
 * that ARE defects are indistinguishable until they cost something, and four
 * slot collisions in this convergence are what they cost.
 */
#define IT_POOL_EVICT_CEILING 6u

/* ── T296: one capability, one authority (Stage 5 Step 2) ───────────────
 * Device authority used to be a BIT (IRIS_BOOTCAP_HW_ACCESS) on the same
 * capability that carries spawn, debug and framebuffer authority.  Holding the
 * authority to claim a serial port therefore meant holding the authority to
 * claim any interrupt line, spawn processes and power the machine off, and
 * dropping one of those meant cloning a narrowed copy of the whole object.
 *
 * Now there is one capability per authority and the kernel matches it
 * EXACTLY.  Three things follow, and all three are asserted here because each
 * would be silently undone by a resolver that went back to subset matching:
 *
 *   1. each control capability authorises its OWN syscall;
 *   2. neither authorises the other's — the IRQ control capability cannot
 *      create an ioport capability, and vice versa;
 *   3. the boot capability that still carries the remaining mask authorises
 *      NEITHER, even though it is the object they were split from.
 *
 * Extended in Step 2b as each further authority splits off: debug authority
 * (kernel log, scheduler statistics, poweroff) is a capability of its own,
 * which the device capabilities do not imply and which does not imply them.
 *
 * A non-whitelisted port is used for the negative ioport probes only where
 * the whitelist gate cannot mask the result: the whitelist runs BEFORE the
 * authority check, so the positive control is a whitelisted range and the
 * cross-authority probes use one too.
 * Invariants: A1, A5, A7. */
#define T296_SLOT      S1_SLOT_C
#define T296_WL_PORT   0x02F8   /* COM2 — whitelisted, unused by services */

/* ── everything the suite's files share ─────────────────────────────────── */

extern handle_id_t g_serial_h;
void it_serial_write(const char *s);
void it_log_num(uint32_t n);
extern uint32_t g_pass;
extern uint32_t g_total;
void it_child_keep_vspace(void);
void it_child_bind(handle_id_t proc_h);
long it_child_tcb_dest(void);
long it_child_vs_dest(void);
long it_child_tcb(handle_id_t proc_h);
long it_child_vspace(handle_id_t proc_h);
void it_child_drop_vspace(handle_id_t proc_h);
long it_kill(long proc_cptr);
long it_tcb_alive(long tcb_cptr);
long it_alive(long proc_cptr);
int it_fault_reply_fresh(uint32_t i);
int it_pgr_mbox_fresh(uint32_t nleaves);
extern uint32_t g_it_obj_slot_next;
long it_retype2_at(long ut, uint32_t obj_type, uint32_t slot,
                          uint32_t count, long obj_arg);
extern uint32_t g_it_pool_evictions;
extern uint32_t g_it_pool_evict_by_type[20];
long it_retype_slot_alloc(long ut, uint32_t obj_type, long obj_arg);
long it_cs_badge(long src_cptr, uint32_t rights, uint32_t badge);
long it_cs_reduce(long src_cptr, uint32_t rights);
void it_settle(uint32_t rounds);
long it_timer_uptime(uint64_t *out_ns);
long it_wait_timeout(long notif, long out_bits_uptr, long ns);
long it_ioport_create(long auth, long base, long count, long dest);
long it_ioport_narrow(long auth, long first, long last, uint32_t dest);
long it_irqcap_create(long auth, long irq, long dest);
extern long g_it_initrd_size;
long it_initrd_vmo_slot(long auth_cptr, long index);
long it_own_tcb_derived(void);
long it_vspace_self_slot(void);
long it_frame_create_slot(long ut, uint64_t bytes);
long it_ep_create_slot(void);
long it_notify_create_slot(void);
long it_ep_create(void);
long it_notify_create(void);
long it_reply_create_at(uint32_t slot);
extern uint32_t g_it_slot_guard_hits;
extern uint32_t g_it_slot_guard_last;
void it_slot_delete(uint32_t slot);
long it_xfer_slot(handle_id_t src_h, uint32_t slot, uint32_t rights);
int it_slot_is_notif(long slot);
long it_xfer_slot_norights(long src_h, uint32_t slot, uint32_t rights);
long it_cdt_root(handle_id_t src_h, uint32_t slot);
long it_cdt_derive(long src_cptr, uint32_t dest_slot, uint32_t rights);
long it_cdt_reduced(handle_id_t src_h, uint32_t root_slot,
                           uint32_t dest_slot, uint32_t rights);
int it_cdt_alive(long cptr);
long it_cdt_revoke(long cptr);
long it_xfer_dup(long src_h, uint32_t rights);
void it_xfer_release(long cptr);
void it_pass(const char *id);
void it_fail(const char *id, const char *reason);
void it_close(handle_id_t *h);
void test_t001(void);
void test_t002(void);
void test_t003(void);
void test_t008(void);
void test_t009(void);
void test_t010(void);
void test_t013(void);
void test_t014(void);
void test_t015(void);
void test_t016(void);
void test_t018(void);
void test_t019(void);
void test_t020(void);
void test_t021(void);
extern uint8_t *g_ep_io_buf;
void test_t022(void);
void test_t023(void);
void test_t024(void);
void test_t025(void);
extern handle_id_t g_svcmgr_ep_h;
extern handle_id_t g_vfs_ep_h;
extern uint8_t *g_ep_io_buf;
uint32_t it_stage_path(const char *path);
void test_t026(void);
void test_t027(void);
void test_t028(void);
void test_t029(void);
void test_t030(void);
void test_t031(void);
void test_t032(void);
void test_t033(void);
void test_t034(void);
void test_t035(void);
void test_t036(void);
void test_t037(void);
void test_t038(void);
void test_t039(void);
void test_t040(void);
void test_t041(void);
void test_t042(void);
void test_t043(void);
void test_t044(void);
void test_t045(void);
long it_ping_badge(long cptr, uint64_t *out_badge);
void test_t047(void);
void test_t048(void);
void test_t049(void);
void test_t050(void);
void test_t051(void);
void test_t052(void);
void test_t053(void);
long it_status(const char *name, uint32_t *alive, uint32_t *gen);
long it_register_ep(const char *name, handle_id_t ep);
void test_t054(void);
void test_t055(void);
void test_t056(void);
void test_t057(void);
void test_t058(void);
void test_t059(void);
void test_t060(void);
void test_t061(void);
void test_t062(void);
void test_t063(void);
void test_t064(void);
void test_t065(void);
void test_t066(void);
void test_t067(void);
void test_t068(void);
void test_t069(void);
void test_t070(void);
void test_t071(void);
void test_t072(void);
void test_t073(void);
void test_t074(void);
long lp_spawn_child_cn(uint32_t cn_leaf, handle_id_t cmd_ep_h,
                              handle_id_t *out_proc_h);
long lp_spawn_child(handle_id_t cmd_ep_h, handle_id_t *out_proc_h);
void test_t075(void);
void test_t077(void);
void test_t078(void);
void test_t076(void);
void test_t079(void);
void test_t080(void);
void test_t081(void);
void test_t082(void);
void test_t083(void);
void test_t084(void);
void test_t085(void);
void test_t086(void);
void test_t087(void);
void test_t088(void);
long it_lookup_name_slot(const char *name, uint32_t reply_slot,
                                struct iris_msg *msg);
long it_unregister_id(uint32_t id);
void test_t089(void);
void test_t090(void);
void test_t091(void);
void test_t092(void);
int it_sched_ext(uint32_t w[14]);
int it_task_live(uint32_t *out);
int it_sched_ext3(uint32_t w3[6]);
int it_sched_ext4(uint32_t w4[5]);
int it_sched_ext5(uint32_t w5[5]);
int it_setup_self_vspace(void);
long it_map_fixup(long nr, long a0, long a1, long a2, long a3);
long it_map_fixup_inv(unsigned long label, long c, long a1, long a2, long a3);
long it_cspace_self(void);
long it_thread_create(uint64_t entry, uint64_t rsp, uint64_t arg);
long it_thread_ipc_buffer(long tcb);
/* A-33: the IPC-buffer VA of the thread it_thread_create just made. */
extern volatile uint64_t g_it_thread_buf;
int it_sched_ext2(uint32_t w2[4]);
void test_t093(void);
void test_t094(void);
void test_t095(void);
void test_t096(void);
void test_t097(void);
void test_t098(void);
long it_lp_cmd_rslot(handle_id_t cmd_ep_h, uint32_t slot);
long it_lp_send_cap(handle_id_t cmd_ep_h, long notif);
long it_lp_cmd(handle_id_t cmd_ep_h, uint32_t label);
long it_lp_wait_exit(handle_id_t proc_h);
void test_t099(void);
void test_t100(void);
void test_t101(void);
void test_t102(void);
void test_t103(void);
void test_t104(void);
void test_t105(void);
void test_t106(void);
extern uint32_t g_fz_seed;
uint32_t fz_rand(void);
void fz_note(const char *t, uint32_t seed, uint32_t iter);
extern handle_id_t       g_fz_data_ep;
void test_t107(void);
void test_t108(void);
void test_t109(void);
void test_t110(void);
void test_t111(void);
void test_t112(void);
void it_quiesce_reaper(void);
void test_t113(void);
void test_t114(void);
void test_t115(void);
void test_t116(void);
void test_t117(void);
void test_t118(void);
extern uint8_t           g_sh_stk[SH_NWORK][8192];
extern volatile int      g_sh_done[SH_NWORK];
extern volatile uint32_t g_sh_prog[SH_NWORK];
extern handle_id_t       g_sh_ep;
extern volatile uint32_t g_sh_mode;
extern volatile uint32_t g_sh_iters;
extern handle_id_t       g_sh_sc;
extern void (*const g_sh_entries[SH_NWORK])(void);
int sh_start(uint32_t n);
int sh_wait_all(uint32_t n);
void test_t119(void);
void test_t120(void);
extern volatile long g_t121_res[3];
extern void (*const g_t121_entries[3])(void);
void test_t121(void);
void test_t122(void);
void test_t123(void);
void test_t124(void);
long it_auth_ut(void);
int it_ut_reset(void);
void test_t125(void);
void test_t126(void);
void test_t127(void);
void test_t128(void);
void test_t129(void);
void test_t130(void);
void test_t131(void);
handle_id_t it_retype_frame(void);
void test_t132(void);
void test_t133(void);
void test_t134(void);
void test_t135(void);
void test_t136(void);
void test_t137(void);
void test_t138(void);
void test_t139(void);
extern uint8_t g_it_fault_have[IT_FAULT_LEAVES];
extern uint64_t g_it_fault_label[IT_FAULT_LEAVES];
extern uint64_t g_it_fault_badge[IT_FAULT_LEAVES];
long it_fault_info(uint32_t leaf, struct it_fault *f);
long it_lp_cmd_va(handle_id_t ep_h, uint32_t label, uint64_t va);
int it_fault_wait_ep(long fault_ep, uint32_t mbox);
long it_fault_resume(uint32_t mbox);
long it_fault_kill(uint32_t mbox);
void test_t140(void);
void test_t141(void);
void test_t142(void);
void test_t143(void);
void test_t144(void);
void test_t145(void);
void test_t146(void);
void test_t147(void);
struct it_snap it_snap_take(void);
int it_snap_baseline(const struct it_snap *a, const struct it_snap *b,
                            const char **why);
int it_snap_baseline_live(const struct it_snap *a, const struct it_snap *b,
                                 const char **why);
void it_fz_note(const char *t, uint32_t seed, uint32_t iter, uint32_t op);
extern const long it_fz_bad_handles[4];
void test_t148(void);
void test_t149(void);
void test_t150(void);
void test_t151(void);
void test_t152(void);
void test_t153(void);
void test_t154(void);
void test_t155(void);
long it_lp_report_slots(const struct svc_mint *extra, uint32_t nextra);
long it_lookup_rights(long svcmgr_cptr, const char *name);
void test_t156(void);
void test_t157(void);
void test_t158(void);
void test_t159(void);
void test_t160(void);
void test_t161(void);
void test_t162(void);
void test_t163(void);
void test_t164(void);
void test_t165(void);
void test_t166(void);
void test_t167(void);
void test_t168(void);
void test_t169(void);
void test_t170(void);
void test_t171(void);
long it_unregister(uint32_t dyn_id);
void test_t172(void);
void test_t173(void);
void test_t174(void);
void test_t175(void);
void test_t176(void);
void test_t177(void);
void test_t178(void);
void test_t179(void);
void test_t180(void);
void t25_reap(handle_id_t *proc_h);
void t25_tgt_reap(struct t25_tgt *g);
int t25_tgt_spawn(struct t25_tgt *g, const char **why);
long t25_pager_spawn(const struct t25_tgt *g, handle_id_t frame_h,
                            iris_rights_t frame_rights,
                            const struct svc_mint *extra, uint32_t nextra,
                            handle_id_t *out_cmd, handle_id_t *out_proc);
long t25_serve(handle_id_t pcmd, uint32_t sub, uint32_t count,
                      uint64_t mflags, uint64_t va_ovr, uint64_t expect_cr2);
long t25_xprobe(handle_id_t pcmd, uint32_t vtid, uint64_t va, uint32_t vseq);
long t25_resume_seq(const struct t25_tgt *g, uint32_t tid, uint32_t seq,
                           int kill);
int t25_wait_fault(const struct t25_tgt *g, struct it_fault *f);
int t25_wait_delivered(uint32_t base);
uint32_t t25_delivered_now(void);
int t25_wait_refault(const struct t25_tgt *g, uint32_t old_seq,
                            struct it_fault *f);
int t25_frame_word(handle_id_t fr, uint32_t *val, int write);
void test_t181(void);
void test_t182(void);
void test_t183(void);
void test_t184(void);
void test_t185(void);
void test_t186(void);
void test_t187(void);
void test_t188(void);
void test_t189(void);
void test_t190(void);
long it_frame_live(void);
void t26_grant_close(handle_id_t *g);
handle_id_t t26_grant(void);
int t26_page_word(handle_id_t page, uint32_t *val, int write);
void test_t191(void);
void test_t192(void);
void test_t193(void);
void test_t194(void);
void test_t195(void);
void test_t196(void);
void test_t197(void);
void test_t198(void);
void test_t199(void);
void test_t200(void);
int t27_pager_spawn(struct t27_pager *p,
                           struct t25_tgt *targets, uint32_t nt,
                           handle_id_t *vmos, uint32_t nv, uint32_t vmo_w_mask,
                           int do_register, const char **why);
long t27_pager_call(handle_id_t ctrl_ep, uint32_t op, uint32_t tidx,
                           uint32_t vidx, uint32_t flags,
                           uint64_t offset, uint64_t expect);
void t27_pager_reap(struct t27_pager *p);
int t27_resolve_read(struct t27_pager *p, struct t25_tgt *g,
                            uint32_t tidx, uint32_t vidx, uint64_t offset,
                            uint64_t va, uint32_t pat, const char **why);
void test_t201(void);
void test_t202(void);
void test_t203(void);
void test_t204(void);
void test_t205(void);
void test_t206(void);
void test_t207(void);
void test_t208(void);
void test_t209(void);
uint32_t t210_rnd(uint32_t *s);
void test_t210(void);
void test_t211(void);
void test_t212(void);
void test_t213(void);
void test_t214(void);
void test_t215(void);
void test_t216(void);
long t28_stat(handle_id_t vfs_cap, const char *name);
long t28_grant_revoke_name(handle_id_t admin, const char *name, uint64_t *newgen);
int t28_fbk_spawn(struct t28_fbk *f, struct t25_tgt *targets, uint32_t nt,
                         const char **why);
void t28_fbk_reap(struct t28_fbk *f);
int t28_backing_setup(struct t28_fbk *f, uint32_t bidx, const char *name,
                             uint64_t size, struct t28_grant *gr, const char **why);
long t28_reg_region(handle_id_t ctrl, const struct pgr_region_req *src);
void t28_region(struct pgr_region_req *rq, uint32_t ridx, uint32_t tidx, uint32_t bidx,
                       uint64_t va, uint64_t mem_len, uint64_t file_off, uint64_t file_len,
                       uint32_t prot, uint32_t mode, uint64_t gen);
int t28_read_verify(struct t28_fbk *f, struct t25_tgt *g, uint32_t tidx,
                           uint64_t va, uint8_t expect_byte, const char **why);
void test_t217(void);
void test_t218(void);
void test_t219(void);
int t28_write_resolve(struct t28_fbk *f, struct t25_tgt *g, uint32_t tidx,
                             uint64_t va, const char **why);
void test_t220(void);
void test_t221(void);
void test_t222(void);
void test_t223(void);
void test_t224(void);
void test_t225(void);
void test_t226(void);
void test_t227(void);
void test_t228(void);
void test_t229(void);
void test_t230(void);
void test_t231(void);
void test_t232(void);
void test_t233(void);
void test_t234(void);
void test_t235(void);
void test_t236(void);
void test_t237(void);
int it_utq_g(struct it_utq_global *q);
int it_utq_1(long ut, struct it_utq_one *q);
int it_utq_o(struct it_utq_objects *q);
int it_utq_mdb(struct it_utq_mdb *q);
long s1_sub_ut(uint64_t bytes);
void test_t238(void);
int it_bare_child(handle_id_t *cmd_out, handle_id_t *proc_out);
void it_bare_kill(handle_id_t *cmd, handle_id_t *proc);
void test_t239(void);
void test_t240(void);
void test_t244(void);
void test_t245(void);
void test_t248(void);
void test_t249(void);
void test_t250(void);
void test_t251(void);
void test_t252(void);
void test_t253(void);
void test_t254(void);
void test_t255(void);
void test_t256(void);
void test_t257(void);
void test_t258(void);
void test_t259(void);
void test_t260(void);
void test_t261(void);
void test_t262(void);
int it_utq_t(struct it_utq_taskobj *q);
void test_t267(void);
void test_t283(void);
void test_t284(void);
void test_t285(void);
void test_t286(void);
void test_t287(void);
void test_t288(void);
void test_t289(void);
void test_t290(void);
void test_t292(void);
void test_t293(void);
void test_t294(void);
void test_t297(void);
void test_t298(void);
void test_t299(void);
void test_t300(void);
void test_t301(void);
void test_t302(void);
void test_t303(void);
void test_t304(void);
uint32_t it_ipc_buffer_gauge(void);
void test_t305(void);
void test_t306(void);
void test_t307(void);
void test_t308(void);
void test_t309(void);
void test_t310(void);
void test_t311(void);
void test_t312(void);
void test_t313(void);
void test_t314(void);
void test_t315(void);
void test_t316(void);
void test_t317(void);
void test_t318(void);
void test_t320(void);
void test_t321(void);
void test_t322(void);
void test_t323(void);
void test_t325(void);
extern volatile uint32_t g_t326_ran;
extern uint8_t           g_t326_stacks[T326_THREADS][2048];
void t326_body(void);
void test_t326(void);
void test_t327(void);
void test_t328(void);
void test_t329(void);
void test_t330(void);
void test_t331(void);
void test_t332(void);
void test_t333(void);
void test_t334(void);
void test_t335(void);
void test_t336(void);
void test_t337(void);
void test_t338(void);
void test_t339(void);
void test_t340(void);
void test_t324(void);
void test_t319(void);
void test_t296(void);
void test_t295(void);

/* ── the syscall wrappers, inline because every test is made of them ────── */


/* ── Syscall helpers ────────────────────────────────────────────────────── */

static inline long it_sys0(long nr) {
    return iris_syscall4((long)nr, (long)0L, (long)0L, (long)0L, (long)0);
}

static inline long it_sys1(long nr, long a0) {
    return iris_syscall4((long)nr, (long)a0, (long)0L, (long)0L, (long)0);
}

static inline long it_sys2(long nr, long a0, long a1) {
    /* Phase S1: arg2 (rdx) is ALWAYS zeroed — SYS_EP_RECV/SYS_EP_NB_RECV now
     * interpret it as the explicit reply-object CPtr, so leaking garbage
     * there would randomly stage bogus replies. */
    return iris_syscall3(nr, a0, a1, 0L);
}

static inline long it_sys3(long nr, long a0, long a1, long a2) {
    long r = iris_syscall3(nr, a0, a1, a2);
    if (r == (long)IRIS_ERR_MISSING_TABLE) r = it_map_fixup(nr, a0, a1, a2, 0);
    return r;
}

static inline long it_sys4(long nr, long a0, long a1, long a2, long a3) {
    long r = iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)a3);
    if (r == (long)IRIS_ERR_MISSING_TABLE) r = it_map_fixup(nr, a0, a1, a2, a3);
    return r;
}

/*
 * ── Ledger A-32: the suite invokes capabilities ──────────────────────────
 *
 * `it_sysN(SYS_X, cap, …)` becomes `it_invokeN(cap, INV_X, …)`.  The shape of
 * a call site changes in one way that is the whole point of the conversion:
 * the capability is no longer an argument that happens to come first, it is
 * what is being invoked, and the method cannot be named without it.
 *
 * The MISSING_TABLE fixup rides along unchanged — a map into a window with no
 * paging levels under it is answered by supplying them and retrying, and that
 * is a property of mapping rather than of how the map was named.
 */
static inline long it_invoke(long c, unsigned long label,
                             long a1, long a2, long a3) {
    long r = iris_invoke(c, label, a1, a2, a3);
    if (r == (long)IRIS_ERR_MISSING_TABLE)
        r = it_map_fixup_inv(label, c, a1, a2, a3);
    return r;
}
static inline long it_invoke0(long c, unsigned long l) { return it_invoke(c, l, 0, 0, 0); }
static inline long it_invoke1(long c, unsigned long l, long a1) { return it_invoke(c, l, a1, 0, 0); }
static inline long it_invoke2(long c, unsigned long l, long a1, long a2) { return it_invoke(c, l, a1, a2, 0); }
static inline uint8_t t28_pat(uint64_t i)    { return (uint8_t)((i * 31u + 7u) & 0xFFu); }
static inline uint8_t t28_pat2(uint64_t i)   { return (uint8_t)((i * 17u + 101u) & 0xFFu); }
static inline uint8_t t28_patseg(uint64_t i) { return (uint8_t)((i * 13u + 0x40u) & 0xFFu); }
static inline uint8_t t28_pats(uint64_t i)   { return (uint8_t)((i * 7u + 1u) & 0xFFu); }

#endif /* IRIS_TEST_IT_PRIV_H */
