.global syscall_entry
.extern syscall_dispatch

.section .bss
.align 16
syscall_kstack_base:
    .space 4096
syscall_kstack_top:

.section .data
/*
 * Shadow globals — kept in sync by syscall_set_kstack / syscall_set_user_cr3
 * in syscall_dispatch.c for debugging; the live path uses GS-relative reads.
 */
.align 8
.global syscall_kstack_ptr
syscall_kstack_ptr:
    .quad syscall_kstack_top
.align 8
.global syscall_user_cr3
syscall_user_cr3:
    .quad 0

.section .text
/*
 * SWAPGS ABI (Fase 2):
 *   Ring-0 resting: GS_BASE = &cpu_local[cpu_id], KGS_BASE = 0.
 *   Ring-3 user:    GS_BASE = 0,                  KGS_BASE = &cpu_local[cpu_id].
 *
 *   On SYSCALL from ring-3: GS_BASE=0, KGS_BASE=&cpu_local.
 *   Entry SWAPGS → GS_BASE=&cpu_local, KGS_BASE=0 — kernel can use %gs.
 *   Exit  SWAPGS → GS_BASE=0, KGS_BASE=&cpu_local — user sees null GS.
 *
 *   Offsets used:
 *     %gs:48  = cpu_local.syscall_kstack   (kernel stack top for current task)
 *     %gs:56  = cpu_local.syscall_user_cr3 (user CR3 for current task)
 */
syscall_entry:
    /* SWAPGS first: GS_BASE ↔ KERNEL_GS_BASE → %gs = &cpu_local[0] */
    swapgs

    /* save user rsp in r8 (caller-saved scratch); load kernel stack */
    movq %rsp, %r8
    movq %gs:48, %rsp           /* cpu_local.syscall_kstack */

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
     * after addq $8: rsp+40=user_rip, rsp+48=user_rflags, rsp+56=user_rsp
     * rax = syscall return value — must NOT be clobbered until sysretq */
    movq 40(%rsp), %rcx    /* user rip    → rcx (sysretq restores RIP from RCX) */
    movq 48(%rsp), %r11    /* user rflags → r11 (sysretq restores RFLAGS from R11) */
    movq 56(%rsp), %r9     /* user rsp    → r9 (scratch; not consumed by sysretq) */
    addq $64, %rsp

    /* switch to user CR3 using r8 as scratch; preserves rax (return value) */
    movq %gs:56, %r8        /* cpu_local.syscall_user_cr3 */
    testq %r8, %r8
    jz .Lsyscall_no_user_cr3
    movq %r8, %cr3
.Lsyscall_no_user_cr3:

    /* validate RCX canonical: bits[63:47] must all be 0 for user addresses.
     * Non-canonical RIP causes SYSRETQ to #GP in ring-0.  Use r8 as scratch
     * so rax (return value) and r11/rcx (user state) remain intact. */
    movq %rcx, %r8
    shrq $47, %r8
    jnz .Lsyscall_noncanonical

    movq %r9, %rsp
    swapgs                  /* restore user GS: GS_BASE=0, KERNEL_GS_BASE=&cpu_local */
    sysretq                 /* rax=return value, rcx=user RIP, r11=user RFLAGS */

.Lsyscall_noncanonical:
    /* iretq fallback: SS=0x1B(user data), CS=0x23(user code) */
    pushq $0x1B
    pushq %r9
    pushq %r11
    pushq $0x23
    pushq %rcx
    swapgs                  /* restore user GS before iretq */
    iretq

.section .note.GNU-stack,"",@progbits
