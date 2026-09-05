.global user_entry_trampoline
.extern syscall_user_cr3
.extern cpu_local

.section .text
user_entry_trampoline:
    /* preparar segmentos de datos de usuario antes de iretq */
    movw $0x1B, %ax    /* user data selector (GDT[3]) — matches iretq SS */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs      /* also sets IA32_GS_BASE = 0 (GDT[3].base = 0) */

    /* switch to user CR3 (no-flush PCID) before returning to ring-3; rax is
     * used as a scratch register and is zeroed by the xorq below */
    movq syscall_user_cr3(%rip), %rax
    testq %rax, %rax
    jz .Ltramp_no_user_cr3
    movq %rax, %cr3
.Ltramp_no_user_cr3:

    /* Restore ring-3 SWAPGS state unconditionally.
     *
     * Fase 2 ABI: ring-3 must have GS_BASE=0, KGS_BASE=&cpu_local so the next
     * SYSCALL's SWAPGS yields GS_BASE=&cpu_local for the kernel.
     *
     * The movw above sets GS_BASE=0 (GDT[3] base=0).  KGS_BASE could be 0 if
     * the trampoline was entered from syscall context (where SWAPGS at entry
     * left KGS_BASE=0).  Unconditionally restore KGS_BASE=&cpu_local[0] so the
     * invariant holds regardless of which context triggered the first task run. */
    leaq cpu_local(%rip), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl $0xC0000102, %ecx          /* IA32_KERNEL_GS_BASE */
    wrmsr

    xorq %rax, %rax
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
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
 * CR3 has to become the thread's, and IA32_KERNEL_GS_BASE has to hold
 * &cpu_local so the FIRST syscall's swapgs finds it — there is no swapgs here
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

    /* rbx is the thread's argument, and the ONE register that survives the
     * zeroing below — the trampoline this replaces carried it in the same
     * register for the same reason. */
    movq 104(%rdi), %rbx

    movw $0x1B, %ax             /* user data selector (GDT[3]) */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs               /* also sets IA32_GS_BASE = 0 */

    movq syscall_user_cr3(%rip), %rax
    testq %rax, %rax
    jz .Lstart_no_user_cr3
    movq %rax, %cr3
.Lstart_no_user_cr3:

    leaq cpu_local(%rip), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl $0xC0000102, %ecx      /* IA32_KERNEL_GS_BASE */
    wrmsr

    xorq %rax, %rax
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
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
