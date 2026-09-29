/* SPDX-License-Identifier: Apache-2.0 */
/*
 * heap.c — the break, over the program's own budget.
 *
 * `prog_mem` is the same allocator `services/alloc` proved in Stage 10-run
 * step 3: `brk` retypes frames out of `IRIS_CPTR_OWN_UNTYPED` and maps them
 * into `IRIS_CPTR_OWN_VSPACE`.  Nothing is asked of anybody — the permission
 * IS the budget — so `malloc` returning NULL is a program that has spent what
 * it was given, and nothing else on the machine is affected by it.
 */
#include "libc_internal.h"
#include "../common/prog_mem.h"
#include <iris/program_abi.h>
#include <iris/paging.h>
#include <iris/endpoint_proto.h>

/* Slot 38 is this library's page-table scratch; the frames it maps take slots
 * from 128 upward, which docs/contracts/program.md §2 gives the program and
 * which no C program will ever name. */
#define LIBC_SLOT_PT 38u

static struct prog_mem g_mem;
static int g_ready;

void __libc_heap_init(void) {
    if (!__libc.untyped) return;
    prog_mem_setup(&g_mem, USER_PRIVATE_BASE, __libc.untyped,
                   IRIS_CPTR_OWN_VSPACE, IRIS_PROG_SLOT_FREE2_LO, 255u,
                   LIBC_SLOT_PT);
    g_ready = 1;
}

void *__libc_sbrk(long delta) {
    uint64_t old;
    if (!g_ready || delta <= 0) return 0;
    old = prog_brk(&g_mem, (int64_t)delta);
    if (old == (uint64_t)-1) return 0;
    return (void *)(uintptr_t)old;
}
