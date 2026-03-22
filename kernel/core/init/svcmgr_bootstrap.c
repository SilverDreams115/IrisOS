/*
 * ── Service manager bootstrap ────────────────────────────────────
 *
 * Spawns the ring-3 service manager and gives it its bootstrap
 * KChannel.  The kernel retains a reference to that channel so that
 * phase 2 can send SVCMGR_MSG_SPAWN_SERVICE requests.
 *
 * Key architectural difference from kbd_bootstrap_init():
 *   - The kernel does NOT call ns_register here.
 *   - svcmgr self-registers via SYS_NS_REGISTER after it starts.
 *   - This is the target model: services own their NS identity.
 *
 * Phase 2 migration path:
 *   1. For each compiled-in service, call svcmgr_request_spawn()
 *      instead of spawning from the kernel directly.
 *   2. svcmgr receives SVCMGR_MSG_SPAWN_SERVICE, calls SYS_SPAWN +
 *      SYS_NS_REGISTER.
 *   3. Remove kbd_bootstrap_init(); keep irq_routing_register (that
 *      stays kernel-side permanently).
 *
 * See iris/svcmgr_proto.h for the full message protocol.
 */

#include <iris/svcmgr_bootstrap.h>
#include <iris/svcmgr_proto.h>
#include <iris/serial.h>
#include <iris/task.h>
#include <iris/irq_routing.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kobject.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>

/*
 * Kernel-side reference to the svcmgr bootstrap channel.
 * Used to send SVCMGR_MSG_SPAWN_SERVICE requests.
 * Not released: this ref lives for the duration of the kernel.
 */
static struct KChannel *svcmgr_bootstrap_ch = 0;

/*
 * KProcess of the service manager, set once by svcmgr_bootstrap_init().
 * Used by sys_ns_register to restrict SYS_NS_REGISTER to svcmgr only.
 * See svcmgr_bootstrap.h for the authority model contract.
 */
static struct KProcess *svcmgr_proc = 0;

struct KProcess *svcmgr_get_process(void) {
    return svcmgr_proc;
}

