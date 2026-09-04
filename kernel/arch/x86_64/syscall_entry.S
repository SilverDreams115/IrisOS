.global syscall_entry
.extern syscall_dispatch
.extern syscall_save_user_ctx

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

    /* save the syscall frame:
     *   [rsp+112] user rbp   ┐
     *   [rsp+104] user rbx   │ callee-saved (see below)
     *   [rsp+96]  user r12   │
     *   [rsp+88]  user r13   │
     *   [rsp+80]  user r14   │
     *   [rsp+72]  user r15   ┘
     *   [rsp+64] user rsp    (saved from r8)
     *   [rsp+56] user rflags (r11, set by syscall insn)
     *   [rsp+48] user rip    (rcx, set by syscall insn)
     *   [rsp+40] arg3        (r10, 4th user arg)
     *   [rsp+32] arg2        (rdx)
     *   [rsp+24] arg1        (rsi)
     *   [rsp+16] arg0        (rdi)
     *   [rsp+8]  syscall num (rax)
     *   [rsp+0]  alignment pad
     *
     * The callee-saved registers are pushed FIRST, above everything else, so
     * that every offset the rest of this file uses stayed where it was.
     *
     * A procedural kernel does not need them here: it obeys the C ABI, so the
     * user's rbx/rbp/r12-r15 either stay in registers or get spilled onto this
     * very frame, and the normal exit path finds them restored.  Step 2 of the
     * event kernel is precisely the decision to ABANDON this frame, which
     * throws those spills away — so an abandoned thread would resume in ring 3
     * with kernel values in six of its registers.  seL4 saves the full user
     * context on every entry for the same reason; this is that cost, six
     * pushes, paid on the syscall path.
     */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %r8         /* user rsp    */
    pushq %r11        /* user rflags */
    pushq %rcx        /* user rip    */
    pushq %r10        /* arg3        */
    pushq %rdx        /* arg2        */
    pushq %rsi        /* arg1        */
    pushq %rdi        /* arg0        */
    pushq %rax        /* syscall no  */
    subq $8, %rsp     /* alignment   */

    /*
     * Stage 9-evt Step 2: copy the user context into the TCB.
     *
     * The frame above is what step 2 abandons when a handler parks, so the
     * return address, flags and stack pointer must exist somewhere else by
     * then.  Done in C, from a pointer to this frame, so that neither the
     * frame layout nor `struct task`'s field offsets are hard-coded here —
     * a field added in C would otherwise silently corrupt a register save.
     *
     * The argument registers are reloaded from the frame immediately below,
     * so clobbering them here costs nothing.
     */
    movq %rsp, %rdi
    call syscall_save_user_ctx

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


.section .text
/*
 * syscall_return_to_user(rax_value, user_rip, user_rflags, user_rsp,
 *                        callee_saved)
 *
 * Stage 9-evt Step 2 — return to ring 3 WITHOUT a syscall frame.
 *
 * The normal exit path above reads the user's rip, flags and stack out of the
 * frame syscall_entry pushed.  A syscall that parked and was re-entered on a
 * fresh stack has no such frame, so it passes those three values in registers
 * and this builds an iretq frame from them.
 *
 * Arguments rather than GS-relative reads on purpose: struct task's field
 * offsets are not hard-coded anywhere in assembly, so a field added in C
 * cannot silently corrupt a register restore.
 *
 * IRETQ rather than SYSRETQ because SYSRETQ takes RIP from RCX and RFLAGS from
 * R11 — a calling convention this path does not have — and because IRETQ
 * accepts a non-canonical RIP without faulting in ring 0, which the exit path
 * above needs a separate check for.
 *
 *   rdi = value to return in rax
 *   rsi = user RIP
 *   rdx = user RFLAGS
 *   rcx = user RSP
 *   r8  = &task->sc_user_regs[0]  — r15, r14, r13, r12, rbx, rbp
 */
.global syscall_return_to_user
syscall_return_to_user:
    /* switch to the user address space, as the normal exit does.  r9 is the
     * scratch here because r8 carries the callee-saved block. */
    movq %gs:56, %r9        /* cpu_local.syscall_user_cr3 */
    testq %r9, %r9
    jz .Lrtu_no_cr3
    movq %r9, %cr3
.Lrtu_no_cr3:

    /* iretq frame: SS, RSP, RFLAGS, CS, RIP.  Built before the register
     * restore below, because the pushes read rcx, rdx and rsi. */
    pushq $0x1B             /* user data selector */
    pushq %rcx              /* user RSP    */
    pushq %rdx              /* user RFLAGS */
    pushq $0x23             /* user code selector */
    pushq %rsi              /* user RIP    */

    movq %rdi, %rax         /* syscall return value */

    /* the user's callee-saved registers, saved at entry by syscall_entry and
     * carried in the TCB across the park */
    movq  0(%r8), %r15
    movq  8(%r8), %r14
    movq 16(%r8), %r13
    movq 24(%r8), %r12
    movq 32(%r8), %rbx
    movq 40(%r8), %rbp

    swapgs                  /* ring-3 GS: GS_BASE=0, KGS_BASE=&cpu_local */
    iretq

.section .note.GNU-stack,"",@progbits
