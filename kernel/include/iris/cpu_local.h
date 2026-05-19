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
 * On BSP (boot processor) boot:
 *   gdt_init() writes &cpu_local[0] to IA32_GS_BASE (MSR 0xC0000101) and to
 *   IA32_KERNEL_GS_BASE (MSR 0xC0000102), and sets cpu_local[0].self.
 *
 * SMP note: each AP must set its own IA32_GS_BASE to &cpu_local[ap_id]
 * and initialise the self-pointer before enabling interrupts.
 */

#define MAX_CPUS 8

struct task;

struct iris_cpu_local {
    struct iris_cpu_local *self;           /* offset 0  — GS:0 self-pointer */
    struct task           *current_task;   /* offset 8  — task on this CPU  */
    uint32_t               cpu_id;         /* offset 16 — logical CPU index */
    uint32_t               lapic_id;       /* offset 20 — LAPIC hardware ID */
    uint64_t               context_switches; /* offset 24 — total context switches on this CPU */
    uint64_t               idle_ticks;     /* offset 32 — scheduler ticks spent in idle */
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
