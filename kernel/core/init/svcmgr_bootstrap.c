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
 *   2. svcmgr consumes the declarative built-in service catalog,
 *      creates public channels in userland, and keeps normal
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
#include <iris/nc/kbootcap.h>
#include <iris/nc/kobject.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/initrd.h>
#include <iris/service_catalog.h>
#include <iris/elf_loader.h>
#include <iris/scheduler.h>

/*
 * Kernel-side reference to the svcmgr bootstrap channel.
 * Used to install the first client bootstrap handle into user_init and to
 * carry bootstrap capability / lifecycle traffic into svcmgr.
 * Not released: this ref lives for the duration of the kernel.
 */
static struct KChannel *svcmgr_bootstrap_ch = 0;

/*
 * Bootstrap capability delivery:
 * The kernel injects a dedicated spawn capability handle into svcmgr over the
 * same private bootstrap channel used for later lifecycle events.
 */

void svcmgr_bootstrap_init(void) {
    struct KChannel *ch = kchannel_alloc();
    struct KBootstrapCap *spawn_cap = 0;
    if (!ch) {
        serial_write("[IRIS][SVCMGR] WARN: bootstrap channel alloc failed\n");
        return;
    }

    /* Load svcmgr from the embedded initrd and spawn it as an ELF process. */
    const void *elf_data = 0;
    uint32_t    elf_size = 0;
    if (!initrd_find(IRIS_BOOTSTRAP_SUPERVISOR_IMAGE, &elf_data, &elf_size)) {
        serial_write("[IRIS][SVCMGR] FATAL: svcmgr not found in initrd\n");
        kobject_release(&ch->base);
        return;
    }

    iris_elf_image_t img;
    iris_error_t lerr = elf_loader_load(elf_data, elf_size, &img);
    if (lerr != IRIS_OK) {
        serial_write("[IRIS][SVCMGR] FATAL: elf_loader_load failed\n");
        kobject_release(&ch->base);
        return;
    }

    struct task *sm = task_spawn_elf(&img, 0);
    if (!sm) {
        serial_write("[IRIS][SVCMGR] WARN: spawn failed\n");
        elf_loader_free_image(&img);
        kobject_release(&ch->base);
        return;
    }
    /* img is now owned by sm->process — do NOT call elf_loader_free_image */

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

    spawn_cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
    if (!spawn_cap) {
        serial_write("[IRIS][SVCMGR] FATAL: spawn cap alloc failed\n");
        task_abort_spawned_user(sm);
        kobject_release(&ch->base);
        return;
    }
    {
        struct KChanMsg msg;

        for (uint32_t i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
        msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
        svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                               SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP);
        msg.data_len = SVCMGR_BOOTSTRAP_MSG_LEN;
        msg.attached_handle = HANDLE_INVALID;
        msg.attached_rights = RIGHT_READ;

        kobject_retain(&spawn_cap->base);
        kobject_active_retain(&spawn_cap->base);
        if (kchannel_send_attached(ch, &msg, &spawn_cap->base, RIGHT_READ) != IRIS_OK) {
            serial_write("[IRIS][SVCMGR] FATAL: spawn cap bootstrap send failed\n");
            kobject_active_release(&spawn_cap->base);
            kobject_release(&spawn_cap->base);
            kbootcap_free(spawn_cap);
            task_abort_spawned_user(sm);
            kobject_release(&ch->base);
            return;
        }
        kbootcap_free(spawn_cap);
    }

    /*
     * Retain the kernel-side reference for bootstrap client installation
     * and optional selftest traffic.
     * kobject_retain was already done by handle_table_insert; we keep
     * one additional reference here for the kernel's own use.
     */
    kobject_retain(&ch->base);
    svcmgr_bootstrap_ch = ch;

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
