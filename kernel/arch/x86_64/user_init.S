.global user_init

.section .rodata
msg_hello: .asciz "Hello from ring 3!\n"

.section .text
user_init:
    /* SYS_WRITE */
    movq $0, %rax
    leaq msg_hello(%rip), %rdi
    syscall

    /* SYS_EXIT */
    movq $1, %rax
    movq $0, %rdi
    syscall

    /* should not reach here */
.dead:
    hlt
    jmp .dead

.section .note.GNU-stack,"",@progbits
