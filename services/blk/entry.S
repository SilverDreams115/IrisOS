/*
 * entry.S — AHCI disk service entry point (Stage 10).
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers every cap as
 * a pre-start CSpace mint).  Aligns the stack, seeds the stack canary, passes
 * RBX as the first argument (RDI), and calls blk_main — a persistent service
 * loop that returns only on a SHUTDOWN request, after which we SYS_EXIT.
 *
 * It holds ONLY the caps its supervisor mints into its manifest: the endpoint
 * it serves on, a reply object, an endpoint to the bus service, the authority
 * to contain its own controller's DMA, and a budget.  No ports, no IRQ, no
 * spawn cap, no VFS, no KDEBUG — it cannot even find its controller without
 * asking somebody else.
 */

.global _start
.extern blk_main

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
    call  blk_main

.blk_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .blk_dead

.section .note.GNU-stack,"",@progbits
