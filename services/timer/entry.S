/* SPDX-License-Identifier: Apache-2.0 */
/*
 * entry.S — timer service entry point (ledger A-24).
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers every cap as
 * a pre-start CSpace mint).  Aligns the stack, seeds the stack canary, passes
 * RBX as the first argument (RDI), and calls timer_main — a persistent service
 * loop that returns only on a SHUTDOWN request, after which we SYS_EXIT.
 *
 * The timer holds ONLY the caps its supervisor mints into its manifest: a
 * control endpoint, the timer IRQ capability, the notification that IRQ is
 * routed into, a reply object and a budget.  No spawn cap, no ioport, no VFS,
 * no KDEBUG — a service that can only measure time.
 */

.global _start
.extern timer_main

.section .text._start
_start:
    andq  $~0xFULL, %rsp
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
    call  timer_main

.timer_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .timer_dead

.section .note.GNU-stack,"",@progbits
