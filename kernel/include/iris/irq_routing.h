#ifndef IRIS_IRQ_ROUTING_H
#define IRIS_IRQ_ROUTING_H

#include <stdint.h>
#include <iris/nc/kchannel.h>

struct KProcess;

/* Maps hardware IRQ lines (0-15) to KChannel objects.
 * When a registered IRQ fires, the kernel sends a KChanMsg with
 * data[0] = scancode/byte into the channel instead of invoking the
 * in-kernel driver.  User-space servers receive via SYS_CHAN_RECV. */

void    irq_routing_init    (void);

/* Register a KChannel for the given IRQ.  The routing table retains
 * a reference to ch (kobject_retain).  Pass NULL to unregister. */
void    irq_routing_register(uint8_t irq, struct KChannel *ch, struct KProcess *owner);

/* Called from ISR context: send a one-byte payload into the channel
 * registered for irq.  Returns 0 if sent, -1 if no channel or full. */
int32_t irq_routing_signal  (uint8_t irq, uint8_t data_byte);
void    irq_routing_unregister_owner(struct KProcess *owner);

#endif
