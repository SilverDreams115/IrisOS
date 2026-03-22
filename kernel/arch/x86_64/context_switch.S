/*
 * context_switch(struct cpu_context *old,
 *                struct cpu_context *new,
 *                uint64_t *old_rsp,
 *                uint64_t new_rsp)
 *
 * rdi = old_ctx
 * rsi = new_ctx
 * rdx = &old_rsp
 * rcx = new_rsp
 *
 * Guarda/restaura callee-saved + RSP + RIP + RFLAGS.
 * Guardar RFLAGS evita que un yield desde un IRQ handler (IF=0)
 * contamine el estado de la tarea que se reanuda (debería tener IF=1).
 *
 * Layout de cpu_context:
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
    /* guardar RSP actual de la tarea que sale */
    movq %rsp, (%rdx)

    /* guardar registros callee-saved */
    movq %r15, 0(%rdi)
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbx, 32(%rdi)
    movq %rbp, 40(%rdi)

    /* guardar RIP: return address en la cima del stack */
    movq (%rsp), %rax
    movq %rax, 48(%rdi)

    /* guardar RFLAGS (especialmente IF) */
    pushfq
    popq 56(%rdi)

    /* restaurar callee-saved de la tarea entrante */
    movq 0(%rsi),  %r15
    movq 8(%rsi),  %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbx
    movq 40(%rsi), %rbp

    /* restaurar RFLAGS — usa stack viejo como scratch (limpia después) */
    pushq 56(%rsi)
    popfq

    /* restaurar RSP de la nueva tarea */
    movq %rcx, %rsp

    /* escribir RIP en la cima del stack nuevo y retornar */
    movq 48(%rsi), %rax
    movq %rax, (%rsp)
    ret

.section .note.GNU-stack,"",@progbits
