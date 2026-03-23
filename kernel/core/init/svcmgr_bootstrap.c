/*
 * ── Service manager bootstrap ────────────────────────────────────
 *
 * Spawns the ring-3 service manager and gives it its bootstrap
 * KChannel.  The kernel retains a reference to that channel so that
 * it can queue early spawn requests and install one narrow bootstrap
 * client handle for the first user task that must reach svcmgr.
 *
 * Key architectural difference from kbd_bootstrap_init():
 *   - The kernel does NOT call ns_register here.
 *   - svcmgr self-bootstrap is explicit via task arg0, not via lookup.
 *   - Later userland clients should also prefer explicit bootstrap
 *     handles over kernel nameserver lookup for the first control path.
 *
 * Phase 2 migration path:
 *   1. For each compiled-in service, call svcmgr_request_spawn()
 *      instead of spawning from the kernel directly.
 *   2. svcmgr receives SVCMGR_MSG_SPAWN_SERVICE, calls SYS_SPAWN,
 *      bootstraps the child with attached-handle IPC, and keeps
 *      normal discovery authority in userland.
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
 * Used to send SVCMGR_MSG_SPAWN_SERVICE requests and to install the
 * first client bootstrap handle into user_init.
 * Not released: this ref lives for the duration of the kernel.
 */
static struct KChannel *svcmgr_bootstrap_ch = 0;

