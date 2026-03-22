.global syscall_entry
.extern syscall_dispatch

.section .bss
.align 16
syscall_kstack_base:
    .space 4096
syscall_kstack_top:

.section .data
.align 8
.global syscall_kstack_ptr
syscall_kstack_ptr:
    .quad syscall_kstack_top

.section .text
syscall_entry:
    /* save user rsp, switch to current task kernel stack */
    movq %rsp, %r10
    movq syscall_kstack_ptr(%rip), %rsp

    /* save syscall frame */
    pushq %r10        /* user rsp    */
    pushq %r11        /* user rflags */
    pushq %rcx        /* user rip    */
    pushq %rdx        /* arg2        */
    pushq %rsi        /* arg1        */
    pushq %rdi        /* arg0        */
    pushq %rax        /* syscall no  */

    /* align stack for C call */
    subq $8, %rsp

    /* syscall_dispatch(num, arg0, arg1, arg2) */
    movq  8(%rsp), %rdi
    movq 16(%rsp), %rsi
    movq 24(%rsp), %rdx
    movq 32(%rsp), %rcx
    call syscall_dispatch
    /* return value already in rax */

    addq $8, %rsp

    /* recover saved user context */
    movq 32(%rsp), %r8     /* user rip */
    movq 40(%rsp), %r9     /* user rflags */
    movq 48(%rsp), %r10    /* user rsp */

    /* drop saved frame */
    addq $56, %rsp

    /* build ring3 iret frame: SS, RSP, RFLAGS, CS, RIP */
    pushq $0x23
    pushq %r10
    pushq %r9
    pushq $0x1B
    pushq %r8

    iretq

.section .note.GNU-stack,"",@progbits
