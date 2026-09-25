/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_TIMER_PROTO_H
#define IRIS_TIMER_PROTO_H

/*
 * timer_proto.h — the wire contract of the IRIS timer service (ledger A-24).
 *
 * The kernel has no way to block a thread on TIME.  `SYS_SLEEP`,
 * `SYS_CLOCK_NANOSLEEP` and `SYS_NOTIFY_WAIT_TIMEOUT` are retired: a kernel
 * that can block on time owns a policy about time — how long a thread may
 * wait, whose waiting is worth a kernel data structure, what happens when the
 * deadline passes — and none of those are decisions a microkernel should be
 * making on somebody's behalf.  seL4 has no timed blocking for the same
 * reason.
 *
 * So waiting is a SERVICE.  This one holds the timer interrupt and a clock,
 * takes requests over an endpoint, and signals a notification the requester
 * gave it when the deadline passes.  A client that wants to wait therefore
 * waits the way it waits for anything else: on a notification somebody
 * signals.  What used to be a syscall is a capability to a server.
 *
 * The manifest the supervisor mints (nothing else is held):
 *   slot TMR_SLOT_CTRL_EP   the control endpoint it serves          (READ)
 *   slot TMR_SLOT_IRQ_CAP   the timer IRQ capability                (ROUTE)
 *   slot TMR_SLOT_IRQ_NOTIF the notification that IRQ is routed into, BOUND to
 *                           this service's thread so one thread can take both
 *                           ticks and requests (ledger A-23)        (WRITE)
 *   slot TMR_SLOT_REPLY     its reply object                        (WRITE)
 *   slot IRIS_CPTR_OWN_UNTYPED  the budget its receive slots are carved from
 *
 * No spawn cap, no ioport, no VFS, no debug authority, no address space but
 * its own.  A timer service that can only measure time is the point.
 */

/* ── request ─────────────────────────────────────────────────────────────
 * label      = TMR_OP_ARM
 * words[0]   = delay in NANOSECONDS from now (0 = fire at the next tick)
 * words[1]   = the signal bits to raise when it expires
 * attached_cap = the notification to signal, transferred to the service
 *
 * reply words[0] = 0 on success, or a TMR_ERR_* marker.
 * reply words[1] = an opaque TOKEN naming this timer, for TMR_OP_CANCEL.
 *
 * The notification travels as a CAPABILITY rather than a name because that is
 * the whole authority the service needs and the whole authority it gets: it
 * can signal what it was handed, and it cannot name anything else.
 */
#define TMR_OP_ARM        0x544D5201ull   /* 'TMR' 1 */

/* ── cancel ──────────────────────────────────────────────────────────────
 * label    = TMR_OP_CANCEL
 * words[0] = the token the ARM reply returned
 *
 * A bounded wait that ends EARLY leaves a timer armed, and a service that
 * never hears about it holds one table entry and one capability per abandoned
 * wait until the deadline — which is a leak proportional to how often anybody
 * changes their mind.  So a client can take it back.
 *
 * Whose timer it is, is decided by the BADGE on the capability the request
 * arrived through, not by the token: a token is a number, and a number is not
 * authority.  Cancelling somebody else's timeout would be a small thing to be
 * able to do and there is no reason to be able to do it.
 */
#define TMR_OP_CANCEL     0x544D5202ull   /* 'TMR' 2 */

/* ── uptime ──────────────────────────────────────────────────────────────
 * label    = TMR_OP_UPTIME
 * reply words[0] = nanoseconds since this service started counting
 *
 * A client that was granted a clock should be able to ASK it, rather than
 * reaching around it to the kernel.  That is the whole of this operation: it
 * answers with the same monotonic nanoseconds `SYS_CLOCK_GET` reports, from the
 * task that owns the timer line.
 *
 * Ledger A-27 records why the syscall itself stayed: on x86 `rdtsc` is an
 * unprivileged instruction, so a monotonic read cannot be gated by anything —
 * retiring the syscall would have moved the same ungated read into an
 * instruction.  What is gateable is WAITING, and A-24 gated it.
 */
#define TMR_OP_UPTIME     0x544D5203ull   /* 'TMR' 3 */

#define TMR_ERR_NOTYOURS  4u

#define TMR_ERR_FULL      1u   /* no free timer slot */
#define TMR_ERR_NOCAP     2u   /* no notification was transferred */
#define TMR_ERR_BADOP     3u

/* ── the service's own CSpace map ─────────────────────────────────────── */
#define TMR_SLOT_CTRL_EP    3u
#define TMR_SLOT_IRQ_CAP    11u   /* == IRIS_CPTR_IRQ_CAP, the usual place */
#define TMR_SLOT_IRQ_NOTIF  7u    /* == IRIS_CPTR_IRQ_NOTIFY */
#define TMR_SLOT_REPLY      13u   /* == IRIS_CPTR_OWN_REPLY */

/*
 * The armed timers.  A fixed table because the service holds one CAPABILITY
 * per armed timer and its CSpace is a fixed size — the ceiling is a property
 * of the memory its supervisor gave it, which is the honest place for it.
 */
#define TMR_MAX_TIMERS      64u
/* The CNode is a radix level: its slot count is a power of two, and leaf 0 is
 * the guard slot every CNode in this system keeps empty. */
#define TMR_CN_SLOTS        128u
#define TMR_SLOT_CN         20u   /* CNode of client notification capabilities */
#define TMR_CLIENT_CPTR(i)  ((long)((((uint64_t)(i) + 1u) << 8) | TMR_SLOT_CN))

#endif /* IRIS_TIMER_PROTO_H */
