/* SPDX-License-Identifier: Apache-2.0 */
/*
 * entry.S — the shared-object registry's entry point.
 *
 * An ordinary service entry: RBX carries nothing and every capability arrived
 * as a pre-start CSpace mint.  What it holds is an endpoint to serve on, a
 * reply object, an endpoint to the VFS, and a budget — and the budget is the
 * interesting one, because it is where every shared library in the system
 * lives.  A registry with a small budget is a machine that can hold few
 * libraries, which is a fact a person can see rather than a limit compiled in.
 */

.global _start
.extern objreg_main

.section .text._start
_start:
    andq  $-16, %rsp
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
    movq  %rbx, %rdi
    call  objreg_main

.objreg_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .objreg_dead

.section .note.GNU-stack,"",@progbits