void svcmgr_bootstrap_init(void) {
    struct KChannel *ch = kchannel_alloc();
    if (!ch) {
        serial_write("[IRIS][SVCMGR] WARN: bootstrap channel alloc failed\n");
        return;
    }

    extern void svcmgr_main(void);
    struct task *sm = task_spawn_user((uint64_t)(uintptr_t)svcmgr_main, 0);
    if (!sm) {
        serial_write("[IRIS][SVCMGR] WARN: spawn failed\n");
        kobject_release(&ch->base);
        return;
    }

    handle_id_t h = handle_table_insert(&sm->process->handle_table,
                                        &ch->base,
                                        RIGHT_READ | RIGHT_WRITE);
    task_set_bootstrap_arg0(sm, (uint64_t)h);

    /*
     * Retain the kernel-side reference for spawn requests.
     * kobject_retain was already done by handle_table_insert; we keep
     * one additional reference here for the kernel's own use.
     */
    kobject_retain(&ch->base);
    svcmgr_bootstrap_ch = ch;

    /*
     * Record svcmgr's KProcess for NS authority enforcement.
     * sys_ns_register will reject callers whose process != svcmgr_proc.
     * Set before any task_yield so it is visible before svcmgr runs.
     */
    svcmgr_proc = sm->process;

    serial_write("[IRIS][SVCMGR] service manager spawned, id=");
    serial_write_dec(sm->id);
    serial_write(", bootstrap_handle=");
    serial_write_dec((uint64_t)h);
    serial_write("\n");

    /*
     * No ns_register call here.  svcmgr registers itself as "svcmgr"
     * via SYS_NS_REGISTER once it starts running.  The kernel is not
     * the registrar for the service manager.
     */

    /* ── Queue kbd spawn request for svcmgr ─────────────────────────────
     *
     * kbd channels:
     *   kbd_irq_ch  — receives IRQ 1 hardware events; also the public
     *                 service inbox (clients send HELLO/STATUS here).
     *                 The kernel pre-wires IRQ routing, then gives svcmgr
     *                 a handle so svcmgr can register it as "kbd".
     *
     *   reply channel — svcmgr creates this via SYS_CHAN_CREATE and
     *                   registers it as "kbd.reply".  The kernel does NOT
     *                   pre-register it; svcmgr is the registrar.  Because
     *                   svcmgr registers "kbd.reply" before calling
     *                   SYS_SPAWN (and before yielding), the entry is
     *                   visible to user_init without any retry.
     *
     * IRQ routing ownership:
     *   owner = sm->process (svcmgr).  svcmgr arranged the routing and
     *   is the responsible supervisor in this transitional phase.  When
     *   svcmgr exits, kprocess_teardown() calls irq_routing_unregister_owner
     *   and the IRQ 1 route is cleared automatically.
     *
     *   Transitional note: the ideal final owner is kbd_server's KProcess.
     *   That would require a syscall to update the owner after svcmgr
     *   spawns kbd_server (SYS_IRQ_ROUTE_REGISTER or similar).  Deferred
     *   to the lifecycle/restart phase; svcmgr as owner is correct for now.
     */
    extern void kbd_server(void);
    struct KChannel *kbd_irq_ch = kchannel_alloc();
    if (!kbd_irq_ch) {
        serial_write("[IRIS][SVCMGR] WARN: kbd channel alloc failed\n");
    } else {
        irq_routing_register(1, kbd_irq_ch, sm->process);

        handle_id_t irq_chan_h = handle_table_insert(&sm->process->handle_table,
                                                      &kbd_irq_ch->base,
                                                      RIGHT_READ | RIGHT_WRITE);
        if (irq_chan_h == HANDLE_INVALID) {
            serial_write("[IRIS][SVCMGR] WARN: kbd.irq handle insert failed\n");
            kobject_release(&kbd_irq_ch->base);
        } else {
            struct KChanMsg msg;
            uint32_t i;
            for (i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
            msg.type = SVCMGR_MSG_SPAWN_SERVICE;

            /* entry_vaddr at data[SVCMGR_SPAWN_OFF_ENTRY] */
            uint64_t kbd_entry = (uint64_t)(uintptr_t)kbd_server;
            for (i = 0; i < 8; i++)
                msg.data[SVCMGR_SPAWN_OFF_ENTRY + i] = ((uint8_t *)&kbd_entry)[i];

            /* service name "kbd\0" at data[SVCMGR_SPAWN_OFF_NAME] */
            msg.data[SVCMGR_SPAWN_OFF_NAME + 0] = 'k';
            msg.data[SVCMGR_SPAWN_OFF_NAME + 1] = 'b';
            msg.data[SVCMGR_SPAWN_OFF_NAME + 2] = 'd';
            msg.data[SVCMGR_SPAWN_OFF_NAME + 3] = 0;

            /* rights = RIGHT_READ|RIGHT_WRITE: clients send (WRITE),
             * kbd_server recvs (READ), irq routing sends (WRITE).     */
            uint32_t rights = RIGHT_READ | RIGHT_WRITE;
            for (i = 0; i < 4; i++)
                msg.data[SVCMGR_SPAWN_OFF_RIGHTS + i] = ((uint8_t *)&rights)[i];

            /* pre-created irq/service channel handle */
            for (i = 0; i < 4; i++)
                msg.data[SVCMGR_SPAWN_OFF_REG_CHAN + i] = ((uint8_t *)&irq_chan_h)[i];

            /* reply channel name: svcmgr creates and registers this */
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 0] = 'k';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 1] = 'b';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 2] = 'd';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 3] = '.';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 4] = 'r';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 5] = 'e';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 6] = 'p';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 7] = 'l';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 8] = 'y';
            msg.data[SVCMGR_SPAWN_OFF_REPLY_NAME + 9] = 0;

            msg.data_len = SVCMGR_SPAWN_MSG_LEN;

            iris_error_t sr = kchannel_send(svcmgr_bootstrap_ch, &msg);
            if (sr == IRIS_OK) {
                serial_write("[IRIS][SVCMGR] kbd spawn request queued\n");
            } else {
                serial_write("[IRIS][SVCMGR] WARN: kbd spawn request send failed\n");
            }

            kobject_release(&kbd_irq_ch->base);  /* svcmgr table + irq_routing hold refs */
        }
    }

    kobject_release(&ch->base);  /* release local alloc ref; kernel ref above + table ref remain */
}
