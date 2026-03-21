.global syscall_entry
.extern syscall_dispatch

/*
 * syscall_entry — IrisOS MSR-based syscall handler
 *
 * CPU state on entry (syscall instruction):
 *   rax = syscall number
 *   rdi = arg0, rsi = arg1, rdx = arg2
 *   rcx = saved user RIP   (by CPU)
 *   r11 = saved user RFLAGS (by CPU)
 *   rsp = user stack        (unchanged by CPU)
 *   IF  = 0                 (SFMASK cleared IF)
 *
 * Kernel stack: we use r10 (caller-saved, not an arg) as scratch to
 * save user rsp, then load the per-task kernel stack top from the
 * static variable syscall_kstack, which is updated on every context
 * switch by tss_set_rsp0() → syscall_set_kstack().
 *
 * Save frame layout (rsp+0 = top after all pushes):
 *   [rsp+ 0] = syscall number  (rax)
 *   [rsp+ 8] = arg0            (rdi)
 *   [rsp+16] = arg1            (rsi)
 *   [rsp+24] = arg2            (rdx)
 *   [rsp+32] = user rip        (rcx)
 *   [rsp+40] = user rflags     (r11)
 *   [rsp+48] = user rsp        (r10)
 */

.section .bss
.align 16
syscall_kstack_base:
    .space 4096
syscall_kstack_top:          /* fallback stack, overwritten per task */

.section .data
.align 8
.global syscall_kstack_ptr
syscall_kstack_ptr:
    .quad syscall_kstack_top  /* updated by syscall_set_kstack() on ctx switch */

.section .text
syscall_entry:
    /* 1. save user rsp, switch to per-task kernel stack */
    movq  %rsp, %r10
    movq  syscall_kstack_ptr(%rip), %rsp

    /* 2. build save frame (7 pushes = 56 bytes) */
    pushq %r10               /* [+48] user rsp    */
    pushq %r11               /* [+40] user rflags */
    pushq %rcx               /* [+32] user rip    */
    pushq %rdx               /* [+24] arg2        */
    pushq %rsi               /* [+16] arg1        */
    pushq %rdi               /* [+ 8] arg0        */
    pushq %rax               /* [+ 0] syscall num */

    /* 3. 16-byte align for C ABI (56 bytes pushed: need 8 more) */
    subq  $8, %rsp

    /* 4. syscall_dispatch(num, arg0, arg1, arg2) */
    movq   8(%rsp), %rdi     /* num  */
    movq  16(%rsp), %rsi     /* arg0 */
    movq  24(%rsp), %rdx     /* arg1 */
    movq  32(%rsp), %rcx     /* arg2 */
    call  syscall_dispatch   /* return value → rax */

    /* 5. undo alignment */
    addq  $8, %rsp

    /* 6. restore for sysretq */
    addq  $8,  %rsp          /* skip saved num (return val already in rax) */
    popq  %rdi               /* discard arg0 */
    popq  %rsi               /* discard arg1 */
    popq  %rdx               /* discard arg2 */
    popq  %rcx               /* user rip   → rcx */
    popq  %r11               /* user rflags → r11 */
    popq  %rsp               /* user rsp */

    sysretq

.section .note.GNU-stack,"",@progbits
