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
.global context_switch

context_switch:
    /* Save outgoing FPU/SSE state — must be first, before touching rsp */
    fxsaveq (%r8)

    /* Save outgoing RSP */
    movq %rsp, (%rdx)

    /* Save outgoing callee-saved integer registers */
    movq %r15, 0(%rdi)
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbx, 32(%rdi)
    movq %rbp, 40(%rdi)

    /* Save RIP: the return address sitting on top of the outgoing stack */
    movq (%rsp), %rax
    movq %rax, 48(%rdi)

    /* Save RFLAGS (preserves per-task IF state) */
    pushfq
    popq 56(%rdi)

    /* Restore incoming callee-saved integer registers */
    movq 0(%rsi),  %r15
    movq 8(%rsi),  %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbx
    movq 40(%rsi), %rbp

    /* Switch to the incoming task's kernel stack BEFORE restoring RFLAGS.
     * Any interrupt that fires after popfq will run on the correct stack. */
    movq %rcx, %rsp

    /* Restore incoming RFLAGS — uses the new stack for the transient push */
    pushq 56(%rsi)
    popfq

    /* Restore incoming FPU/SSE state — r9 still holds new_fpu */
    fxrstorq (%r9)

    /* Write incoming RIP to the top of the new stack and return into it */
    movq 48(%rsi), %rax
    movq %rax, (%rsp)
    ret

.section .note.GNU-stack,"",@progbits
