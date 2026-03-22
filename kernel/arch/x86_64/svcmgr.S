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
 * Stack frame (208 bytes, 16-byte aligned):
 *   [rsp+  0 .. rsp+ 75]  KChanMsg receive buffer
 *   [rsp+ 80 .. rsp+111]  scratch (proc_h@80, NS result@88, reply_chan_h@96)
 *   [rsp+112 .. rsp+207]  svc_slot[0..3]  (4 × 24 bytes)
 *
 * svc_slot[i] layout (24 bytes each):
 *   [+0 .. +3]  proc_h   (uint32_t; 0 = empty slot)
 *   [+4]        irq_num  (uint8_t;  0xFF = no IRQ route)
 *   [+5 .. +7]  _pad
 *   [+8 .. +23] name     (char[16]; service name for diagnostics)
 *
 * Slot base offsets from rsp:
 *   slot[0] = rsp+112   slot[1] = rsp+136
 *   slot[2] = rsp+160   slot[3] = rsp+184
 *
 * Lifecycle tracking:
 *   svcmgr iterates all slots at the top of .sm_loop via SYS_PROCESS_STATUS.
 *   On death: logs, closes proc_h, clears slot.
 *   On spawn: finds first free slot; stores proc_h + irq_num + name.
 *   If all slots are full on spawn: closes proc_h immediately (no tracking).
 *   irq_num != 0xFF: calls SYS_IRQ_ROUTE_REGISTER after spawn to transfer
 *     route ownership to the child process (auto-cleared on child exit).
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
sm_str_irqfail:      .asciz "[SVCMGR] WARN: irq route xfer failed\n"
sm_name_self:     .asciz "svcmgr"

.section .text
svcmgr_main:
    /* RBX = bootstrap channel handle (set by svcmgr_bootstrap.c) */

    /* Allocate stack frame (208 bytes, 16-byte aligned) */
    subq  $208, %rsp
    andq  $~0xFULL, %rsp

    /* Zero all service slots (4 × 24 = 96 bytes at rsp+112..rsp+207) */
    movq  $0, 112(%rsp)
    movq  $0, 120(%rsp)
    movq  $0, 128(%rsp)
    movq  $0, 136(%rsp)
    movq  $0, 144(%rsp)
    movq  $0, 152(%rsp)
    movq  $0, 160(%rsp)
    movq  $0, 168(%rsp)
    movq  $0, 176(%rsp)
    movq  $0, 184(%rsp)
    movq  $0, 192(%rsp)
    movq  $0, 200(%rsp)

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
    /* ── lifecycle check (all 4 slots) ──────────────────────────
     * Iterate svc_slot[0..3].  For each non-empty slot:
     *   SYS_PROCESS_STATUS(proc_h) → 1=alive, 0=dead, <0=error
     * On death: log, close proc_h, clear slot fields.
     * On error or empty slot: skip silently.
     * r14 = slot index (0..3), r15 = slot byte offset from rsp.
     * Both registers are preserved across syscall (not rcx/r11).
     */
    movq  $0, %r14                     /* slot index = 0 */
.sm_lifecycle_loop:
    cmpq  $4, %r14
    jge   .sm_buf_zero                 /* all slots checked */

    movq  %r14, %r15
    imulq $24,  %r15
    addq  $112, %r15                   /* r15 = byte offset of slot[r14] */

    movl  (%rsp,%r15), %edi            /* proc_h (0 = empty) */
    testl %edi, %edi
    jz    .sm_lifecycle_next           /* empty slot — skip */

    movq  $26, %rax                    /* SYS_PROCESS_STATUS */
    syscall
    testq %rax, %rax
    js    .sm_lifecycle_next           /* error — keep slot */
    jnz   .sm_lifecycle_next           /* alive — skip */

    /* status = 0: service exited — log, close, clear */
    movq  $0,   %rax                   /* SYS_WRITE */
    leaq  sm_str_svc_exited(%rip), %rdi
    syscall                            /* r14 and r15 preserved */

    movl  (%rsp,%r15), %edi            /* proc_h */
    movq  $15,  %rax                   /* SYS_HANDLE_CLOSE */
    syscall                            /* r14 and r15 preserved */
    movl  $0,  (%rsp,%r15)             /* clear proc_h */
    movb  $0,  4(%rsp,%r15)            /* clear irq_num */
    movq  $0,  8(%rsp,%r15)            /* clear name[0..7] */
    movq  $0, 16(%rsp,%r15)            /* clear name[8..15] */

.sm_lifecycle_next:
    incq  %r14
    jmp   .sm_lifecycle_loop

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
     * Find a free slot (proc_h == 0) among svc_slot[0..3].
     * Store proc_h, irq_num, and service name in the slot.
     * If no free slot: close proc_h immediately and log warning.
     * r14 = slot search index, r15 = slot byte offset.
     */
    movq  $0, %r14
.sm_find_slot:
    cmpq  $4, %r14
    jge   .sm_slot_full
    movq  %r14, %r15
    imulq $24,  %r15
    addq  $112, %r15
    movl  (%rsp,%r15), %eax
    testl %eax, %eax
    jz    .sm_slot_found
    incq  %r14
    jmp   .sm_find_slot

.sm_slot_found:
    movl  80(%rsp),  %eax
    movl  %eax,      (%rsp,%r15)       /* slot.proc_h = proc_h */
    movzbl 56(%rsp), %eax              /* irq_num = data[SVCMGR_SPAWN_OFF_IRQ] */
    movb  %al,       4(%rsp,%r15)      /* slot.irq_num */
    movq  16(%rsp),  %rax              /* name[0..7] from data[SVCMGR_SPAWN_OFF_NAME] */
    movq  %rax,      8(%rsp,%r15)      /* slot.name[0..7] */
    movq  24(%rsp),  %rax              /* name[8..15] */
    movq  %rax,     16(%rsp,%r15)      /* slot.name[8..15] */

    /* ── IRQ ownership transfer ─────────────────────────────────────
     * If irq_num != 0xFF: call SYS_IRQ_ROUTE_REGISTER(irq, chan_h, proc_h)
     * to move the IRQ route owner from svcmgr to the child process.
     * kprocess_teardown will auto-clear the route when the child exits.
     * On failure: log warning and continue (route stays owned by svcmgr).
     */
    movzbl 4(%rsp,%r15), %eax          /* irq_num from slot */
    cmpb  $0xFF, %al
    je    .sm_check_ns_result          /* no IRQ route for this service */

    movl  %eax,     %edi               /* arg0 = irq_num */
    movl  36(%rsp), %esi               /* arg1 = reg_chan_h (data[SVCMGR_SPAWN_OFF_REG_CHAN]) */
    movl  80(%rsp), %edx               /* arg2 = proc_h */
    movq  $27,      %rax               /* SYS_IRQ_ROUTE_REGISTER */
    syscall
    testq %rax, %rax
    js    .sm_irq_xfer_fail
    jmp   .sm_check_ns_result

.sm_irq_xfer_fail:
    movq  $0,   %rax
    leaq  sm_str_irqfail(%rip), %rdi
    syscall
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
