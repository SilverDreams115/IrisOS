/* SPDX-License-Identifier: Apache-2.0 */
/*
 * entry.S — the shared-object consumer's entry point.
 *
 * A service is started with RBX carrying nothing and a bare stack: every
 * capability it has arrived as a pre-start CSpace mint, and it knows where
 * they are because its own header says so.
 *
 * A program is started the way the System V AMD64 ABI says a program is:
 * `%rsp` points at `argc`, with `argv`, `envp` and the auxiliary vector above
 * it.  That vector is how it learns everything it was not compiled knowing —
 * where its own program headers are, where its interpreter was loaded, and
 * which CSpace slots hold its object table and its budget.
 *
 * So this stub does one thing a service's does not: it passes `%rsp` on, as
 * the first argument, because the stack IS the argument.  Nothing else can be
 * assumed at this point — not a heap, not a thread pointer, not even that
 * `%rsp` is where the last program's was.
 */

.global _start
.extern libuser_main
.extern __stack_chk_guard

.text
_start:
    movq  %rsp, %rdi          /* the stack is the argument */
    andq  $-16, %rsp      /* the ABI promises 16; keep it that way */

    /* The canary, seeded the same way every image here seeds it: this tree
     * builds with -mstack-protector-guard=global because ring 3 had no thread
     * pointer, and a program is no different until its runtime sets one. */
    pushq %rdi
    pushq %rbx
    movl  $1, %eax
    cpuid
    movl  %ecx, %eax
    popq  %rbx
    btl   $30, %eax
    jnc   2f
    rdrand %rax
    jc    3f
2:  rdtsc
    shlq  $32, %rdx
    orq   %rdx, %rax
3:  movq  %rax, __stack_chk_guard(%rip)
    popq  %rdi

    call  libuser_main
    /* Its return value IS the exit status: that is the whole contract a
     * program has with whoever spawned it. */
    movq  %rax, %rdi
    movq  $1, %rax            /* SYS_EXIT */
    syscall

.alloc_dead:
    hlt
    jmp   .alloc_dead

.section .note.GNU-stack,"",@progbits
