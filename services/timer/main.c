/*
 * timer/main.c — the IRIS timer service (ledger A-24).
 *
 * The kernel used to be able to block a thread on TIME: `SYS_SLEEP`,
 * `SYS_CLOCK_NANOSLEEP` and `SYS_NOTIFY_WAIT_TIMEOUT` each parked a thread
 * with a deadline and the scheduler woke it.  That is a policy about time
 * living in the kernel — how long a thread may wait, whose waiting is worth a
 * kernel data structure, what "the deadline passed" means — and seL4 has none
 * of it for exactly that reason.
 *
 * So waiting became a service, and this is it.  It holds the timer interrupt,
 * takes "signal this notification in N nanoseconds" over an endpoint, and
 * signals.  A client waits the way it waits for anything else: on a
 * notification.  What used to be a syscall is a capability to a server, which
 * means it can be delegated, revoked, replaced by a different implementation,
 * or simply not granted — none of which was true of a syscall number.
 *
 * ONE THREAD, because ledger A-23 made that possible.  A driver has to take
 * both an interrupt and a request queue, and a thread blocked receiving on an
 * endpoint used to be deaf to signals — so this service could not have been
 * written single-threaded before the bound notification existed.  It binds its
 * IRQ notification to its own thread and receives everything on one syscall,
 * telling the two apart by the message label.
 */
#include <stdint.h>
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include "timer_proto.h"

static inline long tm_sys1(long nr, long a0) {
    return iris_syscall4(nr, a0, 0L, 0L, 0);
}
static void tm_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}

/*
 * One armed timer.  `used` is the only liveness there is: the capability in
 * TMR_CLIENT_CPTR(i) is deleted the moment the timer fires or is displaced, so
 * the service never holds authority over a client it is no longer serving.
 */
struct tm_entry {
    uint64_t deadline_ns;
    uint64_t bits;
    /* Whose timer this is: the badge on the capability the ARM arrived
     * through.  Cancelling checks it, so a token is only ever a way to say
     * WHICH of your own timers you mean. */
    uint64_t owner_badge;
    uint32_t generation;
    uint8_t  used;
};

static struct tm_entry g_timers[TMR_MAX_TIMERS];

/*
 * How many ticks this service has been WOKEN for — a diagnostic, and not a
 * clock.
 *
 * Ledger A-27 tried to make it one, on the reasoning that a driver receiving
 * every tick of a line does not need to be told the time.  It does: a
 * notification carries BITS, not a count, so ticks arriving while the service
 * is not scheduled COALESCE into one wake-up.  Counting wake-ups is a clock
 * that runs slow exactly when the system is busy, which is when a deadline
 * matters most — and the suite took five times as long to run before the cause
 * was obvious.
 *
 * Deadlines are in nanoseconds from `SYS_CLOCK_GET`, which is monotonic and
 * cannot be missed.  A-27 records why that syscall stayed.
 */
static uint64_t g_wakes;

/* Diagnostics, readable only through what the service reports; it holds no
 * debug authority of its own. */
static uint32_t g_armed, g_fired;
/* silence the unused warning on a counter kept for its documentation value */
#define TM_TOUCH_WAKES() ((void)g_wakes)

/*
 * (slot + 1) | generation, so a token that outlived its timer names nothing —
 * and so that a valid token is never ZERO.  The first draft used the raw slot
 * index: arming into slot 0 with generation 0 produced token 0, which the
 * cleanup below reads as "nothing was armed" and promptly deleted the client's
 * notification out from under a timer that then fired into an empty slot.  The
 * first bounded wait in the suite hung, which is the correct amount of
 * noticing for a bug that silent.
 */
static uint64_t tm_token(uint32_t slot) {
    return (uint64_t)(slot + 1u) | ((uint64_t)g_timers[slot].generation << 32);
}

static uint32_t tm_free_slot(void) {
    for (uint32_t i = 0; i < TMR_MAX_TIMERS; i++)
        if (!g_timers[i].used) return i;
    return TMR_MAX_TIMERS;
}

/* A tick: everything whose deadline has passed is signalled and released. */
static void tm_tick(void) {
    long now = tm_sys1(SYS_CLOCK_GET, 0);
    if (now < 0) return;
    g_wakes++;
    for (uint32_t i = 0; i < TMR_MAX_TIMERS; i++) {
        if (!g_timers[i].used) continue;
        if ((uint64_t)now < g_timers[i].deadline_ns) continue;
        (void)iris_invoke1(TMR_CLIENT_CPTR(i), INV_NOTIFY_SIGNAL, (long)g_timers[i].bits);
        /* The grant ends with the deadline: the capability goes back before
         * the slot is reused, so a client cannot be signalled by a timer it
         * did not arm. */
        (void)iris_invoke1((long)TMR_SLOT_CN, INV_CNODE_DELETE, (long)(i + 1u));
        g_timers[i].used = 0;
        g_timers[i].generation++;
        g_fired++;
    }
}

