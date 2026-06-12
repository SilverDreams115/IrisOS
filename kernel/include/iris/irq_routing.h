#ifndef IRIS_IRQ_ROUTING_H
#define IRIS_IRQ_ROUTING_H

#include <stdint.h>
#include <iris/nc/kchannel.h>

struct KProcess;
struct KNotification;

#define IRQ_ROUTE_MAX 16  /* maximum routable hardware IRQ lines (0..IRQ_ROUTE_MAX-1) */

/* Message type placed in KChanMsg.type for all routed IRQ notifications.
 * Each service that consumes a routed IRQ must recognise this opcode.
 * kbd_proto.h defines KBD_MSG_IRQ_SCANCODE with the same value for the
 * keyboard service; the two must stay in sync.                        */
#define IRQ_MSG_TYPE_SIGNAL  0x4u

/* Maps hardware IRQ lines (0-IRQ_ROUTE_MAX-1) to KChannel objects.
 * When a registered IRQ fires, the kernel sends a KChanMsg with
 * type = IRQ_MSG_TYPE_SIGNAL into the channel. The data_byte payload
 * is 0 — the handler process is responsible for reading hardware
 * registers directly via its KIoPort cap. User-space servers receive
 * via SYS_CHAN_RECV.                                                   */

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

/* Called from sys_irq_ack: unmask the hardware IRQ line so new interrupts
 * can fire.  No-op if irq >= IRQ_ROUTE_MAX.  This is the "re-enable" half of
 * the seL4-style deferred ACK: the kernel masks before signalling, ring-3
 * calls SYS_IRQ_ACK after consuming the event to unmask. */
void    irq_routing_ack     (uint8_t irq);

#endif
