/* SPDX-License-Identifier: Apache-2.0 */
/*
 * start.c — everything between the interpreter's self-relocation and `main`.
 *
 * `dlstart.S` has already relocated THIS image; from here C is safe.  What is
 * left is the difference between an interpreter and a C library: resolve the
 * program's symbols against the objects it was given, give the thread a
 * THREAD POINTER, wake up the console and the heap, and hand control to the
 * program's entry — which is `crt1.S`, which calls `main`.
 *
 * ── The thread pointer is Stage 10-run step 1's payoff ─────────────────────
 *
 * Ring 3 had none: `%fs` was unset, `CR4.FSGSBASE` is off, and every service
 * in this tree is built `-mstack-protector-guard=global` because the ordinary
 * x86-64 canary is read from `%fs:0x28`.  `TCB_SetTLSBase` was added for this,
 * and a program compiled against this libc is the first thing in the system
 * that gets to be ORDINARY about it: default stack protector, canary where the
 * compiler expects it, no build flag explaining why not.
 */
#include "libc_internal.h"
#include "../common/elf64.h"
#include <iris/program_abi.h>
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/endpoint_proto.h>
#include <stdint.h>

struct libc_state __libc;

uint64_t __libc_start(const uint64_t *sp);

/*
 * The thread control block this libc gives its thread.
 *
 * Variant II, which is what x86-64 uses: `%fs` points AT the block, `%fs:0`
 * holds its own address (so a program can take the address of a thread-local),
 * and `%fs:0x28` is the stack guard.  Static `__thread` storage would live
 * BELOW the pointer and needs `PT_TLS` from the program, which this libc does
 * not read yet — a program that declares a thread-local will not link, which
 * is a refusal rather than a silent zero.
 */
static uint64_t g_tcb[16] __attribute__((aligned(16)));

int __libc_tls_init(void) {
    uint64_t tp = (uint64_t)(uintptr_t)&g_tcb[0];
    uint64_t a, d;

    g_tcb[0] = tp;                              /* %fs:0    — self          */
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    a = (a ^ (d << 17)) * 6364136223846793005ULL + 1442695040888963407ULL;
    g_tcb[5] = a;                               /* %fs:0x28 — the canary    */

    return iris_invoke1((long)IRIS_CPTR_OWN_TCB, INV_TCB_SET_TLS_BASE,
                        (long)tp) == 0;
}

uint64_t __libc_start(const uint64_t *sp) {
    uint64_t at_phdr = 0, at_phnum = 0, at_phent = 0, at_entry = 0, at_base = 0;
    uint64_t prog_base = 0;
    const uint8_t *phs;
    int have_base = 0;

    if (!sp) return 0;

    /* ── what the stack says.  It is the only input this has. ── */
    {
        uint64_t argc, envc = 0;
        const uint64_t *p;
        argc = sp[0];
        if (argc > 64u) return 0;
        p = sp + 1 + argc + 1;
        while (p[envc] != 0u) { if (envc > 64u) return 0; envc++; }
        p += envc + 1u;
        for (; p[0] != 0u; p += 2) {
            switch (p[0]) {
                case AT_PHDR:         at_phdr  = p[1]; break;
                case AT_PHNUM:        at_phnum = p[1]; break;
                case AT_PHENT:        at_phent = p[1]; break;
                case AT_ENTRY:        at_entry = p[1]; break;
                case AT_BASE:         at_base  = p[1]; break;
                case AT_IRIS_UNTYPED: __libc.untyped = p[1]; break;
                case AT_IRIS_OBJC:    __libc.objc    = p[1]; break;
                case AT_IRIS_OBJV:    __libc.objv    = p[1]; break;
                default: break;
            }
        }
    }
    if (!at_phdr || !at_entry || !at_base) return 0;
    if (at_phnum == 0u || at_phnum > 64u) return 0;
    if (at_phent < sizeof(Elf64_Phdr)) return 0;
    __libc.interp_base = at_base;

    /* Where the program was loaded: `AT_PHDR` is a mapped address and
     * `PT_PHDR` is that table's own unbiased vaddr, so the difference is the
     * base.  It is the only derivation that assumes nothing about layout. */
    phs = (const uint8_t *)(uintptr_t)at_phdr;
    for (uint64_t i = 0; i < at_phnum; i++) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(const void *)(phs + i * at_phent);
        if (p->p_type != PT_PHDR) continue;
        if (at_phdr < p->p_vaddr) return 0;
        prog_base = at_phdr - p->p_vaddr;
        have_base = 1;
        break;
    }
    if (!have_base) return 0;
    __libc.prog_base = prog_base;

    /*
     * Link it.  A failure here is a REFUSAL to start the program: a symbol
     * this libc could not resolve is one the program will call, and letting it
     * run means faulting later at an address that names nothing.
     */
    if (__libc_link_program(prog_base, phs, at_phnum, at_phent) < 0) return 0;

    /* The thread pointer first: everything after this is ordinary C compiled
     * with an ordinary stack protector, and the canary it reads lives there. */
    if (!__libc_tls_init()) return 0;

    __libc_heap_init();
    __libc_console_init();

    return at_entry;
}
