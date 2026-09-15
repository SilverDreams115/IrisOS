/*
 * ap_trampoline.S — from 16-bit real mode to 64-bit long mode, for an
 * application processor (SMP roadmap §9.3 step 3).
 *
 * A STARTUP IPI begins execution at `vector << 12` in REAL MODE: 16-bit, no
 * paging, segment:offset addressing, and a processor that knows nothing about
 * the kernel that woke it.  Everything the BSP established over several
 * hundred lines of C has to be re-entered here in assembly, because there is
 * no C to call into until there is a stack and a 64-bit code segment.
 *
 * POSITION INDEPENDENCE IS THE WHOLE DIFFICULTY.  This code is COPIED to a
 * page below 1 MiB and runs from there, so no link-time address in it is
 * valid.  The first instructions therefore compute the page base from CS —
 * which in real mode after a STARTUP IPI is exactly `vector << 8` — and keep
 * it in EBX for the rest of the sequence.  Every data reference is
 * `base + offset`; the only things patched by the BSP are the two far-jump
 * targets, because a far jump takes its destination as an immediate and there
 * is nowhere to put a register.
 *
 * The sequence, and every step is load-bearing:
 *   1. real      → GDT with 32-bit code/data, CR0.PE          → protected
 *   2. protected → CR4.PAE, CR3, EFER.LME                     (long possible)
 *   3. CR0.PG                                                 (long active)
 *   4. far jump into a 64-bit code segment                    → 64-bit
 *   5. take the stack this AP was given and call into C, never returning
 *
 * PROGRESS BYTES.  Each stage writes one to a fixed offset in this page, which
 * the BSP reads when a processor fails to arrive.  A processor that does not
 * start is otherwise completely silent — no output, no fault, nothing to
 * attach to — and the difference between "the far jump was wrong" and "the
 * page tables were wrong" is an afternoon against a week.
 *
 * AP_TRAMPOLINE_TRACE additionally writes each stage straight to COM1.  It is
 * off by default because it prints on every boot, and it is kept because the
 * memory bytes are only readable if the BSP SURVIVES — and the failures here
 * were of the kind where a faulting AP takes the machine down with it, so the
 * bytes were never read.  Every bug in this file was found with it on:
 *
 *   "12"     the far jump to 32-bit worked, paging did not        (NX low page)
 *   "12345"  long mode reached, the jump into C faulted           (EFER.NXE)
 *
 * Turn it on by defining AP_TRAMPOLINE_TRACE and rebuilding.
 */
#ifdef AP_TRAMPOLINE_TRACE
#define TRACE(ch) movw $0x3F8, %dx ; movb $ch, %al ; outb %al, %dx
#else
#define TRACE(ch)
#endif

.code16
.section .ap_trampoline, "ax"
.global ap_trampoline_start
.global ap_trampoline_end
/* The parameter slots are global so the C side can compute their offsets by
 * subtraction rather than carrying a copy of the numbers.  A copy would be a
 * second place to change, and the failure mode of getting it wrong is a
 * processor that silently never starts. */
.global tramp_gdtr
.global tramp_cr3
.global tramp_stack
.global tramp_entry
.global tramp_progress
.global tramp_pm_ptr
.global tramp_lm_ptr
.global tramp_pm
.global tramp_lm

#define OFF(sym)  (sym - ap_trampoline_start)

ap_trampoline_start:
    cli
    cld

    /* Page base = CS << 4.  In real mode a STARTUP IPI leaves CS = vector<<8
     * and IP = 0, so this is where we are, computed rather than assumed. */
    movw    %cs, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    xorl    %ebx, %ebx
    movw    %ax, %bx
    shll    $4, %ebx                    /* EBX = linear base of this page */

    movb    $1, OFF(tramp_progress)     /* 1: real mode reached */
    TRACE(0x31)
    movw    $0x3F8, %dx                 /* COM1, straight out: an AP that dies
                                         * before C has no other way to speak */
    movb    $0x31, %al                  /* '1' */
    outb    %al, %dx

    lgdtl   OFF(tramp_gdtr)

    movl    %cr0, %eax
    orl     $1, %eax                    /* CR0.PE */
    movl    %eax, %cr0

    /* Far jump to reload CS with the 32-bit code selector.  The offset is an
     * immediate and is patched by the BSP to `base + OFF(tramp_pm)`. */
    .byte   0x66, 0xea                  /* ljmpl ptr16:32 */
