/*
 * svcmgr.S — Service manager process, ring-3.
 *
 * Entry contract (set by svcmgr_bootstrap.c):
 *   RBX = bootstrap KChannel handle (RIGHT_READ | RIGHT_WRITE)
 *
 * Phase 1 behaviour:
 *   1. Self-registers as "svcmgr" in the nameserver via SYS_NS_REGISTER.
 *      The kernel does NOT register svcmgr — this demonstrates the
 *      self-registration model that services should follow.
 *   2. Enters a receive loop on the bootstrap channel.
 *   3. Messages are received but not yet dispatched (stub).
 *
 * Phase 2 migration:
 *   Replace the stub loop body with dispatch on KChanMsg.type:
 *     SVCMGR_MSG_SPAWN_SERVICE → SYS_SPAWN + SYS_NS_REGISTER + ACK
 *   Remove kbd_bootstrap_init() spawn/register calls;
 *   keep irq_routing_register (permanent kernel-side concern).
 *
 * See iris/svcmgr_proto.h for the full protocol definition.
 *
 * KChanMsg stack layout (76 bytes):
 *   [rsp+ 0] type      (uint32_t, 4 bytes)
 *   [rsp+ 4] sender_id (uint32_t, 4 bytes)
 *   [rsp+ 8] data[64]  (uint8_t[])
 *   [rsp+72] data_len  (uint32_t, 4 bytes)
 *
 * Stack frame (128 bytes, 16-byte aligned):
 *   [rsp+ 0 .. rsp+75]  KChanMsg receive buffer
 *   [rsp+80 .. rsp+111] scratch (proc_h, NS result, reply_chan_h)
 *   [rsp+112.. rsp+115] svc_slot[0].proc_h  (uint32_t, 0 = empty slot)
 *   [rsp+116.. rsp+127] svc_slot[0] reserved (future: name, flags)
 *
 * svc_slot[0] is the lifecycle tracking slot for the first spawned service.
 * svcmgr stores proc_h here instead of closing it, enabling SYS_PROCESS_STATUS
 * queries at the top of each recv-loop iteration.  When the process is observed
 * dead, svcmgr closes the handle and clears the slot.
 * Transitional note: only 1 slot exists; extend to N when more services are
 * managed.  The single slot is sufficient for the current kbd-only phase.
 */

.global svcmgr_main

.section .rodata
sm_str_started:      .asciz "[SVCMGR] started\n"
sm_str_ready:        .asciz "[SVCMGR] ready\n"
sm_str_regfail:      .asciz "[SVCMGR] WARN: self-register failed\n"
sm_str_recverr:      .asciz "[SVCMGR] recv error, retrying\n"
sm_str_spawnok:      .asciz "[SVCMGR] service spawned\n"
sm_str_spawnfail:    .asciz "[SVCMGR] WARN: spawn failed\n"
sm_str_nsfail:       .asciz "[SVCMGR] WARN: ns register failed\n"
sm_str_replyregfail: .asciz "[SVCMGR] WARN: reply chan ns register failed\n"
sm_str_svc_exited:   .asciz "[SVCMGR] service exited\n"
sm_str_slot_full:    .asciz "[SVCMGR] WARN: service slot full, proc_h not tracked\n"
sm_name_self:     .asciz "svcmgr"

.section .text
svcmgr_main:
    /* RBX = bootstrap channel handle (set by svcmgr_bootstrap.c) */

    /* Allocate stack frame (128 bytes, 16-byte aligned) */
    subq  $128, %rsp
    andq  $~0xFULL, %rsp

    /* Zero service slot — stack is fresh but alignment may vary */
    movl  $0, 112(%rsp)               /* svc_slot[0].proc_h = 0 (empty) */
    movq  $0, 116(%rsp)               /* svc_slot[0] reserved */
    movq  $0, 124(%rsp)               /* svc_slot[0] reserved */

    /* ── announce start ─────────────────────────────────────── */
    movq  $0,   %rax                    /* SYS_WRITE */
    leaq  sm_str_started(%rip), %rdi
    syscall

    /* ── self-register in nameserver ────────────────────────── */
    /*
     * SYS_NS_REGISTER(name, handle, rights)
     *   name   = "svcmgr"
     *   handle = bootstrap KChannel handle (RBX)
     *   rights = RIGHT_WRITE (2) — callers can send spawn requests
     *
     * This is the architectural difference from kbd_bootstrap_init():
     * the service manager owns its own nameserver identity.
     */
    movq  $24,  %rax                    /* SYS_NS_REGISTER */
    leaq  sm_name_self(%rip), %rdi
    movq  %rbx, %rsi                    /* bootstrap handle */
    movq  $2,   %rdx                    /* RIGHT_WRITE */
    syscall
    testq %rax, %rax
    jnz   .sm_regfail

    /* ── announce ready ─────────────────────────────────────── */
    movq  $0,   %rax                    /* SYS_WRITE */
    leaq  sm_str_ready(%rip), %rdi
    syscall

    /* ── bootstrap receive loop ─────────────────────────────── */
