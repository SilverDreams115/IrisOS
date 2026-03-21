
.global gdt_flush
.global idt_flush

/* ── gdt_flush ──────────────────────────────────────────────────────────
 * Carga la nueva GDT y recarga los registros de segmento.
 * rdi = dirección del gdtr
 */
gdt_flush:
    lgdt (%rdi)

    /* recargar CS con un far return */
    pushq $0x08             /* kernel code selector */
    leaq  .reload_cs(%rip), %rax
    pushq %rax
    lretq

.reload_cs:
    /* recargar registros de datos con kernel data selector */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    ret

/* ── idt_flush ──────────────────────────────────────────────────────────
 * Carga la IDT.
 * rdi = dirección del idtr
 */
idt_flush:
    lidt (%rdi)
    ret

/* ── tss_flush ──────────────────────────────────────────────────────────
 * Carga el TSS en el registro TR.
 * rdi = selector TSS (e.g. 0x28)
 */
.global tss_flush
tss_flush:
    movw %di, %ax
    ltr  %ax
    ret
.section .note.GNU-stack,"",@progbits
