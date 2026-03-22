#include <iris/irq_routing.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kobject.h>

#define IRQ_MAX 16
static struct KChannel *irq_table[IRQ_MAX];

void irq_routing_init(void) {
    for (int i = 0; i < IRQ_MAX; i++) irq_table[i] = 0;
}

void irq_routing_register(uint8_t irq, struct KChannel *ch) {
    if (irq >= IRQ_MAX) return;

    /* release old channel if any */
    if (irq_table[irq]) {
        kobject_release(&irq_table[irq]->base);
        irq_table[irq] = 0;
    }
    if (ch) {
        kobject_retain(&ch->base);
        irq_table[irq] = ch;
    }
}

int32_t irq_routing_signal(uint8_t irq, uint8_t data_byte) {
    if (irq >= IRQ_MAX) return -1;
    struct KChannel *ch = irq_table[irq];
    if (!ch) return -1;

    struct KChanMsg msg;
    /* zero-init */
    uint8_t *p = (uint8_t *)&msg;
    for (uint32_t i = 0; i < sizeof(msg); i++) p[i] = 0;

    msg.type     = 4;          /* IPC_MSG_SIGNAL equivalent */
    msg.data[0]  = data_byte;
    msg.data_len = 1;

    iris_error_t r = kchannel_send(ch, &msg);
    return (r == IRIS_OK) ? 0 : -1;
}
