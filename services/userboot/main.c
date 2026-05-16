#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/nc/kchannel.h>
#include <iris/svcmgr_proto.h>
#include "../../services/common/svc_loader.h"

static inline long ub_sys0(long nr) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static void ub_close(handle_id_t h) {
    if (h != HANDLE_INVALID)
        (void)ub_sys1(SYS_HANDLE_CLOSE, (long)h);
}

static void ub_msg_zero(struct KChanMsg *msg) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
}

static long ub_send_spawn_cap(handle_id_t child_boot_h, handle_id_t parent_cap_h) {
    struct KChanMsg msg;
    long dup_h;

    dup_h = ub_sys2(SYS_HANDLE_DUP, (long)parent_cap_h,
                    (long)(RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER));
    if (dup_h < 0) return dup_h;

    ub_msg_zero(&msg);
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP);
    msg.data_len        = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = (handle_id_t)dup_h;
    msg.attached_rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    if (ub_sys2(SYS_CHAN_SEND, (long)child_boot_h, (long)&msg) < 0) {
        ub_close((handle_id_t)dup_h);
        return (long)IRIS_ERR_WOULD_BLOCK;
    }

    return 0;
}

static void ub_park_root_bootstrap(void) {
    /* Root bootstrap task policy:
     * after delegating authority it keeps no live handles and remains parked.
     * That makes the lifecycle explicit without putting first-task teardown
     * back on the critical healthy-path IPC boundary. */
    for (;;) (void)ub_sys1(SYS_SLEEP, 60000);
}

void iris_userboot_main(handle_id_t bootstrap_cap_h) {
    handle_id_t init_proc_h = HANDLE_INVALID;
    handle_id_t init_boot_h = HANDLE_INVALID;

    if (bootstrap_cap_h == HANDLE_INVALID)
        goto fail;

    /* Verify kernel catalog size matches the ring-3 table before loading anything. */
    if (svc_initrd_count(bootstrap_cap_h) != (long)SL_CATALOG_COUNT)
        goto fail;

    if (svc_load(bootstrap_cap_h, "init", &init_proc_h, &init_boot_h) < 0)
        goto fail;

    if (ub_send_spawn_cap(init_boot_h, bootstrap_cap_h) < 0)
        goto fail;

    ub_close(init_boot_h);
    ub_close(init_proc_h);
    ub_close(bootstrap_cap_h);
    ub_park_root_bootstrap();

fail:
    ub_close(init_boot_h);
    ub_close(init_proc_h);
    ub_close(bootstrap_cap_h);
    (void)ub_sys1(SYS_EXIT, 1);
    for (;;) {}
}
