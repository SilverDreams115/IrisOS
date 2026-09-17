/*
 * entry.S — e1000 network service entry point (Stage 10).
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers every cap as
 * a pre-start CSpace mint).  Aligns the stack, seeds the stack canary, passes
 * RBX as the first argument (RDI), and calls net_main — a persistent service
 * loop that returns only on a SHUTDOWN request, after which we SYS_EXIT.
 *
 * Same manifest as the disk service, and for the same reasons: the endpoint it
 * serves on, a reply object, an endpoint to the bus service, the authority to
 * contain its own controller's DMA, and a budget.  A network card is the
 * clearest case for that last one — it reads a RING of physical addresses
 * continuously and nothing tells it to stop.
 */

.global _start
.extern net_main

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
    call  net_main

.net_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .net_dead

.section .note.GNU-stack,"",@progbits
