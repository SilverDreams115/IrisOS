#include <iris/phase3_selftest.h>
#include <iris/serial.h>
#include <iris/task.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <iris/nc/handle.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kvmo.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <stdint.h>

static HandleTable phase3_ht;
static HandleTable phase3_dest_ht;
static HandleTable phase3_full_ht;
static HandleTable phase3_channel_close_ht;
static struct KProcess phase3_channel_recv_proc;
static handle_id_t phase3_fill_ids[HANDLE_TABLE_MAX];

static int phase3_quota_selftest(void) {
    struct KProcess *proc = 0;
    struct KChannel *channels[KPROCESS_CHANNEL_QUOTA + 1];
    struct KNotification *notifs[KPROCESS_NOTIFICATION_QUOTA + 1];
    struct KVmo *vmos[KPROCESS_VMO_QUOTA + 1];
    int ok = 0;

    for (uint32_t i = 0; i < KPROCESS_CHANNEL_QUOTA + 1; i++) channels[i] = 0;
    for (uint32_t i = 0; i < KPROCESS_NOTIFICATION_QUOTA + 1; i++) notifs[i] = 0;
    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA + 1; i++) vmos[i] = 0;

    proc = kprocess_alloc();
    if (!proc) goto out;

    for (uint32_t i = 0; i < KPROCESS_CHANNEL_QUOTA; i++) {
        channels[i] = kchannel_alloc();
        if (!channels[i]) goto out;
        if (kchannel_bind_owner(channels[i], proc) != IRIS_OK) goto out;
    }
    channels[KPROCESS_CHANNEL_QUOTA] = kchannel_alloc();
    if (!channels[KPROCESS_CHANNEL_QUOTA]) goto out;
    if (kchannel_bind_owner(channels[KPROCESS_CHANNEL_QUOTA], proc) != IRIS_ERR_NO_MEMORY) goto out;
    kchannel_free(channels[0]);
    channels[0] = 0;
    if (kchannel_bind_owner(channels[KPROCESS_CHANNEL_QUOTA], proc) != IRIS_OK) goto out;

    for (uint32_t i = 0; i < KPROCESS_NOTIFICATION_QUOTA; i++) {
        notifs[i] = knotification_alloc();
        if (!notifs[i]) goto out;
        if (knotification_bind_owner(notifs[i], proc) != IRIS_OK) goto out;
    }
    notifs[KPROCESS_NOTIFICATION_QUOTA] = knotification_alloc();
    if (!notifs[KPROCESS_NOTIFICATION_QUOTA]) goto out;
    if (knotification_bind_owner(notifs[KPROCESS_NOTIFICATION_QUOTA], proc) != IRIS_ERR_NO_MEMORY) goto out;

    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA; i++) {
        vmos[i] = kvmo_create(0x1000ULL);
        if (!vmos[i]) goto out;
        if (kvmo_bind_owner(vmos[i], proc) != IRIS_OK) goto out;
    }
    vmos[KPROCESS_VMO_QUOTA] = kvmo_create(0x1000ULL);
    if (!vmos[KPROCESS_VMO_QUOTA]) goto out;
    if (kvmo_bind_owner(vmos[KPROCESS_VMO_QUOTA], proc) != IRIS_ERR_NO_MEMORY) goto out;

    if (proc->owned_channels != KPROCESS_CHANNEL_QUOTA) goto out;
    if (proc->owned_notifications != KPROCESS_NOTIFICATION_QUOTA) goto out;
    if (proc->owned_vmos != KPROCESS_VMO_QUOTA) goto out;

    ok = 1;

out:
    for (uint32_t i = 0; i < KPROCESS_CHANNEL_QUOTA + 1; i++) {
        if (channels[i]) kchannel_free(channels[i]);
    }
    for (uint32_t i = 0; i < KPROCESS_NOTIFICATION_QUOTA + 1; i++) {
        if (notifs[i]) knotification_free(notifs[i]);
    }
    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA + 1; i++) {
        if (vmos[i]) kvmo_free(vmos[i]);
    }
    if (proc) kprocess_free(proc);
    return ok;
}

