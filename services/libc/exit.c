/* SPDX-License-Identifier: Apache-2.0 */
/*
 * exit.c — how a program stops.
 *
 * `SYS_EXIT`, which ends the THREAD.  Its status is kept by the kernel for
 * whoever holds a capability to that thread, which since Stage 7 is what a
 * supervisor names — there is no process object and nothing else to reap.
 * `proc` hands the caller that capability, so `exit(7)` here is the 7 the
 * shell prints.
 */
#include <stdlib.h>
#include <unistd.h>
#include "libc_internal.h"

_Noreturn void _exit(int status) {
    __asm__ volatile ("syscall" :: "a"(1L), "D"((long)status) : "memory");
    for (;;) __asm__ volatile ("hlt");
}

/* No `atexit` and no destructors yet: nothing in this libc registers one, and
 * a table that is always empty is a table somebody will one day trust. */
_Noreturn void exit(int status) { _exit(status); }

_Noreturn void abort(void) { _exit(134); }   /* 128 + SIGABRT, by convention */

/*
 * The stack protector's landing point.
 *
 * A program built here is built ORDINARILY — `-fstack-protector-strong`, with
 * the canary read from `%fs:0x28` where the compiler expects it — which is
 * only possible because `__libc_tls_init` gave the thread a thread pointer.
 * Every other image in this tree is built `-mstack-protector-guard=global`
 * with a comment explaining why not; a C program is the first thing in the
 * system that does not need the comment.
 *
 * It exits rather than printing: the canary is checked on the way OUT of a
 * frame that has already been overwritten, so the stack cannot be trusted to
 * make a call on.  The status is the conventional one so that whoever reads it
 * can tell this apart from a program that chose to exit.
 */
_Noreturn void __stack_chk_fail(void) { _exit(127); }
