/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_PROG_STACK_H
#define IRIS_PROG_STACK_H

#include <stdint.h>

/*
 * The initial stack of a program, laid out where a C runtime expects it.
 *
 * System V AMD64 process initialisation, which is what an ELF entry point
 * finds and therefore not a place to be creative.  `docs/contracts/program.md`
 * §4 is the contract; this builds it.
 *
 * ── Why this is its own file with its own test ─────────────────────────────
 *
 * It is the part of a spawn most likely to be subtly wrong and least likely to
 * say so.  Every pointer written here is an address in the CHILD's address
 * space while the memory is mapped in the SPAWNER's, so the whole job is
 * writing one set of addresses through another — and a program handed an argv
 * whose pointers are the spawner's does not fail, it reads somebody else's
 * memory or faults somewhere unrelated to the mistake.
 *
 * So it takes both addresses, computes with the child's, and is exercised on
 * the host where the layout can be read back byte for byte.
 */

struct prog_auxv {
    uint64_t type;
    uint64_t val;
};

/*
 * Build it at the TOP of the region, and return the `%rsp` the child must
 * start with — or 0 if it does not fit, which a caller must treat as a
 * failure rather than as a small stack.
 *
 *   map_base   where the region is mapped in THIS address space
 *   child_base where the same region is mapped in the CHILD's
 *   bytes      its size; both mappings cover it
 *
 * `argv` and `envp` are arrays of NUL-terminated strings; their terminating
 * NULL entries are written by this function rather than passed.  The auxiliary
 * vector's AT_NULL terminator is likewise written here, so a caller cannot
 * forget it.
 *
 * The returned `%rsp` is 16-byte aligned, which the ABI requires at the entry
 * point and which every SSE spill in a runtime's startup depends on.
 */
uint64_t prog_stack_build(void *map_base, uint64_t child_base, uint64_t bytes,
                          const char *const *argv, uint32_t argc,
                          const char *const *envp, uint32_t envc,
                          const struct prog_auxv *aux, uint32_t auxc);

#endif /* IRIS_PROG_STACK_H */
