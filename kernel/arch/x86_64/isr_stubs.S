
.extern isr_handler

/*
 * Macro for an ISR with no error code — the CPU does not push an error code
 * for these exceptions, so we push it ourselves as 0 to keep the frame
 * uniform.
 */
.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp   isr_common
.endm

/*
 * Macro for an ISR with an error code — the CPU already pushed the error code.
 */
.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp   isr_common
.endm

/* x86_64 exceptions */
ISR_NOERR 0   /* divide by zero            */
ISR_NOERR 1   /* debug                     */
ISR_NOERR 2   /* NMI                       */
ISR_NOERR 3   /* breakpoint                */
ISR_NOERR 4   /* overflow                  */
ISR_NOERR 5   /* bound range exceeded      */
ISR_NOERR 6   /* invalid opcode            */
ISR_NOERR 7   /* device not available      */
ISR_ERR   8   /* double fault              */
ISR_NOERR 9   /* coprocessor segment overrun */
ISR_ERR   10  /* invalid TSS               */
ISR_ERR   11  /* segment not present       */
ISR_ERR   12  /* stack segment fault       */
ISR_ERR   13  /* general protection fault  */
ISR_ERR   14  /* page fault                */
ISR_NOERR 15  /* reserved                  */
ISR_NOERR 16  /* x87 floating point        */
ISR_ERR   17  /* alignment check           */
ISR_NOERR 18  /* machine check             */
ISR_NOERR 19  /* SIMD floating point       */
ISR_NOERR 20  /* virtualization            */
ISR_NOERR 21  /* reserved                  */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30  /* security exception        */
ISR_NOERR 31  /* reserved                  */

/*
 * Common handler — saves every register, calls isr_handler in C, restores
 * and returns.
 */

/* IRQ0 — timer (vector 0x20 = 32) */
ISR_NOERR 32
ISR_NOERR 33  /* IRQ1 — PS/2 keyboard */

/* PIC IRQs 2-15 (vectors 34-47) — needed to avoid triple-faulting on spurious
 * IRQs or unhandled peripherals (PIC IRQ7/IRQ15 especially). */
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39  /* IRQ7  — spurious PIC1 */
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47  /* IRQ15 — spurious PIC2 */

ISR_NOERR 240 /* RESCHEDULE_IPI_VECTOR (0xF0) — cross-CPU reschedule */

isr_common:
    /*
     * SWAPGS ABI: at ISR entry the stack (after stub pushes) is:
     *   RSP+0  = vector, RSP+8 = error, RSP+16 = RIP, RSP+24 = CS, ...
     * CS bits[1:0] == 3 → came from ring-3 → must SWAPGS to get kernel GS.
     * CS bits[1:0] == 0 → came from ring-0 → GS already kernel, no SWAPGS.
     *
     * NMI PARANOID_ENTRY debt (Fase 2.1):
     *
     *   Fase 2 ABI: ring-0 always has GS_BASE = &cpu_local.  The CS-check is
     *   therefore CORRECT for the overwhelmingly common cases:
     *     · ring-3 user code → CS bits=3 → SWAPGS → GS_BASE=&cpu_local ✓
     *     · ring-0 normal (idle loop, ring-0 ISRs) → CS bits=0, GS already ✓
     *
     *   Two tiny residual windows remain where CS bits=0 but GS_BASE=0:
     *
     *   Window 1 — between the SYSCALL instruction and the first `swapgs` in
     *   syscall_entry.S (~2 instructions: swapgs + movq %rsp,%r8).
     *   The SYSCALL instruction sets CS to ring-0 but does NOT swap GS.
     *   User's GS_BASE was 0 → still 0 here.  An NMI here sees CS=ring-0 and
     *   skips SWAPGS → handler runs with GS_BASE=0.
     *
     *   Window 2 — between the exit `swapgs` and SYSRETQ/IRETQ in
     *   syscall_entry.S (~3 instructions).  Exit SWAPGS sets GS_BASE=0 before
     *   SYSRETQ; CS is still ring-0; NMI sees CS=ring-0 and skips SWAPGS.
     *
     *   Identical issue exists for the window between ISR exit SWAPGS and IRETQ
     *   when returning to ring-3 from an interrupt.
     *
     *   Safe for current IRIS because:
     *     a) NMI always panics (always_fatal in isr_handler); the panic path
     *        uses direct IO port writes — no GS-relative access at all.
     *     b) QEMU does not generate NMIs in normal operation.
     *     c) Windows are 2-3 instructions: statistically negligible on hardware.
     *
     *   Fix required before NMI can use cpu_self() or any GS-relative access:
     *     Implement PARANOID_ENTRY for NMI and #MC: replace the CS-check with
     *     RDGSBASE, check if GS_BASE >= KERNEL_BASE (canonical kernel address),
     *     and SWAPGS only if it is not.  This handles all windows correctly
     *     regardless of CS.  Alternatively, use a per-CPU scratch MSR or a
     *     fixed per-CPU mapped byte as "in-SWAPGS-window" flag.
     *     RDGSBASE requires FSGSBASE (CPUID.07H:EBX[0]=1); verify on target.
     *     Until implemented, NMI must not call cpu_self() or klog_write().
     */
    testb $3, 24(%rsp)
    jz .Lisr_from_ring0
    swapgs
.Lisr_from_ring0:

    /* save the general-purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Fase 20: #GP/#PF (vectors 13/14) enter through IST1, a fixed per-CPU
     * stack reused from the top on every entry.  A task that the C handler
     * suspends in TASK_BLOCKED_FAULT would leave its context live (frame + C
     * frames + context_switch) inside IST1, and the next #GP/#PF from any
     * other task would clobber it.  For faults originating in ring 3 the
     * task's own kernel stack is guaranteed empty, so we migrate the whole
     * frame (22 qwords) there before calling the C handler; the rest of the
     * handling (block, resume, restore, iretq) then runs on per-task memory.
     * %gs:48 = cpu_local.syscall_kstack (top of the current task's kstack,
     * maintained by task_yield; offset fixed by contract in cpu_local.h).
     * Vectors without an IST already enter on RSP0 and do not need this; ring-0
     * faults stay on IST1 (fatal path, does not return).  rax/rcx/rdx/rsi/rdi
     * are clobbered here but their values are already saved in the frame and
     * restored from the copy on exit. */
    movq 120(%rsp), %rax        /* frame->vector */
    cmpq $13, %rax
    je   .Lisr_migrate_off_ist
    cmpq $14, %rax
    jne  .Lisr_no_migrate
.Lisr_migrate_off_ist:
    testb $3, 144(%rsp)         /* frame->cs: ring-3 origin only */
    jz   .Lisr_no_migrate
    movq %gs:48, %rdi           /* cpu_local.syscall_kstack */
    subq $176, %rdi             /* sizeof(struct full_frame) = 22 qwords */
    movq %rdi, %rdx
    movq %rsp, %rsi
    movl $22, %ecx
    cld
    rep movsq
    movq %rdx, %rsp             /* continue on the task's kstack */
.Lisr_no_migrate:

    /* rdi = pointer to the full frame on the stack */
    movq %rsp, %rdi
    call isr_handler

    /* restore the registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /*
     * Mirror the entry check: if returning to ring-3, SWAPGS to restore user GS.
     * Stack layout here (after all pops) is identical to entry layout.
     */
    testb $3, 24(%rsp)
    jz .Lisr_exit_ring0
    swapgs
.Lisr_exit_ring0:

    /* pop the vector and error code off the stack */
    addq $16, %rsp
    iretq

.section .note.GNU-stack,"",@progbits