.sm_loop:
    /* ── lifecycle check ────────────────────────────────────────
     * Poll any tracked service before blocking on the next recv.
     * SYS_PROCESS_STATUS(proc_h) → 1=alive, 0=dead, <0=error.
     * On death: log, close proc_h, clear the slot.
     * On error or empty slot: skip silently.
     */
    movl  112(%rsp), %edi              /* svc_slot[0].proc_h (0 = empty) */
    testl %edi, %edi
    jz    .sm_buf_zero                 /* slot empty — skip check */

    movq  $26,  %rax                   /* SYS_PROCESS_STATUS */
    syscall
    testq %rax, %rax
    js    .sm_buf_zero                 /* error — skip (keep slot) */
    jnz   .sm_buf_zero                 /* alive (1) — skip */

    /* status = 0: service exited */
    movq  $0,   %rax                   /* SYS_WRITE */
    leaq  sm_str_svc_exited(%rip), %rdi
    syscall

    movl  112(%rsp), %edi              /* proc_h */
    movq  $15,  %rax                   /* SYS_HANDLE_CLOSE */
    syscall
    movl  $0, 112(%rsp)               /* clear slot */

.sm_buf_zero:
    /* zero the receive buffer */
    movq  $0,  0(%rsp)
    movq  $0,  8(%rsp)
    movq  $0, 16(%rsp)
    movq  $0, 24(%rsp)
    movq  $0, 32(%rsp)
    movq  $0, 40(%rsp)
    movq  $0, 48(%rsp)
    movq  $0, 56(%rsp)
    movq  $0, 64(%rsp)
    movq  $0, 72(%rsp)

    /* SYS_CHAN_RECV(bootstrap_handle, &msg) */
    movq  $14,  %rax                    /* SYS_CHAN_RECV */
    movq  %rbx, %rdi
    movq  %rsp, %rsi
    movq  $0,   %rdx
    syscall
    testq %rax, %rax
    jnz   .sm_recv_err

    /* Dispatch on KChanMsg.type at [rsp+0] */
    cmpl  $0x0001, 0(%rsp)             /* SVCMGR_MSG_SPAWN_SERVICE */
    je    .sm_handle_spawn
    jmp   .sm_loop

    /*
     * SVCMGR_MSG_SPAWN_SERVICE handler.
     *
     * KChanMsg receive buffer at [rsp+0..rsp+75]:
     *   [rsp+ 0] type       (uint32_t)  = 0x0001
     *   [rsp+ 4] sender_id  (uint32_t)
     *   [rsp+ 8] data[0..7] = entry_vaddr    (uint64_t)
     *   [rsp+16] data[8..23]= service name   (char[16])
     *   [rsp+32] data[24..27]= rights        (uint32_t)
     *   [rsp+36] data[28..31]= reg_chan_h    (handle_id_t)
     *   [rsp+40] data[32..47]= reply_name    (char[16])
     *
     * Scratch at [rsp+80..rsp+111]:
     *   [rsp+80..83] proc_h saved here (uint32_t) after SYS_SPAWN
     *   [rsp+88..95] NS result saved here (int64_t) after SYS_NS_REGISTER
     *   [rsp+96..99] reply_chan_h (uint32_t) after SYS_CHAN_CREATE
     *
     * Reply channel lifecycle:
     *   If reply_name[0] != 0: SYS_CHAN_CREATE → SYS_NS_REGISTER(reply_name)
     *   → SYS_HANDLE_CLOSE(reply_chan_h).  NS holds the KObject reference.
     *   svcmgr does not use the reply channel itself.
     *   This registration happens BEFORE SYS_SPAWN so the NS entry is
     *   visible to any process that runs after svcmgr yields.
     *
     * proc_h lifecycle tracking:
     *   If svc_slot[0] is empty (svc_slot[0].proc_h == 0):
     *     store proc_h in svc_slot[0] for SYS_PROCESS_STATUS polling.
     *   If svc_slot[0] is full:
     *     close proc_h immediately (can't track — log warning).
     *   In both cases proc_h is either tracked or closed; no accumulation.
     */
