#ifndef IRIS_PANIC_H
#define IRIS_PANIC_H

/*
 * iris_panic — unconditional kernel halt with a serial diagnostic message.
 *
 * Never returns.  May be called from any context including IRQ handlers and
 * early boot before the scheduler is up.  Uses direct serial I/O so it works
 * even if the klog ring or the console service is not reachable.
 */
__attribute__((noreturn)) void iris_panic(const char *msg);

/* Evaluates cond; if false, panics with msg. */
#define IRIS_ASSERT(cond, msg)  \
    do { if (!(cond)) iris_panic(msg); } while (0)

#endif
