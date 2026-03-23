/*
 * kbd_server.S — keyboard server in ring 3.
 *
 * Entry contract:
 *   RBX = private bootstrap channel from svcmgr
 *
 * Startup sequence:
 *   1. receive public service handle ("kbd") over bootstrap IPC
 *   2. receive reply channel handle ("kbd.reply") over bootstrap IPC
 *   3. close bootstrap channel
 *   4. enter recv loop on the bootstrapped service handle
 *
 * The service no longer uses SYS_NS_LOOKUP directly.  svcmgr remains the
 * userland discovery authority and bootstraps child services by attached
 * handle transfer over the private bootstrap channel.
 */
.global kbd_server

.section .text
kbd_server:
    movq  $0, %r12                     /* reply channel handle */
    movq  $0, %r13                     /* service/IRQ channel handle */

    subq  $128, %rsp
    andq  $~0xFULL, %rsp

    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'s',  4(%rdi)
    movb  $'t',  5(%rdi)
    movb  $'a',  6(%rdi)
    movb  $'r',  7(%rdi)
    movb  $'t',  8(%rdi)
    movb  $'\n', 9(%rdi)
    movb  $0,   10(%rdi)
    movq  $0,    %rax
    syscall

.kbd_bootstrap_recv:
    testq %r12, %r12
    jz    .kbd_bootstrap_need
    testq %r13, %r13
    jnz   .kbd_bootstrap_done
.kbd_bootstrap_need:
    movq  $0,  0(%rsp)
    movq  $0,  8(%rsp)
    movq  $0, 16(%rsp)
    movq  $0, 24(%rsp)
    movq  $0, 32(%rsp)
    movq  $0, 40(%rsp)
    movq  $0, 48(%rsp)
    movq  $0, 56(%rsp)
    movq  $0, 64(%rsp)
    movq  $0, 72(%rsp)
    movq  $0, 80(%rsp)

    movq  $14,  %rax
    movq  %rbx, %rdi
    movq  %rsp, %rsi
    movq  $0,   %rdx
    syscall
    testq %rax, %rax
    jnz   .kbd_bootstrap_fail

    cmpl  $0x8002, 0(%rsp)
    jne   .kbd_bootstrap_fail
    movl  76(%rsp), %eax
    testl %eax, %eax
    jz    .kbd_bootstrap_fail

    cmpl  $1, 8(%rsp)
    je    .kbd_bootstrap_service
    cmpl  $2, 8(%rsp)
    je    .kbd_bootstrap_reply
    jmp   .kbd_bootstrap_fail

.kbd_bootstrap_service:
    movq  %rax, %r13
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_reply:
    movq  %rax, %r12
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_done:
    movq  $15,  %rax
    movq  %rbx, %rdi
    syscall

    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'r',  4(%rdi)
    movb  $'e',  5(%rdi)
    movb  $'a',  6(%rdi)
    movb  $'d',  7(%rdi)
    movb  $'y',  8(%rdi)
    movb  $'\n', 9(%rdi)
    movb  $0,   10(%rdi)
    movq  $0,    %rax
    syscall

.kbd_recv:
    movq  $14,   %rax
    movq  %r13,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall
    testq %rax,  %rax
    jnz   .kbd_recv_err

    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'r',  4(%rdi)
    movb  $'e',  5(%rdi)
    movb  $'c',  6(%rdi)
    movb  $'v',  7(%rdi)
    movb  $'\n', 8(%rdi)
    movb  $0,    9(%rdi)
    movq  $0,    %rax
    syscall

    cmpl  $1,    0(%rsp)
    je    .kbd_hello
    cmpl  $2,    0(%rsp)
    je    .kbd_status
    cmpl  $4,    0(%rsp)
    jne   .kbd_recv

    movzbl 8(%rsp), %ecx
    testb  %cl,  %cl
    jz    .kbd_recv

    leaq  96(%rsp), %rdi
    movb  $'[',  0(%rdi)
    movb  $'K',  1(%rdi)
    movb  $'B',  2(%rdi)
    movb  $'D',  3(%rdi)
    movb  $']',  4(%rdi)
    movb  $' ',  5(%rdi)
    movb  $'0',  6(%rdi)
    movb  $'x',  7(%rdi)

    movl  %ecx,  %eax
    shrl  $4,    %eax
    andl  $0xF,  %eax
    cmpl  $10,   %eax
    jl    .hi_dec
    addl  $('A' - 10), %eax
    jmp   .hi_store
.hi_dec:
    addl  $'0',  %eax
