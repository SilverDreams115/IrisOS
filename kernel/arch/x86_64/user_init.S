.global user_init

.section .rodata
msg_start:      .asciz "init bootstrap start\n"
msg_lookup_ok:  .asciz "kbd lookup OK\n"
msg_send_ok:    .asciz "kbd handshake sent\n"
msg_lookup_err: .asciz "kbd lookup FAILED\n"
msg_send_err:   .asciz "kbd handshake FAILED\n"
svc_kbd:        .asciz "kbd"

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

    /* Build KChanMsg on stack:
     *   type      = 1  (bootstrap handshake)
     *   sender_id = 0
     *   data[0]   = 'H'
     *   data_len  = 1
     */
    subq $80, %rsp
    andq $~0xFULL, %rsp
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
    jnz .send_fail

    /* SYS_WRITE */
    movq $0, %rax
    leaq msg_send_ok(%rip), %rdi
    syscall

    /* SYS_HANDLE_CLOSE(handle) */
    movq $15, %rax
    movq %rbx, %rdi
    syscall

    /* Give the service time to run and print its handshake log. */
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

.send_fail:
    movq $0, %rax
    leaq msg_send_err(%rip), %rdi
    syscall
    movq $15, %rax
    movq %rbx, %rdi
    syscall
    movq $1, %rax
    movq $2, %rdi
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
