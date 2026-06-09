/*
 * entry.S — iris_test service entry point.
 *
 * RBX = bootstrap KChannel handle (svc_loader convention).
 * Aligns the stack, seeds the stack canary, passes RBX as first argument (RDI),
 * calls iris_test_main, then performs a clean SYS_EXIT.
 */

.global _start
.extern iris_test_main

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
    call  iris_test_main

.iris_test_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .iris_test_dead

.section .note.GNU-stack,"",@progbits
