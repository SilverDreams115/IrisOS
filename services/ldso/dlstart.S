/* SPDX-License-Identifier: Apache-2.0 */
/*
 * dlstart.S — the interpreter's first instruction, and its self-relocation.
 *
 * ── Why this is assembly ───────────────────────────────────────────────────
 *
 * Because until it has run, NO GLOBAL IN THIS IMAGE IS TRUSTWORTHY.  An
 * interpreter is an `ET_DYN` object loaded at a bias nobody told it, and every
 * pointer its compiler emitted into `.data.rel.ro` or reached through the GOT
 * still holds a link-time address.  C that looks harmless — a string literal
 * stored in a struct, a table of function pointers, a `static const char *` —
 * compiles into exactly those, and the failure is a jump to a small number
 * with no way to report anything.
 *
 * So the self-relocation is written where the instruction stream is the only
 * thing being trusted: locals in registers, and RIP-relative access to
 * nothing.  Once it is done, C is safe and everything else is C.
 *
 * ── How it learns where it is ──────────────────────────────────────────────
 *
 * From `AT_BASE` on the initial stack, which the spawner put there because the
 * spawner is the one thing that knows.  The alternative — deriving the base
 * from `GOT[0]`, which holds the link-time address of `_DYNAMIC` — is the
 * classic trick and needs no cooperation, but it needs a GOT entry to be
 * readable before relocation and that is one more thing to be quietly wrong
 * about.  `AT_BASE` is on the stack, in the open, and is what `AT_BASE` is for.
 *
 * ── What it applies ────────────────────────────────────────────────────────
 *
 * `R_X86_64_RELATIVE` only, over its OWN `DT_RELA` table.  Anything else in
 * its own image would mean an interpreter that needs an interpreter.
 */

#define AT_NULL      0
#define AT_BASE      7
#define PT_DYNAMIC   2
#define DT_NULL      0
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define R_RELATIVE   8

.global _start
.extern dl_main

.section .text._start
_start:
    movq  %rsp, %rbx              /* the initial stack, kept for the program */

    /* ── walk the stack to AT_BASE ──────────────────────────────────── */
    movq  (%rsp), %rcx            /* argc                                  */
    leaq  8(%rsp,%rcx,8), %rsi    /* &argv[argc]  -> the NULL after argv    */
    addq  $8, %rsi                /* envp[0]                                */
1:  cmpq  $0, (%rsi)
    je    2f
    addq  $8, %rsi
    jmp   1b
2:  addq  $8, %rsi                /* auxv[0]                                */
    xorq  %r12, %r12              /* r12 = our base, 0 until found          */
3:  movq  (%rsi), %rax
    cmpq  $AT_NULL, %rax
    je    4f
    cmpq  $AT_BASE, %rax
    jne   5f
    movq  8(%rsi), %r12
    jmp   4f
5:  addq  $16, %rsi
    jmp   3b
4:
    testq %r12, %r12
    jz    .Lgo                    /* no interpreter base: nothing to fix up */

    /* ── find our own PT_DYNAMIC ────────────────────────────────────────
     * Our ELF header is at base+0: the program linker script puts the headers
     * inside the first PT_LOAD, which is the same property that makes AT_PHDR
     * a mapped address. */
    movzwl 0x38(%r12), %ecx
    movzwl 0x36(%r12), %edx       /* e_phentsize                            */
    movq  0x20(%r12), %rsi        /* e_phoff                                */
    addq  %r12, %rsi              /* -> the program header table            */
    xorq  %r13, %r13              /* r13 = _DYNAMIC vaddr, 0 until found    */
6:  testl %ecx, %ecx
    jz    7f
    cmpl  $PT_DYNAMIC, (%rsi)     /* p_type                                 */
    jne   8f
    movq  0x10(%rsi), %r13        /* p_vaddr                                */
    jmp   7f
8:  addq  %rdx, %rsi
    decl  %ecx
    jmp   6b
7:
    testq %r13, %r13
    jz    .Lgo

    /* ── read DT_RELA / DT_RELASZ / DT_RELAENT ───────────────────────── */
    addq  %r12, %r13              /* -> our dynamic section                 */
    xorq  %r8,  %r8               /* rela vaddr                             */
    xorq  %r9,  %r9               /* rela bytes                             */
    movq  $24, %r10               /* rela entsize, default sizeof(Elf64_Rela)*/
9:  movq  (%r13), %rax
    cmpq  $DT_NULL, %rax
    je    10f
    cmpq  $DT_RELA, %rax
    jne   11f
    movq  8(%r13), %r8
    jmp   12f
11: cmpq  $DT_RELASZ, %rax
    jne   13f
    movq  8(%r13), %r9
    jmp   12f
13: cmpq  $DT_RELAENT, %rax
    jne   12f
    movq  8(%r13), %r10
12: addq  $16, %r13
    jmp   9b
10:
    testq %r9, %r9
    jz    .Lgo
    testq %r10, %r10
    jz    .Lgo

    /* ── apply every R_X86_64_RELATIVE to ourselves ──────────────────── */
    addq  %r12, %r8               /* -> our relocation table                */
14: testq %r9, %r9
    jz    .Lgo
    movq  8(%r8), %rax            /* r_info                                 */
    cmpl  $R_RELATIVE, %eax       /* the TYPE is the low 32 bits            */
    jne   15f
    movq  (%r8), %rcx             /* r_offset                               */
    movq  16(%r8), %rdx           /* r_addend                               */
    addq  %r12, %rdx              /* base + addend                          */
    movq  %rdx, (%r12,%rcx,1)     /* ...written at base + r_offset          */
15: addq  %r10, %r8
    subq  %r10, %r9
    jmp   14b

.Lgo:
    /*
     * Relocated.  C is safe from here, and `dl_main` answers the address to
     * enter the PROGRAM at — which it reads off the same stack, because
     * `AT_ENTRY` is the spawner's statement of where the program begins and
     * this interpreter has no other way to know.
     */
    movq  %rbx, %rdi              /* the initial stack                      */
    andq  $-16, %rsp              /* the ABI wants 16 at a call             */
    call  dl_main

    testq %rax, %rax
    jz    .Ldead                  /* nothing to enter: fall into exit       */

    /*
     * Hand the program the stack it was PROMISED, not the one this function
     * left behind.  A runtime's entry reads `argc` off `%rsp`; anything else
     * there is a program reading this interpreter's frame.
     */
    movq  %rbx, %rsp
    xorq  %rdx, %rdx              /* no atexit handler from us              */
    xorq  %rbp, %rbp
    jmp   *%rax

.Ldead:
    movq  $1, %rax                /* SYS_EXIT                               */
    movq  $70, %rdi               /* "the interpreter could not start it"   */
    syscall
    hlt
    jmp   .Ldead

.section .note.GNU-stack,"",@progbits