.hi_store:
    movb  %al,   8(%rdi)

    movl  %ecx,  %eax
    andl  $0xF,  %eax
    cmpl  $10,   %eax
    jl    .lo_dec
    addl  $('A' - 10), %eax
    jmp   .lo_store
.lo_dec:
    addl  $'0',  %eax
.lo_store:
    movb  %al,   9(%rdi)
    movb  $'\n', 10(%rdi)
    movb  $0,    11(%rdi)

    movq  $0,    %rax
    movq  $0,    %rsi
    movq  $0,    %rdx
    syscall
    jmp   .kbd_recv

.kbd_hello:
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'b',  4(%rdi)
    movb  $'o',  5(%rdi)
    movb  $'o',  6(%rdi)
    movb  $'t',  7(%rdi)
    movb  $'s',  8(%rdi)
    movb  $'t',  9(%rdi)
    movb  $'r', 10(%rdi)
    movb  $'a', 11(%rdi)
    movb  $'p', 12(%rdi)
    movb  $' ', 13(%rdi)
    movb  $'O', 14(%rdi)
    movb  $'K', 15(%rdi)
    movb  $'\n',16(%rdi)
    movb  $0,   17(%rdi)
    movq  $0,    %rax
    syscall

    movq  $0,  0(%rsp)
    movq  $0,  8(%rsp)
    movq  $0, 16(%rsp)
    movq  $0, 24(%rsp)
    movq  $0, 32(%rsp)
    movq  $0, 40(%rsp)
    movq  $0, 48(%rsp)
    movq  $0, 56(%rsp)
    movq  $0, 64(%rsp)
    movq  $0, 72(%rsp)
    movq  $0, 80(%rsp)
    movl  $0x80000001, 0(%rsp)
    movb  $1,          8(%rsp)
    movl  $1,         72(%rsp)

    movq  $13,   %rax
    movq  %r12,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall

    movq  $8,    %rax
    movq  $1,    %rdi
    syscall
    jmp   .kbd_recv

.kbd_recv_err:
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'r',  4(%rdi)
    movb  $'e',  5(%rdi)
    movb  $'c',  6(%rdi)
    movb  $'v',  7(%rdi)
    movb  $' ',  8(%rdi)
    movb  $'E',  9(%rdi)
    movb  $'R', 10(%rdi)
    movb  $'R', 11(%rdi)
    movb  $'\n',12(%rdi)
    movb  $0,   13(%rdi)
    movq  $0,    %rax
    syscall
    jmp   .kbd_recv

.kbd_status:
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'s',  4(%rdi)
    movb  $'t',  5(%rdi)
    movb  $'a',  6(%rdi)
    movb  $'t',  7(%rdi)
    movb  $'u',  8(%rdi)
    movb  $'s',  9(%rdi)
    movb  $' ', 10(%rdi)
    movb  $'O', 11(%rdi)
    movb  $'K', 12(%rdi)
    movb  $'\n',13(%rdi)
    movb  $0,   14(%rdi)
    movq  $0,    %rax
    syscall

    movq  $0,  0(%rsp)
    movq  $0,  8(%rsp)
    movq  $0, 16(%rsp)
    movq  $0, 24(%rsp)
    movq  $0, 32(%rsp)
    movq  $0, 40(%rsp)
    movq  $0, 48(%rsp)
    movq  $0, 56(%rsp)
    movq  $0, 64(%rsp)
    movq  $0, 72(%rsp)
    movq  $0, 80(%rsp)
    movl  $0x80000002, 0(%rsp)
    movb  $0x03,       8(%rsp)
    movl  $1,         72(%rsp)

    movq  $13,   %rax
    movq  %r12,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall

    movq  $8,    %rax
    movq  $1,    %rdi
    syscall
    jmp   .kbd_recv

.kbd_bootstrap_fail:
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'b',  4(%rdi)
    movb  $'o',  5(%rdi)
    movb  $'o',  6(%rdi)
    movb  $'t',  7(%rdi)
    movb  $' ',  8(%rdi)
    movb  $'F',  9(%rdi)
    movb  $'A', 10(%rdi)
    movb  $'I', 11(%rdi)
    movb  $'L', 12(%rdi)
    movb  $'\n',13(%rdi)
    movb  $0,   14(%rdi)
    movq  $0,    %rax
    syscall

.kbd_dead:
    movq  $1,    %rax
    movq  $0,    %rdi
    syscall
    hlt
    jmp   .kbd_dead

.section .note.GNU-stack,"",@progbits
