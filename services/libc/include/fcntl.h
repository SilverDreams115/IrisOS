/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _FCNTL_H
#define _FCNTL_H
/*
 * There are no flags yet, and that is honest rather than minimal: this system
 * has no write path (see docs/architecture/sel4-convergence-roadmap.md,
 * Stage 10-run step 7), so every open is read-only and a flag that said
 * otherwise would be a flag that lied.  The argument is accepted and ignored,
 * so a program that passes O_RDONLY compiles and means what it says.
 */
#define O_RDONLY 0
int open(const char *path, int flags, ...);
#endif
