/* SPDX-License-Identifier: Apache-2.0 */
.global syscall_entry
.extern syscall_dispatch
.extern syscall_save_user_ctx

/*
 * The shadow globals are DELETED (SMP roadmap §9.3 step 4).
 *
 * `syscall_kstack_ptr` and `syscall_user_cr3` were RIP-relative copies of two
 * per-CPU fields, written on every dispatch and described as kept "for
 * debugging" — the live read path here has been GS-relative for some time.
 * They were not harmless.  A per-CPU value with a global copy beside it is an
 * invitation to read the copy, and something did: both ring-3 entry paths in
 * user_trampoline.S took the user CR3 from the global, so on four processors
 * the last core to dispatch chose which address space every other core's
 * thread resumed into.  The fix was to read %gs there; deleting the globals is
 * what stops it coming back.
 *
 * The 4 KiB `syscall_kstack_base` went with them.  It was the kernel's single
 * syscall stack before the stack belonged to a core (Stage 9-evt step 3), and
 * since then nothing but the shadow's initialiser has named it.
 */

.section .text
/*
 * SWAPGS ABI (Fase 2):
 *   Ring-0 resting: GS_BASE = &cpu_local[cpu_id], KGS_BASE = 0.
 *   Ring-3 user:    GS_BASE = 0,                  KGS_BASE = &cpu_local[cpu_id].
 *
 *   On SYSCALL from ring-3: GS_BASE=0, KGS_BASE=&cpu_local.
 *   Entry SWAPGS → GS_BASE=&cpu_local, KGS_BASE=0 — kernel can use %gs.
 *   Exit  SWAPGS → GS_BASE=0, KGS_BASE=&cpu_local — user sees null GS.
 *
 *   Offsets used:
 *     %gs:48  = cpu_local.syscall_kstack   (kernel stack top for current task)
 *     %gs:56  = cpu_local.syscall_user_cr3 (user CR3 for current task)
 *     %gs:72  = cpu_local.syscall_user_rsp (scratch: see below)
 */