void timer_main(handle_id_t bootstrap_ch_h);
void timer_main(handle_id_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    /*
     * Bind the IRQ notification to this thread.  Without it (ledger A-23) a
     * receive on the control endpoint would make this service deaf to its own
     * interrupt, and the whole design would need a second thread to hold the
     * two halves apart.
     */
    (void)iris_invoke1((long)IRIS_CPTR_OWN_TCB, INV_TCB_BIND_NOTIFICATION, (long)TMR_SLOT_IRQ_NOTIF);

    for (;;) {
        uint32_t slot = tm_free_slot();
        struct iris_msg m;
        tm_msg_zero(&m);
        /* Declare where a transferred notification should land.  When the
         * table is full there is no slot to declare, and an ARM that arrives
         * anyway is refused with its capability undelivered. */
        m.recv_slot = (slot < TMR_MAX_TIMERS)
                    ? (long)TMR_CLIENT_CPTR(slot) : 0;
        m.reply     = (long)TMR_SLOT_REPLY;

        long r = iris_msg_recv((long)TMR_SLOT_CTRL_EP, &m);
        if (r != 0) continue;

        if (m.label == IRIS_MSG_LABEL_NOTIFICATION) {
            /* The interrupt.  No caller, so nothing is owed a reply. */
            tm_tick();
            (void)iris_invoke0((long)TMR_SLOT_IRQ_CAP, INV_IRQ_ACK);
            continue;
        }

        uint32_t err = 0u;
        uint64_t token = 0u;
        uint64_t uptime = 0u;
        if (m.label == TMR_OP_UPTIME) {
            long now = tm_sys1(SYS_CLOCK_GET, 0);
            uptime = (now < 0) ? 0u : (uint64_t)now;
        } else if (m.label == TMR_OP_CANCEL) {
            uint32_t k1  = (uint32_t)(m.words[0] & 0xFFFFFFFFu);
            uint32_t k   = k1 - 1u;
            uint32_t gen = (uint32_t)(m.words[0] >> 32);
            if (k1 == 0u || k >= TMR_MAX_TIMERS || !g_timers[k].used ||
                g_timers[k].generation != gen) {
                err = TMR_ERR_BADOP;          /* already fired, or never was */
            } else if (g_timers[k].owner_badge != m.sender_badge) {
                err = TMR_ERR_NOTYOURS;       /* somebody else's wait */
            } else {
                (void)iris_invoke1((long)TMR_SLOT_CN, INV_CNODE_DELETE, (long)(k + 1u));
                g_timers[k].used = 0;
                g_timers[k].generation++;
            }
        } else if (m.label != TMR_OP_ARM) {
            err = TMR_ERR_BADOP;
        } else if (slot >= TMR_MAX_TIMERS) {
            err = TMR_ERR_FULL;
        } else if (m.got_caps == 0u) {
            /* A-33: the MessageInfo says whether a capability landed in the
             * slot this receive declared — seL4's `extraCaps`.  `got_cap` is
             * the reply object, which is a different question. */
            err = TMR_ERR_NOCAP;
        } else {
            long now = tm_sys1(SYS_CLOCK_GET, 0);
            if (now < 0) now = 0;
            g_timers[slot].deadline_ns = (uint64_t)now + m.words[0];
            g_timers[slot].bits        = m.words[1] ? m.words[1] : 1ull;
            g_timers[slot].owner_badge = m.sender_badge;
            g_timers[slot].used        = 1;
            token = tm_token(slot);
            g_armed++;
        }

        struct iris_msg rep;
        tm_msg_zero(&rep);
        rep.words[0]   = (m.label == TMR_OP_UPTIME) ? uptime : (uint64_t)err;
        rep.words[1]   = token;
        rep.word_count = 2u;
        (void)iris_msg_reply((long)TMR_SLOT_REPLY, &rep);

        /* A refused or non-ARM request must not leave a capability in the slot
         * the next request will declare. */
        if (token == 0u && slot < TMR_MAX_TIMERS)
            (void)iris_invoke1((long)TMR_SLOT_CN, INV_CNODE_DELETE, (long)(slot + 1u));
    }
}
