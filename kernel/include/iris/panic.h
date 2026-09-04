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

/*
 * Evaluates cond; if false, panics with msg.
 *
 * ALWAYS ON, deliberately, and the reason is worth stating because it is the
 * opposite of what most kernels do.  seL4 compiles its assertions out of a
 * release build because it has a machine-checked proof that they hold; IRIS
 * has no such proof, so the assertion IS the check.  A violated invariant here
 * does not mean a caller passed something silly — that is an error return —
 * it means kernel memory is already inconsistent, and continuing from that
 * point corrupts a capability system whose whole value is that it cannot be
 * corrupted.  A controlled halt with a message is strictly better than
 * carrying on and being wrong quietly.
 *
 * The cost is a compare and a never-taken branch, which the predictor gets
 * right every time.  That is the correct price for a kernel that cannot prove
 * its invariants any other way.
 *
 * Use it ONLY for facts the kernel guarantees about itself.  Anything a caller
 * can cause from ring 3 is an iris_error_t, never an assertion: a syscall that
 * can halt the machine on bad input is a denial of service with a nice
 * message.
 */
#define IRIS_ASSERT(cond, msg)  \
    do { if (!(cond)) iris_panic(msg); } while (0)

#endif
