#ifndef IRIS_CPU_LOCAL_H
#define IRIS_CPU_LOCAL_H

#include <stdint.h>

/*
 * Per-CPU data block — accessed via the GS segment base MSR.
 *
 * Layout rules (MUST NOT change without updating all GS-relative assembly):
 *   offset 0  : self-pointer (iris_cpu_local *)
 *               A single `movq %gs:0, reg` retrieves this pointer from any
 *               ring-0 context, without needing the RDGSBASE instruction.
 *   offset 8  : current_task pointer — the task currently executing on this CPU.
 *
 * SWAPGS ABI (Fase 2):
 *
 *   Ring-0 (kernel):   GS_BASE = &cpu_local[cpu_id],  KGS_BASE = 0
 *   Ring-3 (user):     GS_BASE = 0 (null),            KGS_BASE = &cpu_local[cpu_id]
 *
 *   gdt_init() writes &cpu_local[0] to IA32_GS_BASE (0xC0000101) and 0 to
 *   IA32_KERNEL_GS_BASE (0xC0000102).  This gives ring-0 code immediate access
 *   to %gs without a prior SWAPGS — cpu_self() is safe from the idle loop, from
 *   ring-0 ISRs, and from any non-syscall kernel context.
 *
 *   On syscall/ISR entry from ring-3: SWAPGS exchanges 0 ↔ &cpu_local →
 *   GS_BASE = &cpu_local (kernel uses %gs), KGS_BASE = 0.
 *   On syscall/ISR exit to ring-3:   SWAPGS exchanges &cpu_local ↔ 0 →
 *   GS_BASE = 0 (user sees null GS), KGS_BASE = &cpu_local.
 *   On ISR from ring-0: GS_BASE = &cpu_local already — NO SWAPGS needed.
 *
 *   user_entry_trampoline writes KGS_BASE = &cpu_local[0] before IRETQ so the
 *   next syscall's SWAPGS finds the correct kernel pointer in KGS_BASE.
 *
 * SMP note: each AP must write &cpu_local[ap_id] to IA32_GS_BASE (not
 * IA32_KERNEL_GS_BASE) and write 0 to IA32_KERNEL_GS_BASE, then initialise
 * self before enabling interrupts.
 */

#define MAX_CPUS 8

struct task;
struct CpuRunQueue; /* defined in scheduler_priv.h; pointer kept here to avoid bloating GS area */

struct iris_cpu_local {
    struct iris_cpu_local *self;           /* offset 0  — GS:0 self-pointer */
    struct task           *current_task;   /* offset 8  — task on this CPU  */
    uint32_t               cpu_id;         /* offset 16 — logical CPU index */
    uint32_t               lapic_id;       /* offset 20 — LAPIC hardware ID */
    uint64_t               context_switches; /* offset 24 — total context switches on this CPU */
    uint64_t               idle_ticks;     /* offset 32 — scheduler ticks spent in idle */
    struct CpuRunQueue    *rq;             /* offset 40 — this CPU's run queue */
    /*
     * syscall_kstack / syscall_user_cr3 — kept in sync by syscall_set_kstack()
     * and syscall_set_user_cr3() in syscall_dispatch.c.  syscall_entry.S reads
     * them GS-relative after SWAPGS at syscall entry (Fase 1 complete):
     *   movq %gs:48, %rsp   → kernel stack top
     *   movq %gs:56, %r8    → user CR3
     * The RIP-relative globals syscall_kstack_ptr / syscall_user_cr3 in
     * syscall_entry.S .data are shadow copies kept for debug; the live read
     * path is GS-relative.
     *
     * DO NOT change the offset of these fields without updating syscall_entry.S.
     * syscall_kstack: offset 48; syscall_user_cr3: offset 56.
     */
    uint64_t               syscall_kstack;   /* offset 48 — current task's kstack top */
    uint64_t               syscall_user_cr3; /* offset 56 — current task's user CR3   */
};

extern struct iris_cpu_local cpu_local[MAX_CPUS];

/*
 * cpu_self() — returns a pointer to the current CPU's iris_cpu_local block.
 *
 * Uses `movq %gs:0` to read the self-pointer set by gdt_init().  Safe to call
 * from any ring-0 context after gdt_init() has run.
 */
static inline struct iris_cpu_local *cpu_self(void) {
    struct iris_cpu_local *cl;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(cl) :: );
    return cl;
}

#endif