.sm_handle_spawn:
    /* ── reply channel ─────────────────────────────────────────────────
     * Create, register, and release the reply channel BEFORE SYS_SPAWN.
     * Structural guarantee: "reply_name" is in the NS when svcmgr yields,
     * which is before any later-task (user_init, kbd_server) can look it up.
     */
    cmpb  $0, 40(%rsp)                 /* reply_name[0] == '\0'? */
    je    .sm_do_spawn                 /* no reply channel requested */

    movq  $12,  %rax                   /* SYS_CHAN_CREATE */
    syscall
    testq %rax, %rax
    js    .sm_do_spawn                 /* alloc failed — skip, still spawn */
    movl  %eax, 96(%rsp)              /* save reply_chan_h */

    /* SYS_NS_REGISTER(reply_name, reply_chan_h, RIGHT_READ|RIGHT_WRITE) */
    leaq  40(%rsp), %rdi               /* reply_name at data[32] */
    movl  96(%rsp), %esi               /* reply_chan_h */
    movq  $3,       %rdx               /* RIGHT_READ | RIGHT_WRITE */
    movq  $24,      %rax               /* SYS_NS_REGISTER */
    syscall
    testq %rax, %rax
    jnz   .sm_reply_reg_fail

    /* SYS_HANDLE_CLOSE(reply_chan_h) — NS holds the KObject ref */
    movl  96(%rsp), %edi
    movq  $15,      %rax
    syscall
    jmp   .sm_do_spawn

.sm_reply_reg_fail:
    movq  $0,   %rax                   /* SYS_WRITE */
    leaq  sm_str_replyregfail(%rip), %rdi
    syscall
    /* Close the handle regardless of NS failure */
    movl  96(%rsp), %edi
    movq  $15,      %rax
    syscall
    jmp   .sm_do_spawn

.sm_do_spawn:
    /* SYS_SPAWN(entry_vaddr, NULL) → proc_h */
    movq  8(%rsp),  %rdi               /* arg0 = entry_vaddr */
    movq  $0,       %rsi               /* arg1 = out_chan_ptr = NULL */
    movq  $0,       %rdx
    movq  $18,      %rax               /* SYS_SPAWN */
    syscall
    testq %rax, %rax
    js    .sm_spawn_fail
    movl  %eax, 80(%rsp)              /* save proc_h for close after NS reg */

    /* SYS_NS_REGISTER(name, reg_chan_h, rights) */
    leaq  16(%rsp), %rdi               /* arg0 = name ptr (data[8]) */
    movl  36(%rsp), %esi               /* arg1 = reg_chan_h (zero-extends to rsi) */
    movl  32(%rsp), %edx               /* arg2 = rights */
    movq  $24,      %rax               /* SYS_NS_REGISTER */
    syscall
    movq  %rax, 88(%rsp)              /* save NS result before close clobbers rax */

    /* ── proc_h lifecycle tracking ─────────────────────────────────
     * Store proc_h in svc_slot[0] if empty; otherwise close it.
     * The slot is polled at the top of .sm_loop via SYS_PROCESS_STATUS.
     */
    movl  112(%rsp), %eax              /* svc_slot[0].proc_h */
    testl %eax, %eax
    jnz   .sm_slot_full                /* slot occupied — close proc_h */

    movl  80(%rsp),  %eax
    movl  %eax, 112(%rsp)             /* svc_slot[0].proc_h = proc_h */
    jmp   .sm_check_ns_result

.sm_slot_full:
    movq  $0,   %rax                   /* SYS_WRITE */
    leaq  sm_str_slot_full(%rip), %rdi
    syscall
    movl  80(%rsp), %edi               /* proc_h */
    movq  $15,      %rax               /* SYS_HANDLE_CLOSE */
    syscall

.sm_check_ns_result:
    /* check saved NS result */
    movq  88(%rsp), %rax
    testq %rax, %rax
    jnz   .sm_ns_fail

    movq  $0,   %rax                   /* SYS_WRITE */
    leaq  sm_str_spawnok(%rip), %rdi
    syscall
    jmp   .sm_loop

.sm_spawn_fail:
    movq  $0,   %rax                   /* SYS_WRITE */
    leaq  sm_str_spawnfail(%rip), %rdi
    syscall
    jmp   .sm_loop

.sm_ns_fail:
    movq  $0,   %rax                   /* SYS_WRITE */
    leaq  sm_str_nsfail(%rip), %rdi
    syscall
    jmp   .sm_loop

.sm_recv_err:
    movq  $0,   %rax                    /* SYS_WRITE */
    leaq  sm_str_recverr(%rip), %rdi
    syscall
    jmp   .sm_loop

.sm_regfail:
    movq  $0,   %rax                    /* SYS_WRITE */
    leaq  sm_str_regfail(%rip), %rdi
    syscall
    /* Continue to recv loop even on regfail — svcmgr stays alive */
    jmp   .sm_loop

    /* unreachable */
.sm_dead:
    movq  $1,   %rax                    /* SYS_EXIT */
    movq  $0,   %rdi
    syscall
    hlt
    jmp   .sm_dead

.section .note.GNU-stack,"",@progbits
