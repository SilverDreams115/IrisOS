
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

    /* Fase 20: los #GP/#PF (vectores 13/14) entran por IST1, un stack fijo
     * por CPU que se reutiliza desde el tope en cada entrada.  Una task que
     * el handler C suspenda en TASK_BLOCKED_FAULT dejaría su contexto vivo
     * (frame + frames de C + context_switch) dentro de IST1, y el siguiente
     * #GP/#PF de cualquier otra task lo pisaría.  Para faults originados en
     * ring 3 el kernel stack propio de la task está garantizado vacío, así
     * que migramos el frame completo (22 qwords) allí antes de llamar al
     * handler C; el resto del manejo (bloqueo, resume, restore, iretq) corre
     * ya sobre memoria por-task.  %gs:48 = cpu_local.syscall_kstack (tope del
     * kstack de la task actual, mantenido por task_yield; offset fijado por
     * contrato en cpu_local.h).  Los vectores sin IST ya entran por RSP0 y no
     * lo necesitan; los faults de ring 0 se quedan en IST1 (path fatal, no
     * retorna).  rax/rcx/rdx/rsi/rdi se clobberean aquí pero sus valores ya
     * están salvados en el frame y se restauran desde la copia al salir. */
    movq 120(%rsp), %rax        /* frame->vector */
    cmpq $13, %rax
    je   .Lisr_migrate_off_ist
    cmpq $14, %rax
    jne  .Lisr_no_migrate
.Lisr_migrate_off_ist:
    testb $3, 144(%rsp)         /* frame->cs: solo origen ring 3 */
    jz   .Lisr_no_migrate
    movq %gs:48, %rdi           /* cpu_local.syscall_kstack */
    subq $176, %rdi             /* sizeof(struct full_frame) = 22 qwords */
    movq %rdi, %rdx
    movq %rsp, %rsi
    movl $22, %ecx
    cld
    rep movsq
    movq %rdx, %rsp             /* continuar sobre el kstack de la task */
.Lisr_no_migrate:

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

    /*
     * Mirror the entry check: if returning to ring-3, SWAPGS to restore user GS.
     * Stack layout here (after all pops) is identical to entry layout.
     */
    testb $3, 24(%rsp)
    jz .Lisr_exit_ring0
    swapgs
.Lisr_exit_ring0:

    /* sacar vector e error code del stack */
    addq $16, %rsp
    iretq

.section .note.GNU-stack,"",@progbits
