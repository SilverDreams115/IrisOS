/*
 * entry.S — IP stack service entry point (Stage 10).
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers every cap as
 * a pre-start CSpace mint).  Aligns the stack, seeds the stack canary, passes
 * RBX as the first argument (RDI), and calls ip_main — a persistent service
 * loop that returns only on a SHUTDOWN request, after which we SYS_EXIT.
 *
 * It holds the endpoint it serves on, a reply object, an endpoint to the
 * NETWORK driver, and a budget.  No ports, no controller, no device Untyped,
 * no DMA authority.  A compromised IP stack can send and receive frames on one
 * interface; it cannot reprogram the card, reach another device, or make the
 * NIC write anywhere — the driver already decided what the hardware may touch
 * and this service is downstream of that decision.
 */

.global _start
.extern ip_main

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
    call  ip_main

.ip_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .ip_dead

.section .note.GNU-stack,"",@progbits
