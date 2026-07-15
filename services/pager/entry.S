/*
 * entry.S — pager service entry point (Fase 27 model, promoted to its own
 * binary in Fase 28 once the initrd boot-growth fragility was fixed).
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers every cap as
 * a pre-start CSpace mint).  Aligns the stack, seeds the stack canary, passes
 * RBX as the first argument (RDI), and calls pager_main — a persistent service
 * loop that returns only on a SHUTDOWN request, after which we SYS_EXIT.
 *
 * The pager holds ONLY the caps its supervisor mints into its manifest (a
 * control endpoint, per-target proc/vspace/fault-notification grants, and
 * per-VMO page-source grants).  No spawn cap, no device caps, no untyped, no
 * KDEBUG.
 */

.global _start
.extern pager_main

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
    call  pager_main

.pager_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .pager_dead

.section .note.GNU-stack,"",@progbits
