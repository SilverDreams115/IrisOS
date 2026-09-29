/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crt1.S — a C program's `_start`, and the only assembly a C program has.
 *
 * It is part of the C LIBRARY, not part of the program: every system links one
 * of these into every C executable, and a program is no less "unmodified" for
 * having it.  What it does is the ABI's own handover — `%rsp` points at
 * `argc`, with `argv`, `envp` and the auxiliary vector above it — turned into
 * the C call `main(argc, argv, envp)`.
 *
 * `__libc_start_main` lives in `libc.so` and is UNDEFINED here, so this file is
 * also the reason a C program has a `JUMP_SLOT` relocation at all: it is
 * resolved, at launch, against the object table the spawner filled.
 *
 * `main` travels the other way, as an ARGUMENT.  The library must not
 * reference it: `main` is defined in the program, and a libc that named it
 * would be a shared object with an undefined symbol that only ONE of its
 * consumers could ever satisfy.  Passing it is what every system does, and the
 * reason is this one.
 */

.global _start
.extern __libc_start_main
.extern main

.section .text._start
_start:
    xorq  %rbp, %rbp              /* the outermost frame has no caller      */
    movq  %rsp, %rdi              /* the stack IS the argument              */
    leaq  main(%rip), %rsi        /* ...and `main` is PASSED, not looked up */
    andq  $-16, %rsp              /* the ABI wants 16 at a call             */
    call  __libc_start_main
    /* It does not return.  If it ever did, exiting is better than falling
     * into whatever follows. */
    movq  %rax, %rdi
    movq  $1, %rax
    syscall
    hlt

.section .note.GNU-stack,"",@progbits
