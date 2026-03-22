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

/*
 * Register a KChannel for the given IRQ.
 *
 * Ownership contract:
 *   - The routing table retains a KObject reference to ch.
 *   - owner is the KProcess responsible for this route.  When owner
 *     undergoes kprocess_teardown(), irq_routing_unregister_owner(owner)
 *     is called automatically and the route is cleared.
 *   - Pass ch=NULL to unregister unconditionally (clears owner too).
 *   - Pass owner=NULL only if the route is expected to live for the
 *     duration of the kernel (no process-scoped cleanup required).
 *     This should be the exception, not the norm.
 */
void    irq_routing_register(uint8_t irq, struct KChannel *ch, struct KProcess *owner);

/* Called from ISR context: send a one-byte payload into the channel
 * registered for irq.  Returns 0 if sent, -1 if no channel or full. */
int32_t irq_routing_signal  (uint8_t irq, uint8_t data_byte);
void    irq_routing_unregister_owner(struct KProcess *owner);

#endif
