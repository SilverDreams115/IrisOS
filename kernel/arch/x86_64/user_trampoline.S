/* SPDX-License-Identifier: Apache-2.0 */
.global user_entry_trampoline

/*
 * ── Why both paths below start by reading %gs, and what they used to do ─────
 *
 * Each of these hands a thread to ring 3, and each has to leave two things
 * behind it: the thread's CR3, and IA32_KERNEL_GS_BASE holding the per-CPU
 * block the next SYSCALL's swapgs will pull back in.
 *
 * They used to take both from a fixed place — `syscall_user_cr3(%rip)`, a
 * global shadow of the per-CPU field, and `leaq cpu_local(%rip)`, which is
 * cpu_local[0] and no other.  On one processor that is right, and it is right
 * for the reason that makes it wrong on four: there was only one of each.
 *
 * With four cores, a thread started on CPU 2 went to ring 3 with
 * KERNEL_GS_BASE pointing at CPU 0's block — so its first syscall's swapgs
 * gave CPU 2 CPU 0's per-CPU data, and `%gs:48` handed it CPU 0's kernel
 * stack.  Two processors then ran kernel code on the same stack, which
 * presents as an instruction fetch from inside `core_stacks` and nothing
 * legible before it.  The CR3 shadow is the same defect one field over:
 * whichever core dispatched most recently decided which address space every
 * other core's thread resumed into.
 *
 * Both are read GS-relative here, BEFORE `movw %ax, %gs` clears GS_BASE —
 * after that instruction this core cannot find its own block at all.  The
 * values ride in r12/r13, which are about to be zeroed anyway and so are the
 * two registers that cost nothing to borrow.
 */
.section .text
user_entry_trampoline:
    movq %gs:0,  %r12           /* this core's cpu_local (offset 0: self)   */
    movq %gs:56, %r13           /* this core's syscall_user_cr3             */

    /* preparar segmentos de datos de usuario antes de iretq */
    movw $0x1B, %ax    /* user data selector (GDT[3]) — matches iretq SS */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs      /* also sets IA32_GS_BASE = 0 (GDT[3].base = 0) */

    /* switch to user CR3 (no-flush PCID) before returning to ring-3 */
    testq %r13, %r13
    jz .Ltramp_no_user_cr3
    movq %r13, %cr3
.Ltramp_no_user_cr3:

    /* Restore ring-3 SWAPGS state unconditionally.
     *
     * Fase 2 ABI: ring-3 must have GS_BASE=0, KGS_BASE=&cpu_local[this core]
     * so the next SYSCALL's SWAPGS yields GS_BASE=&cpu_local for the kernel.
     *
     * The movw above sets GS_BASE=0 (GDT[3] base=0).  KGS_BASE could be 0 if
     * the trampoline was entered from syscall context (where SWAPGS at entry
     * left KGS_BASE=0), so it is written unconditionally. */
    movq %r12, %rax
    movq %r12, %rdx
    shrq $32, %rdx
    movl $0xC0000102, %ecx          /* IA32_KERNEL_GS_BASE */
    wrmsr

    xorq %rax, %rax
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    movq %rbx, %rdi             /* the argument again, where C reads it */
    xorq %r8,  %r8
    xorq %r9,  %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    xorq %rbp, %rbp

    iretq


.section .text
/*
 * start_user_ctx_and_iretq(const struct iris_user_ctx *ctx) — step 3.
 *
 * A thread's FIRST entry into ring 3, from its TCB rather than from a frame
 * pre-built on a kernel stack it owns.
 *
 * Everything below is what user_entry_trampoline does and why: the data
 * selectors have to be set once (iretq reloads CS and SS, never DS/ES/FS/GS),
 * CR3 has to become the thread's, and IA32_KERNEL_GS_BASE has to hold this
 * core's &cpu_local so the FIRST syscall's swapgs finds it — there is no swapgs here
 * to put it there, because this path was never in the kernel on this thread's
 * behalf before.  The registers are zeroed rather than left as whatever the
 * dispatcher had in them: a new thread's first instruction should not be able
 * to read kernel values out of a register nobody meant to hand it.
 *
 * The difference from that trampoline is only where the frame comes from.  It
 * used to be pushed onto the thread's own kernel stack at creation, which is
 * one of the two reasons every thread needed a stack before it had ever run.
 */
.global start_user_ctx_and_iretq
start_user_ctx_and_iretq:
    /* Build the iretq frame from the context BEFORE the selectors change:
     * offsets follow struct iris_user_ctx (rip 136, cs 144, rflags 152,
     * rsp 160, ss 168). */
    pushq 168(%rdi)             /* ss     */
    pushq 160(%rdi)             /* rsp    */
    pushq 152(%rdi)             /* rflags */
    pushq 144(%rdi)             /* cs     */
    pushq 136(%rdi)             /* rip    */

    /* rbx is the thread's argument, and the trampoline this replaces carried
     * it in the same register for the same reason: it survives the zeroing
     * below.  It is ALSO delivered in rdi — the System V first argument — so a
     * thread written in C can read it as a parameter.
     *
     * That is not a convenience.  A thread cannot be handed its own TCB
     * capability without somewhere per-thread to put it, and the entry
     * register is the only such place a freshly started thread has; without it
     * the only way for a thread to name itself is SYS_TCB_SELF, which hands
     * out a capability asking for none (charter A5, ledger A-18).  rdi is
     * loaded LAST, after the zeroing, for the same reason rbx is.
     */
    movq 104(%rdi), %rbx

    /* This core's block and this core's user CR3, while %gs still names them.
     * See the note at the top of this file. */
    movq %gs:0,  %r12
    movq %gs:56, %r13

    movw $0x1B, %ax             /* user data selector (GDT[3]) */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs               /* also sets IA32_GS_BASE = 0 */

    testq %r13, %r13
    jz .Lstart_no_user_cr3
    movq %r13, %cr3
.Lstart_no_user_cr3:

    movq %r12, %rax
    movq %r12, %rdx
    shrq $32, %rdx
    movl $0xC0000102, %ecx      /* IA32_KERNEL_GS_BASE */
    wrmsr

    xorq %rax, %rax
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    movq %rbx, %rdi             /* the argument again, where C reads it */
    xorq %r8,  %r8
    xorq %r9,  %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    xorq %rbp, %rbp

    iretq

.section .note.GNU-stack,"",@progbits
