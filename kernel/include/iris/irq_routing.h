#ifndef IRIS_IRQ_ROUTING_H
#define IRIS_IRQ_ROUTING_H

#include <stdint.h>

struct KProcess;
struct KNotification;

#define IRQ_ROUTE_MAX 16  /* maximum routable hardware IRQ lines (0..IRQ_ROUTE_MAX-1) */

/* Fase 13/Track G: IRQ routing is KNotification-only — the legacy KChannel
 * message route (IRQ_MSG_TYPE_SIGNAL / SYS_CHAN_RECV) is fully retired.  An IRQ
 * fires → knotification_signal(notif, 1<<irq); the consumer drains device state
 * via its KIoPort cap and re-arms with SYS_IRQ_ACK. */

void    irq_routing_init    (void);

/* Fase 7.6: route an IRQ to a KNotification instead of a KChannel. When the
 * IRQ fires the kernel calls knotification_signal(notif, 1u << irq) — no
 * message, no queue; the consumer drains device state via its KIoPort cap
 * and re-arms with SYS_IRQ_ACK. A route holds either a channel or a
 * notification, never both (registering one replaces the other). */
void    irq_routing_register_notification(uint8_t irq,
                                          struct KNotification *notif,
                                          struct KProcess *owner);

/*
 * irq_routing_active_count: count IRQ routing table entries with a live channel.
 *
 * Iterates irq_table[IRQ_ROUTE_MAX] and counts entries where ch != NULL.
 * Useful as a compact indicator of how many hardware IRQ lines are currently
 * routed to userland services.  Called from sys_diag_snapshot; acquires the
 * irq_lock briefly to get a consistent snapshot.
 */
uint32_t irq_routing_active_count(void);

/* irq_routing_register (KChannel) retired — Fase 13/Track G.  Routes are
 * registered via irq_routing_register_notification; teardown clears them via
 * irq_routing_unregister_owner. */

/* Called from ISR context: send a one-byte payload into the channel
 * registered for irq.  Returns 0 if sent, -1 if no channel or full. */
int32_t irq_routing_signal  (uint8_t irq, uint8_t data_byte);
void    irq_routing_unregister_owner(struct KProcess *owner);

/* Called from sys_irq_ack: unmask the hardware IRQ line so new interrupts
 * can fire.  No-op if irq >= IRQ_ROUTE_MAX.  This is the "re-enable" half of
 * the seL4-style deferred ACK: the kernel masks before signalling, ring-3
 * calls SYS_IRQ_ACK after consuming the event to unmask. */
void    irq_routing_ack     (uint8_t irq);

#endif
