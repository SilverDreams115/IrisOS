/* SPDX-License-Identifier: Apache-2.0 */
/*
 * start_main.c — the call into `main`, and what happens to what it returns.
 *
 * This is the symbol `crt1.S` cannot resolve for itself: it lives in
 * `libc.so`, the program references it, and the interpreter binds it at launch
 * against the object table the spawner filled.  So an ordinary C program's
 * very first cross-object call is the thing step 6 exists to make work.
 */
#include <stdlib.h>
#include <stdint.h>
#include "libc_internal.h"

/*
 * `main` arrives as an argument, from `crt1.S`.
 *
 * It is not declared `extern` and looked up, because it is defined in the
 * PROGRAM: a C library that referenced `main` would be a shared object with an
 * undefined symbol only one consumer could ever satisfy, and every program
 * that did not define one would fail to link against the library rather than
 * against itself.
 */
typedef int (*main_fn)(int, char **, char **);

_Noreturn void __libc_start_main(const uint64_t *sp, main_fn fn);

_Noreturn void __libc_start_main(const uint64_t *sp, main_fn fn) {
    int    argc = (int)sp[0];
    char **argv = (char **)(void *)(sp + 1);
    char **envp = argv + argc + 1;

    /*
     * `main`'s return value IS the exit status — that is the whole of a C
     * program's contract with whoever started it, and here "whoever" holds a
     * capability to this thread and reads the status off it.
     */
    exit(fn(argc, argv, envp));
}
