#include <iris/syscall.h>

.global _start
.extern iris_userboot_main

.section .text
_start:
    movq  %rbx, %rdi
    call  iris_userboot_main
    movq  $SYS_EXIT, %rax
    movq  $0, %rdi
    syscall
.L_dead:
    hlt
    jmp   .L_dead

.section .note.GNU-stack,"",@progbits