static int phase3_process_selftest(void) {
    struct KProcess *proc = 0;
    struct KChannel *ch = 0;
    struct KVmo *vmo = 0;
    struct KVmo *large_vmo = 0;
    struct KVmo *map_vmo = 0;
    struct task fake_task;
    uint64_t ref_before;
    uint64_t active_before;
    uint64_t first_phys = 0;
    const uint64_t fault_addr = USER_VMO_BASE;
    const uint64_t large_fault_addr = USER_VMO_BASE + 0x00200000ULL;
    const uint64_t large_fault_last = large_fault_addr + 0x1FF000ULL;
    int ok = 0;

    proc = kprocess_alloc();
    ch = kchannel_alloc();
    vmo = kvmo_create(0x1000ULL);
    large_vmo = kvmo_create(0x200000ULL);
    map_vmo = kvmo_create(0x1000ULL);
    if (!proc || !ch || !vmo || !large_vmo || !map_vmo) goto out;
    if (large_vmo->page_capacity < 512u) goto out;

    ref_before = atomic_load_explicit(&ch->base.refcount, memory_order_relaxed);
    active_before = atomic_load_explicit(&ch->base.active_refs, memory_order_relaxed);
    if (kprocess_set_exception_handler(proc, ch) != IRIS_OK) goto out;
    if (kprocess_set_exception_handler(proc, ch) != IRIS_OK) goto out;
    if (proc->exception_chan != ch) goto out;
    if (atomic_load_explicit(&ch->base.refcount, memory_order_relaxed) != ref_before + 1u) goto out;
    if (atomic_load_explicit(&ch->base.active_refs, memory_order_relaxed) != active_before + 1u) goto out;

    proc->cr3 = paging_create_user_space();
    if (!proc->cr3) goto out;
    if (kprocess_register_vmo_map(proc, fault_addr, 0x1000ULL, vmo,
                                  PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) != IRIS_OK) goto out;
    if (kprocess_register_vmo_map(proc, large_fault_addr, 0x200000ULL, large_vmo,
                                  PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) != IRIS_OK) goto out;
    for (uint32_t i = 2; i < 80u; i++) {
        uint64_t virt = USER_VMO_BASE + ((uint64_t)i * 0x00200000ULL);
        if (kprocess_register_vmo_map(proc, virt, 0x1000ULL, map_vmo,
                                      PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) != IRIS_OK) goto out;
    }
    if (proc->vmo_mapping_count < 80u) goto out;

    for (uint32_t i = 0; i < sizeof(fake_task); i++) ((uint8_t *)&fake_task)[i] = 0;
    fake_task.process = proc;

    if (kprocess_resolve_demand_fault(&fake_task, fault_addr) != IRIS_OK) goto out;
    first_phys = vmo->pages[0];
    if (!first_phys) goto out;
    if (paging_virt_to_phys_in(proc->cr3, fault_addr) != first_phys) goto out;

    if (kprocess_resolve_demand_fault(&fake_task, fault_addr) != IRIS_OK) goto out;
    if (vmo->pages[0] != first_phys) goto out;
    if (paging_virt_to_phys_in(proc->cr3, fault_addr) != first_phys) goto out;

    if (kprocess_resolve_demand_fault(&fake_task, large_fault_last) != IRIS_OK) goto out;
    if (!large_vmo->pages[511]) goto out;
    if (paging_virt_to_phys_in(proc->cr3, large_fault_last) != large_vmo->pages[511]) goto out;

    kprocess_teardown(proc, 0);
    if (vmo->pages[0] != first_phys) goto out;
    if (!large_vmo->pages[511]) goto out;
    if (proc->exception_chan != 0) goto out;
    if (atomic_load_explicit(&ch->base.refcount, memory_order_relaxed) != ref_before) goto out;
    if (atomic_load_explicit(&ch->base.active_refs, memory_order_relaxed) != active_before) goto out;

    ok = 1;
out:
    if (proc) {
        kprocess_teardown(proc, 0);
        kprocess_reap_address_space(proc);
        kprocess_free(proc);
    }
    if (ch) kchannel_free(ch);
    if (vmo) kvmo_free(vmo);
    if (large_vmo) kvmo_free(large_vmo);
    if (map_vmo) kvmo_free(map_vmo);
    return ok;
}

