/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libc_lock.c — the two operations, and the yield between them.
 *
 * See `libc_lock.h` for what is locked, what is not, and why the contended
 * path is a yield rather than a wait.
 */
#include "libc_lock.h"
#include <iris/syscall.h>

/* The yield, written here rather than taken from a service helper: this
 * library shares no code with the services, and `exit.c` spells its one
 * syscall the same way for the same reason. */
static void libc_yield(void) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"((long)SYS_YIELD)
                      : "rcx", "r11", "memory");
    (void)r;
}

void __libc_lock(struct libc_lock *l) {
    /*
     * Acquire, then give the processor away.
     *
     * The spin is bounded by whoever holds the lock making progress, and on
     * one processor that cannot happen until this thread stops running — so
     * the yield is not an optimisation here, it is the only thing that makes
     * the loop terminate.
     */
    while (__atomic_test_and_set(&l->held, __ATOMIC_ACQUIRE))
        libc_yield();
}

void __libc_unlock(struct libc_lock *l) {
    __atomic_clear(&l->held, __ATOMIC_RELEASE);
}
