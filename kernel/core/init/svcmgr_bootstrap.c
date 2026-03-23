/*
 * ── Service manager bootstrap ────────────────────────────────────
 *
 * Spawns the ring-3 service manager and gives it its bootstrap
 * KChannel.  The kernel retains a reference to that channel so that
 * it can install one narrow bootstrap client handle for the first
 * user task that must reach svcmgr.
 *
 * Key architectural difference from kbd_bootstrap_init():
 *   - The kernel does NOT call ns_register here.
 *   - svcmgr self-bootstrap is explicit via task arg0, not via lookup.
 *   - Later userland clients should also prefer explicit bootstrap
 *     handles over kernel nameserver lookup for the first control path.
 *
 * Phase 2 migration path:
 *   1. The kernel now bootstraps only svcmgr itself and the first
 *      client handle that must reach it.
 *   2. svcmgr owns the auto-start manifest for compiled-in services,
 *      creates their public channels in userland, and keeps normal
 *      discovery authority there.
 *   3. IRQ routing registration stays kernel-side permanently, but
 *      steady-state service policy no longer originates in the kernel.
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
     * Retain the kernel-side reference for bootstrap client installation
     * and optional selftest traffic.
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

#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
    {
        struct KChanMsg msg;
        uint32_t i;
        extern void phase3_exit_child(void);
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
