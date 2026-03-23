.global svcmgr_main
.extern svcmgr_main_c

.section .text
svcmgr_main:
    andq  $~0xFULL, %rsp
    movq  %rbx, %rdi
    call  svcmgr_main_c

.svcmgr_dead:
    movq  $1, %rax
    movq  $1, %rdi
    syscall
    hlt
    jmp   .svcmgr_dead

.section .note.GNU-stack,"",@progbits
