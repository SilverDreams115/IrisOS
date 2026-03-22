#include <iris/irq_routing.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/spinlock.h>

#define IRQ_MAX 16
struct irq_route_entry {
    struct KChannel *ch;
    struct KProcess *owner;
};

static struct irq_route_entry irq_table[IRQ_MAX];
static spinlock_t irq_lock;

void irq_routing_init(void) {
    spinlock_init(&irq_lock);
    for (int i = 0; i < IRQ_MAX; i++) {
        irq_table[i].ch = 0;
        irq_table[i].owner = 0;
    }
}

void irq_routing_register(uint8_t irq, struct KChannel *ch, struct KProcess *owner) {
    struct KChannel *old = 0;
    if (irq >= IRQ_MAX) return;
    if (ch) {
        kobject_retain(&ch->base);
        kobject_active_retain(&ch->base);
    }

    spinlock_lock(&irq_lock);
    old = irq_table[irq].ch;
    irq_table[irq].ch = ch;
    irq_table[irq].owner = ch ? owner : 0;
    spinlock_unlock(&irq_lock);

    if (old) {
        kobject_active_release(&old->base);
        kobject_release(&old->base);
    }
}

int32_t irq_routing_signal(uint8_t irq, uint8_t data_byte) {
    struct KChannel *ch = 0;
    if (irq >= IRQ_MAX) return -1;

    spinlock_lock(&irq_lock);
    ch = irq_table[irq].ch;
    if (ch) {
        kobject_retain(&ch->base);
    }
    spinlock_unlock(&irq_lock);

    if (!ch) return -1;

    struct KChanMsg msg;
    /* zero-init */
    uint8_t *p = (uint8_t *)&msg;
    for (uint32_t i = 0; i < sizeof(msg); i++) p[i] = 0;

    msg.type     = 4;          /* IPC_MSG_SIGNAL equivalent */
    msg.data[0]  = data_byte;
    msg.data_len = 1;

    iris_error_t r = kchannel_send(ch, &msg);
    kobject_release(&ch->base);
    return (r == IRIS_OK) ? 0 : -1;
}

void irq_routing_unregister_owner(struct KProcess *owner) {
    if (!owner) return;

    for (int i = 0; i < IRQ_MAX; i++) {
        struct KChannel *old = 0;
        spinlock_lock(&irq_lock);
        if (irq_table[i].owner == owner) {
            old = irq_table[i].ch;
            irq_table[i].ch = 0;
            irq_table[i].owner = 0;
        }
        spinlock_unlock(&irq_lock);
        if (old) {
            kobject_active_release(&old->base);
            kobject_release(&old->base);
        }
    }
}
