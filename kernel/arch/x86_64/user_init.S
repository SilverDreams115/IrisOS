.global user_init

.section .rodata
msg_start:      .asciz "init bootstrap start\n"
msg_lookup_ok:  .asciz "kbd lookup OK\n"
msg_reply_ok:   .asciz "kbd.reply lookup OK\n"
msg_hello_ok:   .asciz "kbd hello reply OK\n"
msg_status_ok:  .asciz "kbd status reply OK\n"
msg_lookup_err: .asciz "kbd lookup FAILED\n"
msg_reply_err:  .asciz "kbd.reply lookup FAILED\n"
msg_hello_err:  .asciz "kbd hello FAILED\n"
msg_status_err: .asciz "kbd status FAILED\n"
svc_kbd:        .asciz "kbd"
svc_kbd_reply:  .asciz "kbd.reply"

.section .text
user_init:
    /* SYS_WRITE */
    movq $0, %rax
    leaq msg_start(%rip), %rdi
    syscall

    /* SYS_NS_LOOKUP("kbd", RIGHT_WRITE) */
    movq $25, %rax
    leaq svc_kbd(%rip), %rdi
    movq $2, %rsi
    syscall
    testq %rax, %rax
    js .lookup_fail
    movq %rax, %rbx

    /* SYS_WRITE */
    movq $0, %rax
    leaq msg_lookup_ok(%rip), %rdi
    syscall

    /* SYS_NS_LOOKUP("kbd.reply", RIGHT_READ) */
    movq $25, %rax
    leaq svc_kbd_reply(%rip), %rdi
    movq $1, %rsi
    syscall
    testq %rax, %rax
    js .reply_fail
    movq %rax, %r12

    movq $0, %rax
    leaq msg_reply_ok(%rip), %rdi
    syscall

    subq $80, %rsp
    andq $~0xFULL, %rsp

    /* KBD_OP_HELLO */
    movq $0,  0(%rsp)
    movq $0,  8(%rsp)
    movq $0, 16(%rsp)
    movq $0, 24(%rsp)
    movq $0, 32(%rsp)
    movq $0, 40(%rsp)
    movq $0, 48(%rsp)
    movq $0, 56(%rsp)
    movq $0, 64(%rsp)
    movq $0, 72(%rsp)
    movl $1,  0(%rsp)
    movb $'H', 8(%rsp)
    movl $1, 72(%rsp)

    /* SYS_CHAN_SEND(handle, &msg) */
    movq $13, %rax
    movq %rbx, %rdi
    movq %rsp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .hello_fail

    /* SYS_CHAN_RECV(reply_handle, &msg) */
    movq $14, %rax
    movq %r12, %rdi
    movq %rsp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .hello_fail

    cmpl $0x80000001, 0(%rsp)
    jne .hello_fail
    cmpb $1, 8(%rsp)
    jne .hello_fail

    movq $0, %rax
    leaq msg_hello_ok(%rip), %rdi
    syscall

    /* KBD_OP_GET_STATUS */
    movq $0,  0(%rsp)
    movq $0,  8(%rsp)
    movq $0, 16(%rsp)
    movq $0, 24(%rsp)
    movq $0, 32(%rsp)
    movq $0, 40(%rsp)
    movq $0, 48(%rsp)
    movq $0, 56(%rsp)
    movq $0, 64(%rsp)
    movq $0, 72(%rsp)
    movl $2,  0(%rsp)
    movl $0, 72(%rsp)

    movq $13, %rax
    movq %rbx, %rdi
    movq %rsp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .status_fail

    movq $14, %rax
    movq %r12, %rdi
    movq %rsp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .status_fail

    cmpl $0x80000002, 0(%rsp)
    jne .status_fail
    cmpb $0x03, 8(%rsp)
    jne .status_fail

    movq $0, %rax
    leaq msg_status_ok(%rip), %rdi
    syscall

    /* SYS_HANDLE_CLOSE(handle) */
    movq $15, %rax
    movq %rbx, %rdi
    syscall
    movq $15, %rax
    movq %r12, %rdi
    syscall

    /* Give the service time to print its protocol logs. */
    movq $8, %rax
    movq $5, %rdi
    syscall

    /* SYS_WRITE */
    jmp .exit_ok

.lookup_fail:
    movq $0, %rax
    leaq msg_lookup_err(%rip), %rdi
    syscall
    movq $1, %rax
    movq $1, %rdi
    syscall

.reply_fail:
    movq $0, %rax
    leaq msg_reply_err(%rip), %rdi
    syscall
    movq $15, %rax
    movq %rbx, %rdi
    syscall
    movq $1, %rax
    movq $2, %rdi
    syscall

.hello_fail:
    movq $0, %rax
    leaq msg_hello_err(%rip), %rdi
    syscall
    movq $15, %rax
    movq %rbx, %rdi
    syscall
    movq $15, %rax
    movq %r12, %rdi
    syscall
    movq $1, %rax
    movq $3, %rdi
    syscall

.status_fail:
    movq $0, %rax
    leaq msg_status_err(%rip), %rdi
    syscall
    movq $15, %rax
    movq %rbx, %rdi
    syscall
    movq $15, %rax
    movq %r12, %rdi
    syscall
    movq $1, %rax
    movq $4, %rdi
    syscall

.exit_ok:
    movq $1, %rax
    movq $0, %rdi
    syscall

    /* should not reach here */
.dead:
    hlt
    jmp .dead

.section .note.GNU-stack,"",@progbits
