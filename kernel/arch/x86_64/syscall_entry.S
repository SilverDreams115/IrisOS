.global syscall_entry
.extern syscall_dispatch

/*
 * syscall_entry — IrisOS syscall handler
 *
 * On entry (from syscall instruction):
 *   rax = syscall number
 *   rdi = arg0, rsi = arg1, rdx = arg2
 *   rcx = saved user RIP  (by CPU)
 *   r11 = saved user RFLAGS (by CPU)
 *   rsp = user stack
 *
 * We switch to a static kernel stack, save state, call C dispatcher,
 * restore state, and sysretq back to user.
 */

.section .bss
.align 16
.space 4096
syscall_kstack_top:

.section .text
syscall_entry:
    /* 1. save user rsp in r10, switch to kernel stack */
    movq  %rsp, %r10
    leaq  syscall_kstack_top(%rip), %rsp

    /* 2. save everything we need to restore for sysret */
    pushq %r10               /* user rsp */
    pushq %r11               /* user rflags */
    pushq %rcx               /* user rip */
    pushq %rdi               /* arg0 */
    pushq %rsi               /* arg1 */
    pushq %rdx               /* arg2 */
    pushq %rax               /* syscall number */

    /* 3. call syscall_dispatch(num, arg0, arg1, arg2)
     *    C calling convention: rdi=num, rsi=arg0, rdx=arg1, rcx=arg2
     */
    movq  %rax, %rdi         /* num */
    movq  32(%rsp), %rsi     /* arg0 (saved rdi) — offset from current rsp */
    movq  40(%rsp), %rdx     /* arg1 (saved rsi) */
    movq  48(%rsp), %rcx     /* arg2 (saved rdx) — wait, stack grows down */

    /* Stack layout after 7 pushes (each 8 bytes):
     * rsp+0  = rax (num)
     * rsp+8  = rdx (arg2)
     * rsp+16 = rsi (arg1)
     * rsp+24 = rdi (arg0)
     * rsp+32 = rcx (user rip)
     * rsp+40 = r11 (user rflags)
     * rsp+48 = r10 (user rsp)
     */
    movq  0(%rsp),  %rdi     /* num */
    movq  24(%rsp), %rsi     /* arg0 */
    movq  16(%rsp), %rdx     /* arg1 */
    movq  8(%rsp),  %rcx     /* arg2 */
    call  syscall_dispatch   /* rax = return value */

    /* 4. restore for sysret */
    addq  $8, %rsp           /* skip saved rax */
    popq  %rdx               /* arg2 — discard */
    popq  %rsi               /* arg1 — discard */
    popq  %rdi               /* arg0 — discard */
    popq  %rcx               /* user rip */
    popq  %r11               /* user rflags */
    popq  %rsp               /* user rsp */

    /* rax already has return value from syscall_dispatch */
    sysretq

.section .note.GNU-stack,"",@progbits