static int phase3_handle_selftest(void) {
    struct KObject *obj = 0;
    struct KObject *obj2 = 0;
    iris_rights_t rights = RIGHT_NONE;
    handle_id_t h = HANDLE_INVALID;
    handle_id_t h_src = HANDLE_INVALID;
    handle_id_t h_dup = HANDLE_INVALID;
    handle_id_t h_dest = HANDLE_INVALID;
    struct KNotification *notif = 0;
    struct KNotification *fill_notif = 0;
    struct KBootstrapCap *cap = 0;
    struct KBootstrapCap *cap_clone = 0;
    handle_id_t h_cap_src = HANDLE_INVALID;
    handle_id_t h_cap_alias = HANDLE_INVALID;
    int ok = 0;

    handle_table_init(&phase3_ht);
    handle_table_init(&phase3_dest_ht);
    handle_table_init(&phase3_full_ht);

    notif = knotification_alloc();
    if (!notif) goto out;

    h = handle_table_insert(&phase3_ht, &notif->base,
                            RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT |
                            RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID || handle_id_gen(h) == 0u) goto out;
    if (handle_table_get_object(&phase3_ht, 1u, &obj, &rights) != IRIS_ERR_BAD_HANDLE) goto out;

    if (handle_table_close(&phase3_ht, h) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase3_ht, h, &obj, &rights) != IRIS_ERR_BAD_HANDLE) goto out;

    h_src = handle_table_insert(&phase3_ht, &notif->base,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT |
                                RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h_src == HANDLE_INVALID) goto out;
    if (handle_table_get_object(&phase3_ht, h_src, &obj, &rights) != IRIS_OK) goto out;
    h_dup = handle_table_insert(&phase3_ht, obj, rights_reduce(rights, RIGHT_READ));
    kobject_release(obj);
    obj = 0;
    if (h_dup == HANDLE_INVALID) goto out;
    if (handle_table_get_object(&phase3_ht, h_dup, &obj, &rights) != IRIS_OK) goto out;
    if (rights != RIGHT_READ) goto out;
    kobject_release(obj);
    obj = 0;

    if (handle_table_get_object(&phase3_ht, h_src, &obj, &rights) != IRIS_OK) goto out;
    h_dest = handle_table_insert(&phase3_dest_ht, obj, rights_reduce(rights, RIGHT_WAIT));
    kobject_release(obj);
    obj = 0;
    if (h_dest == HANDLE_INVALID) goto out;
    if (handle_table_close(&phase3_ht, h_src) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase3_ht, h_src, &obj, &rights) != IRIS_ERR_BAD_HANDLE) goto out;
    if (handle_table_get_object(&phase3_dest_ht, h_dest, &obj, &rights) != IRIS_OK) goto out;
    if (rights != RIGHT_WAIT) goto out;
    kobject_release(obj);
    obj = 0;

    cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE |
                         IRIS_BOOTCAP_HW_ACCESS |
                         IRIS_BOOTCAP_KDEBUG);
    if (!cap) goto out;
    h_cap_src = handle_table_insert(&phase3_ht, &cap->base, RIGHT_READ);
    if (h_cap_src == HANDLE_INVALID) goto out;
    h_cap_alias = handle_table_insert(&phase3_dest_ht, &cap->base, RIGHT_READ);
    if (h_cap_alias == HANDLE_INVALID) goto out;

    cap_clone = kbootcap_clone_restricted(cap, IRIS_BOOTCAP_SPAWN_SERVICE);
    if (!cap_clone) goto out;
    if (handle_table_replace(&phase3_dest_ht, h_cap_alias, &cap_clone->base) != IRIS_OK) goto out;
    kobject_release(&cap_clone->base);
    cap_clone = 0;

    if (handle_table_get_object(&phase3_ht, h_cap_src, &obj, &rights) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase3_dest_ht, h_cap_alias, &obj2, &rights) != IRIS_OK) goto out;
    if (obj == obj2) goto out;
    if (!kbootcap_allows((struct KBootstrapCap *)obj,
                         IRIS_BOOTCAP_SPAWN_SERVICE | IRIS_BOOTCAP_HW_ACCESS | IRIS_BOOTCAP_KDEBUG)) goto out;
    if (!kbootcap_allows((struct KBootstrapCap *)obj2, IRIS_BOOTCAP_SPAWN_SERVICE)) goto out;
    if (kbootcap_allows((struct KBootstrapCap *)obj2, IRIS_BOOTCAP_HW_ACCESS)) goto out;
    if (kbootcap_allows((struct KBootstrapCap *)obj2, IRIS_BOOTCAP_KDEBUG)) goto out;
    kobject_release(obj);
    kobject_release(obj2);
    obj = 0;
    obj2 = 0;

    fill_notif = knotification_alloc();
    if (!fill_notif) goto out;
    for (uint32_t i = 0; i < HANDLE_TABLE_MAX; i++) {
        phase3_fill_ids[i] = handle_table_insert(&phase3_full_ht, &fill_notif->base, RIGHT_READ);
        if (phase3_fill_ids[i] == HANDLE_INVALID) goto out;
    }
    if (handle_table_insert(&phase3_full_ht, &fill_notif->base, RIGHT_READ) != HANDLE_INVALID) goto out;
    for (uint32_t i = 0; i < HANDLE_TABLE_MAX; i++) {
        if (handle_table_close(&phase3_full_ht, phase3_fill_ids[i]) != IRIS_OK) goto out;
    }
    if (atomic_load_explicit(&fill_notif->base.active_refs, memory_order_relaxed) != 0u) goto out;
    h = handle_table_insert(&phase3_full_ht, &fill_notif->base, RIGHT_READ);
    if (h == HANDLE_INVALID) goto out;
    if (handle_table_close(&phase3_full_ht, h) != IRIS_OK) goto out;

    ok = 1;

