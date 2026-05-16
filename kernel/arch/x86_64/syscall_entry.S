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
    /* save user rsp in r8 (scratch); r10 is now arg3 from user */
    movq %rsp, %r8
    movq syscall_kstack_ptr(%rip), %rsp

    /* save 4-arg syscall frame:
     *   [rsp+64] user rsp    (saved from r8)
     *   [rsp+56] user rflags (r11, set by syscall insn)
     *   [rsp+48] user rip    (rcx, set by syscall insn)
     *   [rsp+40] arg3        (r10, 4th user arg)
     *   [rsp+32] arg2        (rdx)
     *   [rsp+24] arg1        (rsi)
     *   [rsp+16] arg0        (rdi)
     *   [rsp+8]  syscall num (rax)
     *   [rsp+0]  alignment pad
     */
    pushq %r8         /* user rsp    */
    pushq %r11        /* user rflags */
    pushq %rcx        /* user rip    */
    pushq %r10        /* arg3        */
    pushq %rdx        /* arg2        */
    pushq %rsi        /* arg1        */
    pushq %rdi        /* arg0        */
    pushq %rax        /* syscall no  */
    subq $8, %rsp     /* alignment   */

    /* syscall_dispatch(num, arg0, arg1, arg2, arg3) */
    movq  8(%rsp), %rdi   /* num  */
    movq 16(%rsp), %rsi   /* arg0 */
    movq 24(%rsp), %rdx   /* arg1 */
    movq 32(%rsp), %rcx   /* arg2 */
    movq 40(%rsp), %r8    /* arg3 */
    call syscall_dispatch
    /* return value in rax */

    addq $8, %rsp

    /* recover saved user context:
     * after addq $8: rsp+40=user_rip, rsp+48=user_rflags, rsp+56=user_rsp */
    movq 40(%rsp), %r8     /* user rip    */
    movq 48(%rsp), %r9     /* user rflags */
    movq 56(%rsp), %r10    /* user rsp    */
    addq $64, %rsp

    /* build ring3 iret frame: SS, RSP, RFLAGS, CS, RIP */
    pushq $0x23
    pushq %r10
    pushq %r9
    pushq $0x1B
    pushq %r8

    iretq

.section .note.GNU-stack,"",@progbits