syscall_entry:
    /* SWAPGS first: GS_BASE ↔ KERNEL_GS_BASE → %gs = &cpu_local[0] */
    swapgs

    /*
     * Park the user stack pointer in the per-CPU scratch and take the kernel
     * stack.  This used to be `movq %rsp, %r8`; r8 is the FIFTH ARGUMENT
     * REGISTER now (ledger A-32), and there is no other register free here —
     * rcx and r11 carry the user's RIP and RFLAGS, rax the syscall number, and
     * rdi/rsi/rdx/r10/r8 are the five arguments.  Pushed straight out of
     * memory below, so it never occupies a register at all.
     */
    movq %rsp, %gs:72           /* cpu_local.syscall_user_rsp */
    movq %gs:48, %rsp           /* cpu_local.syscall_kstack */

    /* save the syscall frame:
     *   [rsp+128] user rbp   ┐ callee-saved (see below)
     *   [rsp+120] user rbx   │
     *   [rsp+112] user r12   ┘
     *   [rsp+104] arg8       (r13, 9th user arg — A-33)
     *   [rsp+96]  arg7       (r14, 8th user arg — A-33)
     *   [rsp+88]  arg6       (r15, 7th user arg — A-33)
     *   [rsp+80]  arg5       (r9,  6th user arg — A-33)
     *   [rsp+72]  arg4       (r8,  5th user arg — A-32)
     *   [rsp+64] user rsp    (from %gs:72)
     *   [rsp+56] user rflags (r11, set by syscall insn)
     *   [rsp+48] user rip    (rcx, set by syscall insn)
     *   [rsp+40] arg3        (r10, 4th user arg)
     *   [rsp+32] arg2        (rdx)
     *   [rsp+24] arg1        (rsi)
     *   [rsp+16] arg0        (rdi)
     *   [rsp+8]  syscall num (rax)
     *   [rsp+0]  alignment pad
     *
     * The callee-saved registers are pushed FIRST, above everything else, so
     * that every offset the rest of this file uses stayed where it was.
     *
     * args 4 through 7 are pushed with the callee-saved block rather than next
     * to their four siblings, so that every offset below them — and every
     * instruction in this file that names one — stayed where it was as the
     * argument count grew.
     *
     * r15 and r14 cost nothing to carry: they were ALREADY being pushed here
     * as callee-saved, so the message ABI (A-33) taking them as arguments is
     * the same two stores with a different name on them.  seL4 uses r15 as a
     * message register on x86_64 for exactly this reason.
     *
     * A procedural kernel does not need them here: it obeys the C ABI, so the
     * user's rbx/rbp/r12-r15 either stay in registers or get spilled onto this
     * very frame, and the normal exit path finds them restored.  Step 2 of the
     * event kernel is precisely the decision to ABANDON this frame, which
     * throws those spills away — so an abandoned thread would resume in ring 3
     * with kernel values in six of its registers.  seL4 saves the full user
     * context on every entry for the same reason; this is that cost, six
     * pushes, paid on the syscall path.
     */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13        /* arg8 (A-33) — and the user's r13 */
    pushq %r14        /* arg7 (A-33) — and the user's r14 */
    pushq %r15        /* arg6 (A-33) — and the user's r15 */
    pushq %r9         /* arg5 (A-33) */
    pushq %r8         /* arg4 (A-32) */
    pushq %gs:72      /* user rsp    */
    pushq %r11        /* user rflags */
    pushq %rcx        /* user rip    */
    pushq %r10        /* arg3        */
    pushq %rdx        /* arg2        */
    pushq %rsi        /* arg1        */
    pushq %rdi        /* arg0        */
    pushq %rax        /* syscall no  */
    subq $8, %rsp     /* alignment   */

    /*
     * Stage 9-evt Step 2: copy the user context into the TCB.
     *
     * The frame above is what step 2 abandons when a handler parks, so the
     * return address, flags and stack pointer must exist somewhere else by
     * then.  Done in C, from a pointer to this frame, so that neither the
     * frame layout nor `struct task`'s field offsets are hard-coded here —
     * a field added in C would otherwise silently corrupt a register save.
     *
     * The argument registers are reloaded from the frame immediately below,
     * so clobbering them here costs nothing.
     */
    movq %rsp, %rdi
    call syscall_save_user_ctx

    /*
     * syscall_dispatch(num, arg0..arg8) — ten parameters, nine of them user
     * words (A-33).
     *
     * Six go in registers per the C ABI and FOUR on the stack: arg5 through
     * arg8, in that order from [rsp+0].  32 bytes, which is what four words
     * takes and what the call's 16-byte alignment wants anyway.
     */
    subq $32, %rsp
    movq 112(%rsp), %rax
    movq %rax,  0(%rsp)   /* arg5 */
    movq 120(%rsp), %rax
    movq %rax,  8(%rsp)   /* arg6 */
    movq 128(%rsp), %rax
    movq %rax, 16(%rsp)   /* arg7 */
    movq 136(%rsp), %rax
    movq %rax, 24(%rsp)   /* arg8 */
    movq  40(%rsp), %rdi  /* num  */
    movq  48(%rsp), %rsi  /* arg0 */
    movq  56(%rsp), %rdx  /* arg1 */
    movq  64(%rsp), %rcx  /* arg2 */
    movq  72(%rsp), %r8   /* arg3 */
    movq 104(%rsp), %r9   /* arg4 */
    call syscall_dispatch
    addq $32, %rsp
    /* return value in rax */

    /*
     * A-33: hand the message back.
     *
     * A receive returns a whole message and a message does not fit in rax, so
     * the kernel writes its return words into THIS FRAME and the restore below
     * puts them back in the registers they arrived in.  Done in C, from a
     * pointer to the frame, for the same reason the save at entry is: `struct
     * task`'s field offsets are not hard-coded in assembly.
     *
     * rax is the status and has to survive the call, so it waits in the
     * frame's syscall-number slot, which nothing reads after this point.
     */
    movq %rax, 8(%rsp)
    movq %rsp, %rdi
    call syscall_store_user_ret
    movq 8(%rsp), %rax

    /*
     * A-33: the exit reads everything it needs out of the frame BEFORE it
     * moves rsp, because the frame is on the kernel stack and the last act is
     * to leave it.  The order below is forced by that: scratch first, the
     * return message last, the user stack pointer last of all.
     *
     * rax is the status and is not touched from here to sysretq.
     */
    movq 48(%rsp), %rcx    /* user rip    → rcx (sysretq restores RIP from RCX) */
    movq 56(%rsp), %r11    /* user rflags → r11 (sysretq restores RFLAGS from R11) */

    /* switch to the user address space.  r10 is the scratch and is reloaded
     * with a message register below; the kernel is mapped in every address
     * space, so this frame survives the CR3 write. */
    movq %gs:56, %r10       /* cpu_local.syscall_user_cr3 */
    testq %r10, %r10
    jz .Lsyscall_no_user_cr3
    movq %r10, %cr3
.Lsyscall_no_user_cr3:

    /* validate RCX canonical: bits[63:47] must all be 0 for user addresses.
     * Non-canonical RIP causes SYSRETQ to #GP in ring-0.  Same scratch. */
    movq %rcx, %r10
    shrq $47, %r10
    jnz .Lsyscall_noncanonical

    /*
     * The return message (A-33).  For a call that returns only a status these
     * still hold what the caller passed in — syscall_store_user_ret left the
     * frame alone — which is strictly kinder than the clobber ring 3 already
     * assumes.  For a receive they hold the message: msginfo, badge, four
     * words, and the capability that was delivered.
     */
    movq 16(%rsp), %rdi
    movq 24(%rsp), %rsi
    movq 32(%rsp), %rdx
    movq 40(%rsp), %r10
    movq 72(%rsp), %r8
    movq 80(%rsp), %r9
    movq 88(%rsp), %r15

    movq 64(%rsp), %rsp     /* the user's stack: the last thing read here */
    swapgs                  /* restore user GS: GS_BASE=0, KERNEL_GS_BASE=&cpu_local */
    sysretq                 /* rax=return value, rcx=user RIP, r11=user RFLAGS */