tramp_pm_ptr:
    .long   0                           /* offset   — patched */
    .word   0x08                        /* selector — 32-bit code */

.code32
tramp_pm:
    movw    $0x10, %ax                  /* 32-bit data selector */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    movb    $2, OFF(tramp_progress)(%ebx)   /* 2: protected mode */
    TRACE(0x32)

    /* PAE before LME has any effect, and CR3 before paging is enabled.  The
     * order is architectural, not stylistic. */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax             /* CR4.PAE */
    movl    %eax, %cr4

    movl    OFF(tramp_cr3)(%ebx), %eax  /* the BSP's PML4 */
    movl    %eax, %cr3

    movl    $0xC0000080, %ecx           /* IA32_EFER */
    rdmsr
    orl     $(1 << 8), %eax             /* EFER.LME — long mode enable */
    /*
     * EFER.NXE, and it is not optional.
     *
     * The BSP set it, so the kernel's page tables have bit 63 set on every
     * non-executable mapping — .rodata, .data, .bss, the physmap window.  With
     * NXE CLEAR that bit is RESERVED, and a walk through any such entry is a
     * reserved-bit page fault.  An AP without it reaches long mode, executes
     * from the trampoline page happily, and dies the first time it touches its
     * own stack — which is the function prologue of whatever it calls, so the
     * symptom is "the jump into C faults" and the cause is three steps back.
     */
    orl     $(1 << 11), %eax            /* EFER.NXE */
    wrmsr

    movl    %cr0, %eax
    orl     $(1 << 31), %eax            /* CR0.PG */
    movl    %eax, %cr0

    movb    $3, OFF(tramp_progress)(%ebx)   /* 3: paging on, long mode armed */
    TRACE(0x33)

    /* The far jump into a 64-bit code segment is what switches the
     * processor's width; LME and PG only made it possible. */
    .byte   0xea                        /* ljmp ptr16:32 */
tramp_lm_ptr:
    .long   0                           /* offset   — patched */
    .word   0x18                        /* selector — 64-bit code */

.code64
tramp_lm:
    movb    $4, OFF(tramp_progress)(%rbx)   /* 4: 64-bit long mode */
    TRACE(0x34)

    /* The stack and the entry point were written by the BSP for THIS
     * processor, which is why bring-up is serialised: the page holds one
     * processor's parameters at a time. */
    movq    OFF(tramp_stack)(%rbx), %rsp
    xorq    %rbp, %rbp

    movb    $5, OFF(tramp_progress)(%rbx)   /* 5: about to enter C */
    TRACE(0x35)

    /* The entry point is loaded AFTER the marker, and that ordering is a scar:
     * `outb` takes its byte in AL, which is the low byte of RAX — writing the
     * marker first clobbered the low byte of the address and called into
     * nowhere.  A debugging aid that breaks the thing it is watching is worth
     * a comment so the next person does not reintroduce it. */
    movq    OFF(tramp_entry)(%rbx), %rax
    callq   *%rax

    /* `ap_main` does not return.  If it did, halting is the only honest thing
     * left: this processor has no scheduler state and nowhere to go. */
1:  cli
    hlt
    jmp     1b

/*
 * Parameters at fixed offsets.  `iris/smp.h` names them and the two must
 * agree — a _Static_assert there cannot see these, so the offsets are also
 * asserted at runtime by the BSP before the first IPI.
 */
.align 16
tramp_gdtr:
    .word   0                           /* limit — patched */
    .long   0                           /* base  — patched */
    .long   0
.align 8
tramp_cr3:
    .quad   0                           /* PML4 physical */
tramp_stack:
    .quad   0                           /* this AP's kernel stack top */
tramp_entry:
    .quad   0                           /* ap_main */
tramp_progress:
    .quad   0                           /* how far the last AP got */

ap_trampoline_end:
.section .note.GNU-stack,"",@progbits
