/*
 * init.h — shared primitives for the init service (Fase 14).
 *
 * init began as a single large main.c.  Fase 14 is decomposing it into
 * auditable modules that share this contract.  Current layout (only these two
 * translation units exist today):
 *
 *   main.c          boot supervisor — orchestrates the healthy path + idle loop,
 *                   and still hosts the service-launch and bootstrap-grant logic
 *   init_test.c     runtime probes / S8 exception selftest / smoke markers
 *
 * Further extraction of the launcher and bootstrap-grant logic out of main.c is
 * planned but has NOT been split into separate files yet — do not assume any
 * module beyond the two above exists.
 *
 * This header holds only what every module needs: the raw syscall wrappers and
 * the core kernel ABI includes.  No functional behavior lives here.
 */
#ifndef IRIS_INIT_H
#define IRIS_INIT_H

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>

/* ── Raw syscall helpers ────────────────────────────────────────────────── */

static inline long init_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long init_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long init_sys2(long nr, long a0, long a1) {
    return init_sys3(nr, a0, a1, 0);
}

static inline long init_sys1(long nr, long a0) {
    return init_sys3(nr, a0, 0, 0);
}

static inline long init_sys0(long nr) {
    return init_sys3(nr, 0, 0, 0);
}

/* ── Cross-module contract ──────────────────────────────────────────────── */

/* Boot-supervisor log sink (main.c): console.ep once up, early-serial before. */
void init_log(const char *s);

/* Runtime probes + S8 exception selftest (init_test.c). */
void init_runtime_probe_invalid_userptr(void);
void init_runtime_probe_timeout_overflow(void);
void init_selftest_exception(void);

#endif /* IRIS_INIT_H */