out:
    if (obj) kobject_release(obj);
    if (obj2) kobject_release(obj2);
    handle_table_close_all(&phase3_ht);
    handle_table_close_all(&phase3_dest_ht);
    handle_table_close_all(&phase3_full_ht);
    if (notif) knotification_free(notif);
    if (fill_notif) knotification_free(fill_notif);
    if (cap) kbootcap_free(cap);
    if (cap_clone) kbootcap_free(cap_clone);
    return ok;
}

static int phase3_channel_selftest(void) {
    struct KChannel *ch = kchannel_alloc();
    struct KNotification *notif = 0;
    struct KChanMsg msg;
    struct task fake_waiters[2];
    struct task cancelled_waiter;
    handle_id_t close_h = HANDLE_INVALID;
    struct KObject *obj = 0;
    iris_rights_t rights = RIGHT_NONE;
    int ok = 0;

    if (!ch) return 0;
    notif = knotification_alloc();
    if (!notif) goto out;

    for (uint32_t i = 0; i < sizeof(phase3_channel_recv_proc); i++)
        ((uint8_t *)&phase3_channel_recv_proc)[i] = 0;
    handle_table_init(&phase3_channel_recv_proc.handle_table);
    handle_table_init(&phase3_channel_close_ht);

    for (uint32_t i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
    if (kchannel_try_recv(ch, &msg) != IRIS_ERR_WOULD_BLOCK) goto out;

    for (uint32_t i = 0; i < KCHAN_CAPACITY; i++) {
        msg.type = i + 1;
        if (kchannel_send(ch, &msg) != IRIS_OK) goto out;
    }
    if (kchannel_send(ch, &msg) != IRIS_ERR_OVERFLOW) goto out;
    for (uint32_t i = 0; i < KCHAN_CAPACITY; i++) {
        if (kchannel_try_recv(ch, &msg) != IRIS_OK) goto out;
    }
    if (kchannel_try_recv(ch, &msg) != IRIS_ERR_WOULD_BLOCK) goto out;

    for (uint32_t i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
    msg.type = 0x33;
    kobject_retain(&notif->base);
    kobject_active_retain(&notif->base);
    if (kchannel_send_attached(ch, &msg, &notif->base, RIGHT_WAIT) != IRIS_OK) {
        kobject_active_release(&notif->base);
        kobject_release(&notif->base);
        goto out;
    }
    if (kchannel_try_recv(ch, &msg) != IRIS_ERR_INVALID_ARG) goto out;
    if (kchannel_try_recv_into_process(ch, &phase3_channel_recv_proc, &msg) != IRIS_OK) goto out;
    if (msg.type != 0x33) goto out;
    if (msg.attached_handle == HANDLE_INVALID) goto out;
    if (msg.attached_rights != RIGHT_WAIT) goto out;
    if (handle_table_get_object(&phase3_channel_recv_proc.handle_table,
                                msg.attached_handle, &obj, &rights) != IRIS_OK) goto out;
    if (obj->type != KOBJ_NOTIFICATION) goto out;
    if (rights != RIGHT_WAIT) goto out;
    kobject_release(obj);
    obj = 0;
    if (handle_table_close(&phase3_channel_recv_proc.handle_table, msg.attached_handle) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase3_channel_recv_proc.handle_table,
                                msg.attached_handle, &obj, &rights) != IRIS_ERR_BAD_HANDLE) goto out;

    for (uint32_t i = 0; i < sizeof(fake_waiters); i++) ((uint8_t *)&fake_waiters)[i] = 0;
    fake_waiters[0].state = TASK_BLOCKED_IPC;
    fake_waiters[1].state = TASK_BLOCKED_IPC;
    ch->waiters[0] = &fake_waiters[0];
    ch->waiters[1] = &fake_waiters[1];
    ch->waiter_count = 2;
    close_h = handle_table_insert(&phase3_channel_close_ht, &ch->base, RIGHT_READ | RIGHT_WRITE);
    if (close_h == HANDLE_INVALID) goto out;
    if (handle_table_close(&phase3_channel_close_ht, close_h) != IRIS_OK) goto out;
    close_h = HANDLE_INVALID;
    if (!ch->closed) goto out;
    if (fake_waiters[0].state != TASK_READY) goto out;
    if (fake_waiters[1].state != TASK_READY) goto out;
    if (ch->waiter_count != 0) goto out;
    if (kchannel_try_recv(ch, &msg) != IRIS_ERR_CLOSED) goto out;
    if (kchannel_send(ch, &msg) != IRIS_ERR_CLOSED) goto out;

    ch->closed = 0;
    for (uint32_t i = 0; i < KCHANNEL_WAITERS_MAX; i++) ch->waiters[i] = 0;
    ch->waiter_count = 0;
    for (uint32_t i = 0; i < sizeof(cancelled_waiter); i++) ((uint8_t *)&cancelled_waiter)[i] = 0;
    cancelled_waiter.state = TASK_BLOCKED_IPC;
    ch->waiters[3] = &cancelled_waiter;
    ch->waiter_count = 1;
    kchannel_cancel_waiter(&cancelled_waiter);
    if (ch->waiters[3] != 0) goto out;
    if (ch->waiter_count != 0) goto out;

    ch->closed = 1;
    if (kchannel_waiters_add_or_closed(ch, &cancelled_waiter) != IRIS_ERR_CLOSED) goto out;
    if (ch->waiter_count != 0) goto out;
    ch->closed = 0;

    for (uint32_t i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0xA5u;
    kobject_retain(&notif->base);
    kobject_active_retain(&notif->base);
    if (kchannel_send_attached(ch, &msg, &notif->base, RIGHT_WAIT) != IRIS_OK) {
        kobject_active_release(&notif->base);
        kobject_release(&notif->base);
        goto out;
    }
    msg.type = 0xDEADBEEFu;
    msg.attached_handle = 0xFFFFFFFFu;
    msg.attached_rights = RIGHT_TRANSFER;
    if (kchannel_try_recv_into_process(ch, 0, &msg) != IRIS_ERR_INVALID_ARG) goto out;
    if (msg.type != 0xDEADBEEFu) goto out;
    if (msg.attached_handle != 0xFFFFFFFFu) goto out;
    if (msg.attached_rights != RIGHT_TRANSFER) goto out;
    if (kchannel_try_recv_into_process(ch, &phase3_channel_recv_proc, &msg) != IRIS_OK) goto out;

    ok = 1;
out:
    if (obj) kobject_release(obj);
    handle_table_close_all(&phase3_channel_recv_proc.handle_table);
    handle_table_close_all(&phase3_channel_close_ht);
    kchannel_free(ch);
    if (notif) knotification_free(notif);
    return ok;
}

static int phase3_notification_selftest(void) {
    struct KNotification *n = knotification_alloc();
    struct task fake_waiter;
    struct task cancelled_waiter;
    uint64_t bits = 0;
    int ok = 0;

    if (!n) return 0;

    knotification_signal(n, 0x5ULL);
    if (knotification_wait(n, &bits) != IRIS_OK) goto out;
    if (bits != 0x5ULL) goto out;

    for (uint32_t i = 0; i < sizeof(fake_waiter); i++) ((uint8_t *)&fake_waiter)[i] = 0;
    fake_waiter.state = TASK_BLOCKED_IRQ;
    n->waiters[0] = &fake_waiter;
    n->waiter_count = 1;
    kobject_active_retain(&n->base);
    kobject_active_release(&n->base);
    if (!n->closed) goto out;
    if (fake_waiter.state != TASK_READY) goto out;
    if (n->waiters[0] != 0) goto out;
    if (knotification_wait(n, &bits) != IRIS_ERR_CLOSED) goto out;

    n->closed = 0;
    for (uint32_t i = 0; i < sizeof(cancelled_waiter); i++) ((uint8_t *)&cancelled_waiter)[i] = 0;
    cancelled_waiter.state = TASK_BLOCKED_IRQ;
    n->waiters[0] = &cancelled_waiter;
    n->waiter_count = 1;
    knotification_cancel_waiter(&cancelled_waiter);
    if (n->waiters[0] != 0) goto out;

    ok = 1;
out:
    knotification_free(n);
    return ok;
}

int phase3_selftest_run(void) {
    if (!phase3_handle_selftest()) {
        serial_write("[IRIS][P3] WARN: handle selftest failed\n");
        return 0;
    }
    if (!phase3_channel_selftest()) {
        serial_write("[IRIS][P3] WARN: channel selftest failed\n");
        return 0;
    }
    if (!phase3_notification_selftest()) {
        serial_write("[IRIS][P3] WARN: notification selftest failed\n");
        return 0;
    }
    if (!phase3_process_selftest()) {
        serial_write("[IRIS][P3] WARN: process selftest failed\n");
        return 0;
    }
    if (!phase3_quota_selftest()) {
        serial_write("[IRIS][P3] WARN: quota selftest failed\n");
        return 0;
    }

    serial_write("[IRIS][P3] handle/lifecycle selftests OK\n");
    return 1;
}
