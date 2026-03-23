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
#include <iris/nc/kprocess.h>
#include <stdatomic.h>
#include <stdint.h>

static HandleTable phase3_ht;
static HandleTable phase3_dest_ht;
static HandleTable phase3_full_ht;
static HandleTable phase3_channel_close_ht;
static struct KProcess phase3_channel_recv_proc;
static handle_id_t phase3_fill_ids[HANDLE_TABLE_MAX];

static int phase3_handle_selftest(void) {
    struct KObject *obj = 0;
    iris_rights_t rights = RIGHT_NONE;
    handle_id_t h = HANDLE_INVALID;
    handle_id_t h_src = HANDLE_INVALID;
    handle_id_t h_dup = HANDLE_INVALID;
    handle_id_t h_dest = HANDLE_INVALID;
    struct KNotification *notif = 0;
    struct KNotification *fill_notif = 0;
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
    handle_table_close_all(&phase3_ht);
    handle_table_close_all(&phase3_dest_ht);
    handle_table_close_all(&phase3_full_ht);
    if (notif) knotification_free(notif);
    if (fill_notif) knotification_free(fill_notif);
    return ok;
}

static int phase3_channel_selftest(void) {
    struct KChannel *ch = kchannel_alloc();
    struct KNotification *notif = 0;
    struct KChanMsg msg;
    struct task fake_waiter;
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

    for (uint32_t i = 0; i < sizeof(fake_waiter); i++) ((uint8_t *)&fake_waiter)[i] = 0;
    fake_waiter.state = TASK_BLOCKED_IPC;
    ch->waiter = &fake_waiter;
    close_h = handle_table_insert(&phase3_channel_close_ht, &ch->base, RIGHT_READ | RIGHT_WRITE);
    if (close_h == HANDLE_INVALID) goto out;
    if (handle_table_close(&phase3_channel_close_ht, close_h) != IRIS_OK) goto out;
    close_h = HANDLE_INVALID;
    if (!ch->closed) goto out;
    if (fake_waiter.state != TASK_READY) goto out;
    if (ch->waiter != 0) goto out;
    if (kchannel_try_recv(ch, &msg) != IRIS_ERR_CLOSED) goto out;
    if (kchannel_send(ch, &msg) != IRIS_ERR_CLOSED) goto out;

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
    uint64_t bits = 0;
    int ok = 0;

    if (!n) return 0;

    knotification_signal(n, 0x5ULL);
    if (knotification_wait(n, &bits) != IRIS_OK) goto out;
    if (bits != 0x5ULL) goto out;

    for (uint32_t i = 0; i < sizeof(fake_waiter); i++) ((uint8_t *)&fake_waiter)[i] = 0;
    fake_waiter.state = TASK_BLOCKED_IRQ;
    n->waiter = &fake_waiter;
    kobject_active_retain(&n->base);
    kobject_active_release(&n->base);
    if (!n->closed) goto out;
    if (fake_waiter.state != TASK_READY) goto out;
    if (n->waiter != 0) goto out;
    if (knotification_wait(n, &bits) != IRIS_ERR_CLOSED) goto out;

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

    serial_write("[IRIS][P3] handle/lifecycle selftests OK\n");
    return 1;
}
