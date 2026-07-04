/*
 * entry.S — lifecycle_probe test child entry point.
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers all caps as
 * pre-start CSpace mints).  Aligns the stack, seeds the stack canary, passes RBX
 * as the first argument (RDI), calls lp_main, then performs a clean SYS_EXIT.
 *
 * This is a TEST payload, not a productive service: it is only spawned by the
 * ring-3 iris_test suite to exercise cross-process lifecycle paths.
 */

.global _start
.extern lp_main

.section .text._start
_start:
    andq  $~0xFULL, %rsp
    /* Seed stack canary: RDRAND (hardware TRNG) with CPUID check + RDTSC fallback */
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
    call  lp_main

.lp_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .lp_dead

.section .note.GNU-stack,"",@progbits
