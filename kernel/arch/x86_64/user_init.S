.global user_init

.section .rodata
msg_hello: .asciz "Hello from ring 3!\n"

.section .text
user_init:
    movq $0, %rax
    leaq msg_hello(%rip), %rdi
    syscall

.loop:
    jmp .loop

.section .note.GNU-stack,"",@progbits
