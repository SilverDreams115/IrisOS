/*
 * context_switch(struct cpu_context *old,  // rdi
 *                struct cpu_context *new,  // rsi
 *                uint64_t *old_rsp,        // rdx
 *                uint64_t  new_rsp,        // rcx
 *                uint8_t  *old_fpu,        // r8  — 512-byte FXSAVE buffer, 16-byte aligned
 *                uint8_t  *new_fpu)        // r9  — 512-byte FXSAVE buffer, 16-byte aligned
 *
 * Saves the FPU/SSE state (FXSAVE), callee-saved integer registers,
 * kernel RSP, RIP, and RFLAGS of the outgoing task, then restores
 * the same set for the incoming task (FXRSTOR).
 *
 * RSP is switched to the new task's kernel stack BEFORE restoring RFLAGS.
 * This ensures that if an interrupt fires after RFLAGS is restored (with
 * IF possibly re-enabled), the CPU is already on the correct kernel stack
 * for the incoming task.
 *
 * r8 (old_fpu) and r9 (new_fpu) are caller-saved registers not included in
 * cpu_context, so they retain their parameter values through the entire
 * function body.
 *
 * cpu_context layout:
 *   [0]  r15
 *   [8]  r14
 *   [16] r13
 *   [24] r12
 *   [32] rbx
 *   [40] rbp
 *   [48] rip
 *   [56] rflags
 */
/*
 * context_switch is DELETED (Stage 9-evt step 3).
 *
 * It saved one thread's callee-saved registers and kernel stack pointer and
 * restored another's — the only way the CPU changed hands, and the reason
 * every thread needed a kernel stack to come back to.  Nothing calls it: a
 * thread is resumed from its TCB (an iretq for ring 3, a call for a kernel
 * resume), and the stack it runs on belongs to the core.
 *
 * What survives is the FPU half, which a switch used to do inline and which a
 * TCB resume still needs.
 */

.section .text
/*
 * fpu_save_to / fpu_restore_from — Stage 9-evt step 3.
 *
 * context_switch has always done these as part of a switch, because a switch
 * was the only way the CPU changed hands.  A thread resumed from its TCB
 * (`restore_user_ctx_and_iretq`) never goes through one, so the two halves
 * have to be reachable on their own.
 *
 * Same instructions, same 512-byte 16-byte-aligned area.
 */
.global fpu_save_to
fpu_save_to:
    testq %rdi, %rdi
    jz .Lfpu_save_none
    fxsaveq (%rdi)
.Lfpu_save_none:
    ret

.global fpu_restore_from
fpu_restore_from:
    testq %rdi, %rdi
    jz .Lfpu_restore_none
    fxrstorq (%rdi)
.Lfpu_restore_none:
    ret

.section .note.GNU-stack,"",@progbits
