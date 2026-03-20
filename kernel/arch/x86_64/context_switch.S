/*
 * context_switch(struct cpu_context *old,
 *                struct cpu_context *new,
 *                uint64_t *old_rsp,
 *                uint64_t new_rsp)
 *
 * rdi = old
 * rsi = new
 * rdx = &old_rsp
 * rcx = new_rsp
 *
 * Guarda registros callee-saved + RSP/RIP de la tarea actual
 * y restaura RSP/RIP + callee-saved de la tarea siguiente.
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

    /* guardar RIP actual: está como return address en el stack */
    movq (%rsp), %rax
    movq %rax, 48(%rdi)

    /* restaurar registros callee-saved de la tarea que entra */
    movq 0(%rsi),  %r15
    movq 8(%rsi),  %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbx
    movq 40(%rsi), %rbp

    /* restaurar RSP de la nueva tarea */
    movq %rcx, %rsp

    /* escribir su RIP en la cima del stack y regresar */
    movq 48(%rsi), %rax
    movq %rax, (%rsp)
    ret

.section .note.GNU-stack,"",@progbits
