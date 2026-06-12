#include <iris/irq_routing.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/spinlock.h>
#include <iris/pic.h>

struct irq_route_entry {
    struct KChannel      *ch;     /* legacy message route */
    struct KNotification *notif;  /* Fase 7.6 signal route (kbd) */
    struct KProcess      *owner;
};

static struct irq_route_entry irq_table[IRQ_ROUTE_MAX];
static spinlock_t irq_lock;

void irq_routing_init(void) {
    spinlock_init(&irq_lock);
    for (int i = 0; i < IRQ_ROUTE_MAX; i++) {
        irq_table[i].ch = 0;
        irq_table[i].notif = 0;
        irq_table[i].owner = 0;
    }
}

void irq_routing_register(uint8_t irq, struct KChannel *ch, struct KProcess *owner) {
    struct KChannel *old = 0;
    struct KNotification *old_n = 0;
    if (irq >= IRQ_ROUTE_MAX) return;
    if (ch) {
        kobject_retain(&ch->base);
        kobject_active_retain(&ch->base);
    }

    spinlock_lock(&irq_lock);
    old = irq_table[irq].ch;
    old_n = irq_table[irq].notif;
    irq_table[irq].ch = ch;
    irq_table[irq].notif = 0;
    irq_table[irq].owner = ch ? owner : 0;
    spinlock_unlock(&irq_lock);

    if (old) {
        kobject_active_release(&old->base);
        kobject_release(&old->base);
    }
    if (old_n) {
        kobject_active_release(&old_n->base);
        kobject_release(&old_n->base);
    }

    if (irq < 16)
        pic_set_irq_mask(irq, ch ? 0 : 1);
}

void irq_routing_register_notification(uint8_t irq,
                                       struct KNotification *notif,
                                       struct KProcess *owner) {
    struct KChannel *old = 0;
    struct KNotification *old_n = 0;
    if (irq >= IRQ_ROUTE_MAX) return;
    if (notif) {
        kobject_retain(&notif->base);
        kobject_active_retain(&notif->base);
    }

    spinlock_lock(&irq_lock);
    old = irq_table[irq].ch;
    old_n = irq_table[irq].notif;
    irq_table[irq].ch = 0;
    irq_table[irq].notif = notif;
    irq_table[irq].owner = notif ? owner : 0;
    spinlock_unlock(&irq_lock);

    if (old) {
        kobject_active_release(&old->base);
        kobject_release(&old->base);
    }
    if (old_n) {
        kobject_active_release(&old_n->base);
        kobject_release(&old_n->base);
    }

    if (irq < 16)
        pic_set_irq_mask(irq, notif ? 0 : 1);
}

int32_t irq_routing_signal(uint8_t irq, uint8_t data_byte) {
    struct KChannel *ch = 0;
    struct KNotification *notif = 0;
    if (irq >= IRQ_ROUTE_MAX) return -1;

    spinlock_lock(&irq_lock);
    ch = irq_table[irq].ch;
    notif = irq_table[irq].notif;
    if (ch) kobject_retain(&ch->base);
    if (notif) kobject_retain(&notif->base);
    spinlock_unlock(&irq_lock);

    if (notif) {
        /* Fase 7.6: signal-only delivery (safe from IRQ context). */
        knotification_signal(notif, (uint64_t)1u << irq);
        kobject_release(&notif->base);
        return 0;
    }

    if (!ch) return -1;

    struct KChanMsg msg;
    /* zero-init */
    uint8_t *p = (uint8_t *)&msg;
    for (uint32_t i = 0; i < sizeof(msg); i++) p[i] = 0;

    msg.type     = IRQ_MSG_TYPE_SIGNAL; /* kbd_proto.h KBD_MSG_IRQ_SCANCODE == this */
    msg.data[0]  = data_byte;
    msg.data_len = 1;

    iris_error_t r = kchannel_send(ch, &msg);
    kobject_release(&ch->base);
    return (r == IRIS_OK) ? 0 : -1;
}

uint32_t irq_routing_active_count(void) {
    uint32_t count = 0;
    spinlock_lock(&irq_lock);
    for (int i = 0; i < IRQ_ROUTE_MAX; i++) {
        if (irq_table[i].ch || irq_table[i].notif)
            count++;
    }
    spinlock_unlock(&irq_lock);
    return count;
}

void irq_routing_ack(uint8_t irq) {
    if (irq >= IRQ_ROUTE_MAX) return;
    pic_set_irq_mask(irq, 0);
}

void irq_routing_unregister_owner(struct KProcess *owner) {
    if (!owner) return;

    for (int i = 0; i < IRQ_ROUTE_MAX; i++) {
        struct KChannel *old = 0;
        struct KNotification *old_n = 0;
        spinlock_lock(&irq_lock);
        if (irq_table[i].owner == owner) {
            old = irq_table[i].ch;
            old_n = irq_table[i].notif;
            irq_table[i].ch = 0;
            irq_table[i].notif = 0;
            irq_table[i].owner = 0;
        }
        spinlock_unlock(&irq_lock);
        if (old) {
            kobject_active_release(&old->base);
            kobject_release(&old->base);
        }
        if (old_n) {
            kobject_active_release(&old_n->base);
            kobject_release(&old_n->base);
        }
        if ((old || old_n) && i < 16)
            pic_set_irq_mask((uint8_t)i, 1);
    }
}
