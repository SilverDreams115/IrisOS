#ifndef IRIS_IPC_DEMO_H
#define IRIS_IPC_DEMO_H

/*
 * Legacy IPC demo — bootstrap artifact.
 *
 * Initialises the legacy ipc_* subsystem and schedules the
 * kernel-internal producer/consumer ring-0 demo tasks.
 * This module is opt-in only via ENABLE_LEGACY_IPC_DEMO=1.
 *
 * This module exists solely to keep the legacy ipc_* code exercised
 * while it remains compiled in.  Remove together with
 * kernel/demo/ipc_demo.c and kernel/core/ipc/ipc.c when the demo
 * tasks are retired.
 */

void ipc_demo_start(void);

#endif
