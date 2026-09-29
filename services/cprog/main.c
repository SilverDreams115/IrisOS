/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cprog/main.c — an ordinary C program.
 *
 * There is nothing about IRIS in this file.  It includes standard headers,
 * calls standard functions, and returns a status from `main`.  That is the
 * whole of Stage 10-run step 6's close condition: a C program nobody modified,
 * compiled against this libc, loaded from the filesystem at runtime, printing
 * to the console and exiting with a status somebody reads.
 *
 * What it costs the system to run this is: a `PT_INTERP` naming the C library,
 * an interpreter that relocates itself and then resolves this program's
 * `JUMP_SLOT` for `__libc_start_main` against the object table its spawner
 * filled, a thread pointer so the ordinary stack protector works, a heap out
 * of this program's own budget, and a console endpoint it was handed.  None of
 * that is visible from here, which is the point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    char *buf = malloc(64);
    size_t n;

    if (!buf) return 3;

    strcpy(buf, "IRIS");
    n = strlen(buf);
    if (n != 4u || strcmp(buf, "IRIS") != 0) { free(buf); return 4; }

    snprintf(buf, 64, "%s/%d", "libc", 6);
    if (strcmp(buf, "libc/6") != 0) { free(buf); return 5; }

    printf("[CPROG] hello from a C program: %s, argc=%d, argv[0]=%s\n",
           buf, argc, argc > 0 ? argv[0] : "?");
    printf("[CPROG] %d %u %x %-6s|%6s| %+d %05d %%\n",
           -42, 42u, 0xBEEFu, "left", "right", 7, 99);

    free(buf);

    /* A status somebody reads.  Seven, because zero is what a program that did
     * nothing would also return. */
    return 7;
}
