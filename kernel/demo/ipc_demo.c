/*
 * ════════════════════════════════════════════════════════════════════
 * Legacy IPC demo — kernel/demo/ipc_demo.c
 *
 * PURPOSE: exercise the legacy ipc_* subsystem via two ring-0 tasks
 *   (producer/consumer) that run alongside the modern capability IPC
 *   surface.
 * SCOPE: ring-0 tasks only; no user-space visibility.
 * STATUS: opt-in scaffolding only.  Built and started only with
 *   ENABLE_LEGACY_IPC_DEMO=1; scheduled for removal together with
 *   ipc.c/ipc.h once the nameserver-based KChannel service model is
 *   validated.
 *
 * Do NOT add new tasks here.  New inter-task communication must use
 * KChannel (iris/nc/kchannel.h) + handle table.
 * ════════════════════════════════════════════════════════════════════
 */

/* ── Legacy IPC demo island ── keep isolated; do not add new includes of ipc.h ── */
#include <iris/ipc.h>
/* ── end legacy IPC include ─────────────────────────────────────────────────── */

#include <iris/ipc_demo.h>
#include <iris/scheduler.h>
#include <iris/serial.h>
#include <iris/task.h>

static int32_t ch_a_to_b = -1;  /* legacy IPC channel: producer → consumer */
static int32_t ch_b_to_a = -1;  /* legacy IPC channel: consumer → producer */

static void task_producer(void) {
    struct ipc_message msg;
    uint32_t counter = 0;
    for (;;) {
        counter++;
        msg.type        = IPC_MSG_DATA;
        msg.sender_id   = 1;
        msg.receiver_id = 2;
        msg.data_len    = 4;
        msg.data[0]     = (uint8_t)(counter & 0xFF);
        msg.data[1]     = (uint8_t)((counter >> 8) & 0xFF);
        msg.data[2]     = (uint8_t)((counter >> 16) & 0xFF);
        msg.data[3]     = (uint8_t)((counter >> 24) & 0xFF);

        int32_t r = ipc_send((uint32_t)ch_a_to_b, &msg);
        if (r == IPC_OK) {
            serial_write("[PRODUCER] sent #");
            serial_write_dec(counter);
            serial_write("\n");
        }

        struct ipc_message reply;
        ipc_recv((uint32_t)ch_b_to_a, &reply);
        serial_write("[PRODUCER] reply seq=");
        serial_write_dec(reply.seq);
        serial_write("\n");

        task_yield();
    }
}

static void task_consumer(void) {
    struct ipc_message msg;
    struct ipc_message reply;
    for (;;) {
        ipc_recv((uint32_t)ch_a_to_b, &msg);

        uint32_t value = (uint32_t)msg.data[0]
                       | ((uint32_t)msg.data[1] << 8)
                       | ((uint32_t)msg.data[2] << 16)
                       | ((uint32_t)msg.data[3] << 24);

        serial_write("[CONSUMER] value=");
        serial_write_dec(value);
        serial_write(" from=");
        serial_write_dec(msg.sender_id);
        serial_write("\n");

        reply.type        = IPC_MSG_REPLY;
        reply.sender_id   = 2;
        reply.receiver_id = msg.sender_id;
        reply.data_len    = 0;
        ipc_send((uint32_t)ch_b_to_a, &reply);

        task_yield();
    }
}
/* ════ end legacy IPC demo task definitions ═════════════════════════ */

/*
 * ipc_demo_start — initialise legacy IPC and schedule demo tasks.
 * Called once from iris_kernel_main after scheduler_init().
 * retire: remove this call and delete this file + ipc.c/ipc.h.
 */
void ipc_demo_start(void) {
    /* ── legacy IPC demo init (retire with this file) ── */
    serial_write("[IRIS][IPC] initializing...\n");
    ipc_init();
    ch_a_to_b = ipc_channel_create(1);
    ch_b_to_a = ipc_channel_create(2);
    serial_write("[IRIS][IPC] channels: ");
    serial_write_dec((uint64_t)ch_a_to_b);
    serial_write(" and ");
    serial_write_dec((uint64_t)ch_b_to_a);
    serial_write("\n");
    scheduler_add_task(task_producer);   /* legacy IPC demo — retire with island */
    scheduler_add_task(task_consumer);   /* legacy IPC demo — retire with island */
    serial_write("[IRIS][SCHED] producer + consumer created\n");
}
