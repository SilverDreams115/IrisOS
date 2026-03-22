
.extern isr_handler

/*
 * Macro para ISR sin error code — el CPU no pushea error code
 * para estas excepciones, lo pusheamos nosotros como 0 para
 * mantener el frame uniforme.
 */
.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp   isr_common
.endm

/*
 * Macro para ISR con error code — el CPU ya pusheó el error code.
 */
.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp   isr_common
.endm

/* excepciones x86_64 */
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
 * Handler común — salva todos los registros, llama a isr_handler en C,
 * restaura y retorna.
 */

/* IRQ0 — timer (vector 0x20 = 32) */
ISR_NOERR 32
ISR_NOERR 33  /* IRQ1 — PS/2 keyboard */

/* IRQs 2-15 del PIC (vectores 34-47) — necesarios para no triple-faultear
 * ante IRQs espurios o periféricos no manejados (PIC IRQ7/IRQ15 en especial). */
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

isr_common:
    /* salvar registros de propósito general */
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

    /* rdi = puntero al frame completo en el stack */
    movq %rsp, %rdi
    call isr_handler

    /* restaurar registros */
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

    /* sacar vector e error code del stack */
    addq $16, %rsp
    iretq

.section .note.GNU-stack,"",@progbits
