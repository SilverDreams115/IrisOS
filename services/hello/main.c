/* SPDX-License-Identifier: Apache-2.0 */
/*
 * hello/main.c — the first program.
 *
 * Not a service.  It is an ELF on a filesystem, read by path and spawned by
 * `proc`, and it holds only what the program contract says a program holds.
 *
 * ── What it is for ─────────────────────────────────────────
 *
 * To be the thing that proves the spawn, and to prove it by READING rather
 * than by existing.  A program that merely returned a constant would exit with
 * the same status whether or not its stack was built correctly — and the stack
 * is the part of a spawn most likely to be subtly wrong.  So this one walks
 * what it was given and reports which piece was missing, in its exit status,
 * because on a machine with no debugger the status is the whole of what comes
 * back.
 *
 * It uses no libc.  There is not one yet; that is what the rest of Stage
 * 10-run is for, and this program is what will run against it when there is.
 */
#include <stdint.h>
#include <iris/program_abi.h>

/* The exit statuses, which are the diagnosis.  Distinct and small, because a
 * status is one byte of evidence and a range of them is a map. */
#define HELLO_OK            42
#define HELLO_BAD_ARGC       1
#define HELLO_BAD_ARGV       2
#define HELLO_BAD_ENVP       3
#define HELLO_NO_PAGESZ      4
#define HELLO_BAD_PAGESZ     5
#define HELLO_NO_IRIS_AUX    6
#define HELLO_BAD_UNTYPED    7

int hello_main(const uint64_t *sp);

int hello_main(const uint64_t *sp) {
    if (!sp) return HELLO_BAD_ARGC;

    /*
     * The layout is the ABI's: argc, then argv and a NULL, then envp and a
     * NULL, then the auxiliary vector until AT_NULL.  Walked rather than
     * indexed, because the only thing that says where envp starts is where
     * argv ended.
     */
    uint64_t argc = sp[0];
    if (argc < 1u || argc > 64u) return HELLO_BAD_ARGC;

    const uint64_t *p = sp + 1;
    for (uint64_t i = 0; i < argc; i++) {
        if (p[i] == 0u) return HELLO_BAD_ARGV;   /* a NULL before the end */
    }
    if (p[argc] != 0u) return HELLO_BAD_ARGV;    /* ...and one AT the end */
    p += argc + 1u;

    /* envp, however many, terminated. */
    uint64_t envc = 0;
    while (p[envc] != 0u) {
        if (envc > 64u) return HELLO_BAD_ENVP;
        envc++;
    }
    p += envc + 1u;

    /* The auxiliary vector.  What is being checked is that the entries this
     * system promises are THERE and say what they should — an auxv that was
     * built at the wrong offset would still terminate, and would carry
     * nothing recognisable. */
    int saw_pagesz = 0, saw_objc = 0, saw_untyped = 0;
    for (; p[0] != 0u; p += 2) {
        if (p[0] == AT_PAGESZ) {
            saw_pagesz = 1;
            if (p[1] != 4096u) return HELLO_BAD_PAGESZ;
        } else if (p[0] == AT_IRIS_OBJC) {
            saw_objc = 1;
        } else if (p[0] == AT_IRIS_UNTYPED) {
            saw_untyped = 1;
            /* It must name the slot the contract says holds the budget; a
             * program that read a different number would be a program whose
             * first allocation reached for something else. */
            if (p[1] != 12u) return HELLO_BAD_UNTYPED;
        }
    }

    if (!saw_pagesz)                return HELLO_NO_PAGESZ;
    if (!saw_objc || !saw_untyped)  return HELLO_NO_IRIS_AUX;

    return HELLO_OK;
}