.Lsyscall_noncanonical:
    /* iretq fallback: SS=0x1B(user data), CS=0x23(user code).  The frame is
     * built FIRST, out of registers already loaded, and the message is read
     * afterwards at offsets 40 bytes higher — five pushes. */
    movq 64(%rsp), %r10     /* user rsp */
    pushq $0x1B
    pushq %r10
    pushq %r11
    pushq $0x23
    pushq %rcx
    movq  56(%rsp), %rdi
    movq  64(%rsp), %rsi
    movq  72(%rsp), %rdx
    movq  80(%rsp), %r10
    movq 112(%rsp), %r8
    movq 120(%rsp), %r9
    movq 128(%rsp), %r15
    swapgs                  /* restore user GS before iretq */
    iretq


.section .text
/*
 * syscall_return_to_user(rax_value, user_rip, user_rflags, user_rsp,
 *                        callee_saved)
 *
 * Stage 9-evt Step 2 — return to ring 3 WITHOUT a syscall frame.
 *
 * The normal exit path above reads the user's rip, flags and stack out of the
 * frame syscall_entry pushed.  A syscall that parked and was re-entered on a
 * fresh stack has no such frame, so it passes those three values in registers
 * and this builds an iretq frame from them.
 *
 * Arguments rather than GS-relative reads on purpose: struct task's field
 * offsets are not hard-coded anywhere in assembly, so a field added in C
 * cannot silently corrupt a register restore.
 *
 * IRETQ rather than SYSRETQ because SYSRETQ takes RIP from RCX and RFLAGS from
 * R11 — a calling convention this path does not have — and because IRETQ
 * accepts a non-canonical RIP without faulting in ring 0, which the exit path
 * above needs a separate check for.
 *
 *   rdi = value to return in rax
 *   rsi = user RIP
 *   rdx = user RFLAGS
 *   rcx = user RSP
 *   r8  = &task->sc_user_regs[0]  — r15, r14, r13, r12, rbx, rbp
 *   r9  = &task->sc_ret[0]        — the return message (A-33): ret0..ret6
 *
 * A-33: a parked receive completes here, and a receive returns a MESSAGE.  The
 * fast path above reads it out of the frame; this path has no frame, so the
 * kernel left it in the TCB and hands over a pointer the same way it already
 * does for the callee-saved block.
 */
.global syscall_return_to_user
syscall_return_to_user:
    /* Switch to the user address space, as the normal exit does.
     *
     * r11 is the scratch.  It used to be r9 — which A-33 made an ARGUMENT, the
     * pointer to the return message, and the first thing this function then
     * did was overwrite it with a CR3 value and dereference that.  r8 carries
     * the callee-saved block and r9 the message, so r11 is what is left: it is
     * caller-saved and nothing on this path has put anything in it. */
    movq %gs:56, %r11       /* cpu_local.syscall_user_cr3 */
    testq %r11, %r11
    jz .Lrtu_no_cr3
    movq %r11, %cr3
.Lrtu_no_cr3:

    /* iretq frame: SS, RSP, RFLAGS, CS, RIP.  Built before the register
     * restore below, because the pushes read rcx, rdx and rsi. */
    pushq $0x1B             /* user data selector */
    pushq %rcx              /* user RSP    */
    pushq %rdx              /* user RFLAGS */
    pushq $0x23             /* user code selector */
    pushq %rsi              /* user RIP    */

    movq %rdi, %rax         /* syscall return value */

    /* the user's callee-saved registers, saved at entry by syscall_entry and
     * carried in the TCB across the park */
    movq  8(%r8), %r14
    movq 16(%r8), %r13
    movq 24(%r8), %r12
    movq 32(%r8), %rbx
    movq 40(%r8), %rbp

    /* the return message (A-33).  r15 comes from HERE rather than from the
     * callee-saved block above, because it is a message register: what the
     * thread gets back is what the receive delivered, not what it was holding
     * when it parked.  r8 is read last of all, being the pointer. */
    movq  0(%r9), %rdi
    movq  8(%r9), %rsi
    movq 16(%r9), %rdx
    movq 24(%r9), %r10
    movq 48(%r9), %r15
    movq 40(%r9), %r11      /* ret5 parked: r9 is still the pointer */
    movq 32(%r9), %r8
    movq %r11, %r9

    swapgs                  /* ring-3 GS: GS_BASE=0, KGS_BASE=&cpu_local */
    iretq

.section .note.GNU-stack,"",@progbits
