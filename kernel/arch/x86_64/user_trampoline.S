.global user_entry_trampoline
.extern syscall_user_cr3
.extern cpu_local

.section .text
user_entry_trampoline:
    /* diag: direct serial write 'T' to confirm trampoline reached */
    movw $0x3F8+5, %dx
1:  inb %dx, %al
    testb $0x20, %al
    jz 1b
    movw $0x3F8, %dx
    movb $'T', %al
    outb %al, %dx
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

    /* diag: 'U' = IRETQ about to fire — remove after smoke test diagnosis */
    movw $0x3F8+5, %dx
2:  inb %dx, %al
    testb $0x20, %al
    jz 2b
    movw $0x3F8, %dx
    movb $'U', %al
    outb %al, %dx

    iretq

.section .note.GNU-stack,"",@progbits