/*
 * NS authority is no longer tracked via a module-global pointer.
 * Authority is granted by setting KProcess.ns_authority = 1 on the
 * svcmgr process via kprocess_set_ns_authority().
 * sys_ns_register checks kprocess_has_ns_authority(t->process) directly.
 * svcmgr_get_process() has been removed from the public interface.
 */

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
    if (h == HANDLE_INVALID) {
        serial_write("[IRIS][SVCMGR] FATAL: bootstrap handle insert failed — aborting spawn\n");
        task_abort_spawned_user(sm);
        kobject_release(&ch->base);
        return;
    }
    task_set_bootstrap_arg0(sm, (uint64_t)h);

    /*
     * Retain the kernel-side reference for spawn requests.
     * kobject_retain was already done by handle_table_insert; we keep
     * one additional reference here for the kernel's own use.
     */
    kobject_retain(&ch->base);
    svcmgr_bootstrap_ch = ch;

    /*
     * Grant NS registration authority to svcmgr's KProcess.
     * sys_ns_register calls kprocess_has_ns_authority(t->process).
     * Set before any task_yield so it is visible before svcmgr runs.
     * Authority is a property of the KProcess object, not of a global
     * pointer — no stale-reference risk if svcmgr is ever restarted.
     */
    kprocess_set_ns_authority(sm->process);

    serial_write("[IRIS][SVCMGR] service manager spawned, id=");
    serial_write_dec(sm->id);
    serial_write(", bootstrap_handle=");
    serial_write_dec((uint64_t)h);
    serial_write("\n");

    /*
     * No ns_register call here.  Healthy boot now reaches svcmgr through
     * explicit bootstrap handle delivery, not through kernel nameserver
     * publication of "svcmgr".
     */

    /* ── Queue kbd spawn request for svcmgr ─────────────────────────────
     *
     * kbd channels:
     *   kbd_irq_ch  — receives IRQ 1 hardware events; also the public
     *                 service inbox (clients send HELLO/STATUS here).
     *                 The kernel pre-wires IRQ routing, then gives svcmgr
     *                 one master handle with duplicate+transfer rights.
     *                 svcmgr keeps that master handle and becomes the
     *                 normal discovery authority for clients and children.
     *
     *   reply channel — svcmgr creates this in userland and retains the
     *                   master handle itself.  The kernel does not publish
     *                   or name it.
     *
     * IRQ routing ownership:
     *   owner = sm->process (svcmgr) only for the pre-spawn bootstrap window.
     *   svcmgr later calls SYS_IRQ_ROUTE_REGISTER(1, irq_chan_h, child_proc_h)
     *   after the child exists, so steady-state ownership lives on the
     *   kbd_server KProcess.  If that service exits, kprocess_teardown()
     *   calls irq_routing_unregister_owner(child) and IRQ 1 is cleared.
     *
     *   This bootstrap ownership handoff keeps the route valid before the
     *   child process exists without leaving svcmgr as the steady-state owner.
     */
    extern void phase3_exit_child(void);
    struct KChannel *kbd_irq_ch = kchannel_alloc();
    if (!kbd_irq_ch) {
        serial_write("[IRIS][SVCMGR] WARN: kbd channel alloc failed\n");
    } else {
        irq_routing_register(1, kbd_irq_ch, sm->process);

        handle_id_t irq_chan_h = handle_table_insert(&sm->process->handle_table,
                                                      &kbd_irq_ch->base,
                                                      RIGHT_READ | RIGHT_WRITE |
                                                      RIGHT_DUPLICATE | RIGHT_TRANSFER);
        if (irq_chan_h == HANDLE_INVALID) {
            serial_write("[IRIS][SVCMGR] WARN: kbd.irq handle insert failed\n");
            kobject_release(&kbd_irq_ch->base);
        } else {
            struct KChanMsg msg;
            uint32_t i;
            for (i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
            msg.type = SVCMGR_MSG_SPAWN_SERVICE;

            uint32_t service_id = SVCMGR_SERVICE_KBD;
            for (i = 0; i < 4; i++)
                msg.data[SVCMGR_SPAWN_OFF_SERVICE_ID + i] = ((uint8_t *)&service_id)[i];

            /* pre-created public service channel handle retained by svcmgr */
            for (i = 0; i < 4; i++)
                msg.data[SVCMGR_SPAWN_OFF_REG_CHAN + i] = ((uint8_t *)&irq_chan_h)[i];

            /* IRQ line: 1 = PS/2 keyboard.  svcmgr transfers ownership to the
             * spawned kbd_server KProcess after spawn via SYS_IRQ_ROUTE_REGISTER. */
            msg.data[SVCMGR_SPAWN_OFF_IRQ] = 1;

            msg.data_len = SVCMGR_SPAWN_MSG_LEN;

            iris_error_t sr = kchannel_send(svcmgr_bootstrap_ch, &msg);
            if (sr == IRIS_OK) {
                serial_write("[IRIS][SVCMGR] kbd bootstrap request queued\n");
            } else {
                serial_write("[IRIS][SVCMGR] WARN: kbd spawn request send failed\n");
            }

            kobject_release(&kbd_irq_ch->base);  /* svcmgr table + irq_routing hold refs */
        }
    }

    /* ── Queue vfs bootstrap request for svcmgr ─────────────────────────────
     *
     * Current staged extraction step:
     *   The kernel still owns the in-memory ramfs backing store and the
     *   legacy open/read/close syscall island, but it is no longer the
     *   client-visible owner of the migrated read-only open/read/close path.
     *   svcmgr spawns a ring-3 "vfs" service that owns the file_id/session
     *   namespace exposed to clients for that path.
     *
     * Bootstrap resources:
     *   vfs_ch — public request channel inserted into svcmgr's table here.
     *            svcmgr retains it as the master public handle and returns
     *            duplicates to clients over attached-handle IPC.
     *
     * Transitional boundary:
     *   No IRQ route is involved.  The legacy syscall island remains the
     *   kernel-resident backend/compatibility path for now.
     */
    {
        struct KChannel *vfs_ch = kchannel_alloc();
        if (!vfs_ch) {
            serial_write("[IRIS][SVCMGR] WARN: vfs channel alloc failed\n");
        } else {
            handle_id_t vfs_chan_h = handle_table_insert(&sm->process->handle_table,
                                                         &vfs_ch->base,
                                                         RIGHT_READ | RIGHT_WRITE |
                                                         RIGHT_DUPLICATE | RIGHT_TRANSFER);
            if (vfs_chan_h == HANDLE_INVALID) {
                serial_write("[IRIS][SVCMGR] WARN: vfs handle insert failed\n");
                kobject_release(&vfs_ch->base);
            } else {
                struct KChanMsg msg;
                uint32_t i;
                uint32_t service_id = SVCMGR_SERVICE_VFS;
                for (i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
                msg.type = SVCMGR_MSG_SPAWN_SERVICE;
                for (i = 0; i < 4; i++)
                    msg.data[SVCMGR_SPAWN_OFF_SERVICE_ID + i] = ((uint8_t *)&service_id)[i];
                for (i = 0; i < 4; i++)
                    msg.data[SVCMGR_SPAWN_OFF_REG_CHAN + i] = ((uint8_t *)&vfs_chan_h)[i];
                msg.data[SVCMGR_SPAWN_OFF_IRQ] = 0xFF;
                msg.data_len = SVCMGR_SPAWN_MSG_LEN;

                iris_error_t sr = kchannel_send(svcmgr_bootstrap_ch, &msg);
                if (sr == IRIS_OK) {
                    serial_write("[IRIS][SVCMGR] vfs bootstrap request queued\n");
                } else {
                    serial_write("[IRIS][SVCMGR] WARN: vfs spawn request send failed\n");
                }

                kobject_release(&vfs_ch->base);  /* svcmgr table holds the ref */
            }
        }
    }

#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
    {
        struct KChanMsg msg;
        uint32_t i;
        uint64_t probe_entry = (uint64_t)(uintptr_t)phase3_exit_child;

        for (i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
        msg.type = SVCMGR_MSG_PHASE3_PROBE;
        for (i = 0; i < 8; i++)
            msg.data[SVCMGR_P3_OFF_ENTRY + i] = ((uint8_t *)&probe_entry)[i];
        msg.data_len = SVCMGR_P3_MSG_LEN;

        iris_error_t sr = kchannel_send(svcmgr_bootstrap_ch, &msg);
        if (sr == IRIS_OK) {
            serial_write("[IRIS][SVCMGR] phase3 probe queued\n");
        } else {
            serial_write("[IRIS][SVCMGR] WARN: phase3 probe send failed\n");
        }
    }
#endif

    kobject_release(&ch->base);  /* release local alloc ref; kernel ref above + table ref remain */
}

iris_error_t svcmgr_bootstrap_attach_client(struct task *client,
                                            iris_rights_t rights,
                                            handle_id_t *out_handle) {
    handle_id_t h;

    if (!client || !client->process || !out_handle) return IRIS_ERR_INVALID_ARG;
    if (!svcmgr_bootstrap_ch) return IRIS_ERR_INTERNAL;
    if ((rights & ~RIGHT_WRITE) != 0 || rights == RIGHT_NONE)
        return IRIS_ERR_INVALID_ARG;

    h = handle_table_insert(&client->process->handle_table,
                            &svcmgr_bootstrap_ch->base,
                            rights);
    if (h == HANDLE_INVALID) return IRIS_ERR_TABLE_FULL;

    *out_handle = h;
    return IRIS_OK;
}
