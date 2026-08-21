#include <iris/irq_routing.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/pic.h>

/* Phase 13/Track G: IRQ routing is KNotification-only — the legacy KChannel
 * message route is fully retired (KChannel is no longer an IPC mechanism). */
/*
 * Stage 7-mem: a route has no OWNER field any more.
 *
 * It used to name the KProcess that registered it, for exactly one purpose:
 * so process teardown could sweep the routes that process had installed.  That
 * is the kernel keeping a back-reference to a resource domain in order to
 * clean up after it — and the domain is going away.
 *
 * The seL4 shape is that the binding belongs to the NOTIFICATION, and deleting
 * the notification unbinds the interrupt (`seL4_IRQHandler_Clear` is the
 * explicit form; dropping the capability is the implicit one).  So the route
 * holds a LIFECYCLE reference only — enough to keep the pointer valid for an
 * interrupt that is already in flight — and no ACTIVE one, which is what lets
 * the notification's close hook fire when the last CSpace slot holding it goes.
 * That hook clears the route.  A driver that drops its notification stops
 * receiving its interrupt, with nobody having to remember who registered it.
 */
struct irq_route_entry {
    struct KNotification *notif;  /* signal route (kbd, Phase 7.6) */
};

static struct irq_route_entry irq_table[IRQ_ROUTE_MAX];
static spinlock_t irq_lock;

void irq_routing_init(void) {
    spinlock_init(&irq_lock);
    for (int i = 0; i < IRQ_ROUTE_MAX; i++) {
        irq_table[i].notif = 0;
    }
}

void irq_routing_register_notification(uint8_t irq,
                                       struct KNotification *notif) {
    struct KNotification *old_n = 0;
    if (irq >= IRQ_ROUTE_MAX) return;
    /* Lifecycle reference only — see the note on struct irq_route_entry. */
    if (notif) kobject_retain(&notif->base);

    spinlock_lock(&irq_lock);
    old_n = irq_table[irq].notif;
    irq_table[irq].notif = notif;
    spinlock_unlock(&irq_lock);

    if (old_n) {
        kobject_release(&old_n->base);
    }

    if (irq < 16)
        pic_set_irq_mask(irq, notif ? 0 : 1);
}

int32_t irq_routing_signal(uint8_t irq, uint8_t data_byte) {
    struct KNotification *notif = 0;
    (void)data_byte;  /* signal delivery carries no payload */
    if (irq >= IRQ_ROUTE_MAX) return -1;

    spinlock_lock(&irq_lock);
    notif = irq_table[irq].notif;
    if (notif) kobject_retain(&notif->base);
    spinlock_unlock(&irq_lock);

    if (!notif) return -1;

    /* Phase 7.6: signal-only delivery (safe from IRQ context). */
    knotification_signal(notif, (uint64_t)1u << irq);
    kobject_release(&notif->base);
    return 0;
}

uint32_t irq_routing_active_count(void) {
    uint32_t count = 0;
    spinlock_lock(&irq_lock);
    for (int i = 0; i < IRQ_ROUTE_MAX; i++) {
        if (irq_table[i].notif)
            count++;
    }
    spinlock_unlock(&irq_lock);
    return count;
}

void irq_routing_ack(uint8_t irq) {
    if (irq >= IRQ_ROUTE_MAX) return;
    pic_set_irq_mask(irq, 0);
}

/*
 * Clear every route bound to this notification.  Called from the
 * notification's CLOSE hook — the moment its last CSpace slot goes — so an
 * interrupt stops being delivered to an object nobody can reach.
 */
void irq_routing_unregister_notification(struct KNotification *n) {
    if (!n) return;

    for (int i = 0; i < IRQ_ROUTE_MAX; i++) {
        struct KNotification *old_n = 0;
        spinlock_lock(&irq_lock);
        if (irq_table[i].notif == n) {
            old_n = irq_table[i].notif;
            irq_table[i].notif = 0;
        }
        spinlock_unlock(&irq_lock);
        if (old_n) kobject_release(&old_n->base);
        if (old_n && i < 16)
            pic_set_irq_mask((uint8_t)i, 1);
    }
}
