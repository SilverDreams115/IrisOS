/*
 * entry.S — shell service entry point.
 *
 * RBX = bootstrap KChannel handle (svc_loader convention).
 * Aligns the stack, passes RBX as first argument (RDI), calls sh_main_c,
 * then performs a clean SYS_EXIT.
 */

.global _start
.extern sh_main_c

.section .text._start
_start:
    andq  $~0xFULL, %rsp
    movq  %rbx, %rdi
    call  sh_main_c

.sh_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .sh_dead

.section .note.GNU-stack,"",@progbits
