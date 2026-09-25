/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fpu_switch.S — saving and restoring the FPU/SSE state across a resume.
 *
 * This file was context_switch.S, and the name outlived its subject by four
 * stages.  What is here is the FPU half and nothing else; the header block
 * below records what the other half was and why it went.
 */
/*
 * context_switch is DELETED (Stage 9-evt step 3).
 *
 * It saved one thread's callee-saved registers and kernel stack pointer and
 * restored another's — the only way the CPU changed hands, and the reason
 * every thread needed a kernel stack to come back to.  Nothing calls it: a
 * thread is resumed from its TCB (an iretq for ring 3, a call for a kernel
 * resume), and the stack it runs on belongs to the core.
 *
 * What survives is the FPU half, which a switch used to do inline and which a
 * TCB resume still needs.
 */

.section .text
/*
 * fpu_save_to / fpu_restore_from — Stage 9-evt step 3.
 *
 * context_switch has always done these as part of a switch, because a switch
 * was the only way the CPU changed hands.  A thread resumed from its TCB
 * (`restore_user_ctx_and_iretq`) never goes through one, so the two halves
 * have to be reachable on their own.
 *
 * Same instructions, same 512-byte 16-byte-aligned area.
 */
.global fpu_save_to
fpu_save_to:
    testq %rdi, %rdi
    jz .Lfpu_save_none
    fxsaveq (%rdi)
.Lfpu_save_none:
    ret

.global fpu_restore_from
fpu_restore_from:
    testq %rdi, %rdi
    jz .Lfpu_restore_none
    fxrstorq (%rdi)
.Lfpu_restore_none:
    ret

.section .note.GNU-stack,"",@progbits
