/*
 * main.S — keyboard service, ring 3 (standalone ELF binary).
 *
 * Entry contract (ELF process bootstrap):
 *   RBX = private bootstrap channel handle from svcmgr
 *
 * Startup sequence:
 *   1. receive public service/IRQ handle ("kbd") over bootstrap IPC
 *   2. receive reply channel handle ("kbd.reply") over bootstrap IPC
 *   3. close bootstrap channel
 *   4. enter recv loop on the bootstrapped service handle
 *
 * Service discovery and handle delivery is owned entirely by svcmgr.
 * kbd does not call any nameserver or kernel registry directly.
 *
 * Protocol: iris/kbd_proto.h
 *   Requests arrive on the service/IRQ channel (r13).
 *   Replies are sent over the reply channel (r12).
 *   Hardware IRQ scancodes also arrive on r13 (type KBD_MSG_IRQ_SCANCODE).
 *
 * Bootstrap framing: iris/svcmgr_proto.h  (SVCMGR_MSG_BOOTSTRAP_HANDLE)
 */
#include <iris/syscall.h>
#include <iris/svcmgr_proto.h>
#include <iris/kbd_proto.h>
#include <iris/kbd_ep_proto.h>

.global _start

/* ── Endpoint state (Fase 7.4) ─────────────────────────────────────────────
 * kbd_ep_msg:  struct IrisMsg (64 B) staging area for EP recv/reply.
 *              offsets: label 0, words[0..3] 8/16/24/32, word_count 40,
 *              buf_len 44, buf_uptr 48, attached_handle 56, rights 60.
 * kbd_ep_buf:  bulk landing zone (IRIS_IPC_BUF_SIZE); requests carrying bulk
 *              are rejected, the buffer only keeps the kernel copy in-bounds.
 * kbd_ring:    scancode ring, KBD_EP_RING_CAP deep, drop-oldest on overflow.
 * kbd_park:    parked KReply cap for a blocking KBD_EP_OP_READ (0 = none).
 * kbd_ep_h:    recv side of the service KEndpoint (0 = not granted).
 */
.section .bss
.align 8
kbd_ep_msg:     .skip 64
kbd_ep_buf:     .skip 256
kbd_ring:       .skip KBD_EP_RING_CAP
kbd_ring_head:  .skip 4
kbd_ring_count: .skip 4
kbd_park:       .skip 4
kbd_ep_h:       .skip 4

.section .text._start
_start:
    movq  $0, %r12                     /* reply channel handle (from svcmgr) */
    movq  $0, %r13                     /* service/IRQ channel handle          */
    movq  $0, %r14                     /* KIoPort handle (from svcmgr)        */
    movq  $0, %r15                     /* subscriber channel handle (0 = none) */

    subq  $128, %rsp
    andq  $~0xFULL, %rsp

    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'s',  4(%rdi)
    movb  $'t',  5(%rdi)
    movb  $'a',  6(%rdi)
    movb  $'r',  7(%rdi)
    movb  $'t',  8(%rdi)
    movb  $'\n', 9(%rdi)
    movb  $0,   10(%rdi)
    movq  $0,    %rax
    syscall

/* ── Bootstrap: receive service/reply/ioport/irqcap handles ─────────────── */
.kbd_bootstrap_recv:
    testq %r12, %r12
    jz    .kbd_bootstrap_need
    testq %r13, %r13
    jz    .kbd_bootstrap_need
    testq %r14, %r14
    jz    .kbd_bootstrap_need
    cmpl  $0, 84(%rsp)
    jnz   .kbd_bootstrap_done
.kbd_bootstrap_need:
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
    movq  $0, 80(%rsp)

    movq  $14,  %rax                   /* SYS_CHAN_RECV */
    movq  %rbx, %rdi
    movq  %rsp, %rsi
    movq  $0,   %rdx
    syscall
    testq %rax, %rax
    jnz   .kbd_bootstrap_fail

    /* Validate: must be a SVCMGR_MSG_BOOTSTRAP_HANDLE message */
    cmpl  $SVCMGR_MSG_BOOTSTRAP_HANDLE, 0(%rsp)
    jne   .kbd_bootstrap_fail
    /* Validate: must carry a non-null handle */
    movl  76(%rsp), %eax               /* msg.attached_handle */
    testl %eax, %eax
    jz    .kbd_bootstrap_fail

    /* Dispatch on endpoint kind (data[0] = SVCMGR_BOOTSTRAP_OFF_KIND) */
    cmpl  $SVCMGR_BOOTSTRAP_KIND_SERVICE,    8(%rsp)
    je    .kbd_bootstrap_service
    cmpl  $SVCMGR_BOOTSTRAP_KIND_REPLY,      8(%rsp)
    je    .kbd_bootstrap_reply
    cmpl  $SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP, 8(%rsp)
    je    .kbd_bootstrap_ioport
    cmpl  $SVCMGR_BOOTSTRAP_KIND_IRQ_CAP,    8(%rsp)
    je    .kbd_bootstrap_irqcap
    cmpl  $KBD_EP_KIND_SERVICE_EP,           8(%rsp)
    je    .kbd_bootstrap_ep
    jmp   .kbd_bootstrap_fail

.kbd_bootstrap_service:
    movq  %rax, %r13                   /* r13 = service/IRQ channel handle */
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_reply:
    movq  %rax, %r12                   /* r12 = reply channel handle        */
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_ioport:
    movq  %rax, %r14                   /* r14 = KIoPort handle              */
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_irqcap:
    movl  %eax, 84(%rsp)               /* 84(%rsp) = IRQ cap handle (SYS_IRQ_ACK) */
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_ep:
    movl  %eax, kbd_ep_h(%rip)         /* recv side of the kbd KEndpoint (7.4) */
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_done:
    movq  $15,  %rax                   /* SYS_HANDLE_CLOSE: release bootstrap */
    movq  %rbx, %rdi
    syscall
    movl  84(%rsp), %ebx               /* rbx = irqcap handle for SYS_IRQ_ACK */

/* ── PS/2 flush: drain controller output buffer before entering main loop ── */
/* SYS_IOPORT_IN(r14, 4) reads port 0x64 (status); bit 0 = OBF (data ready).
 * SYS_IOPORT_IN(r14, 0) reads port 0x60 (data) to discard the stale byte.
 * Bounded: at most 16 iterations so stuck hardware cannot spin indefinitely.
 * r11d is used as the countdown counter (caller-saved; no live value here). */
.kbd_ps2_flush:
    testq %r14, %r14
    jz    .kbd_ps2_flush_done
    movl  $16,  %r11d                  /* max drain iterations */
.kbd_ps2_flush_loop:
    testl %r11d, %r11d
    jz    .kbd_ps2_flush_done          /* iteration limit reached */
    decl  %r11d
    movq  $SYS_IOPORT_IN, %rax
    movq  %r14, %rdi
    movq  $4,   %rsi                   /* offset 4 = port 0x64 (status) */
    syscall
    testq %rax, %rax
    js    .kbd_ps2_flush_done          /* syscall error → stop */
    testb $1,   %al                    /* OBF set? */
    jz    .kbd_ps2_flush_done          /* no pending data → done */
    movq  $SYS_IOPORT_IN, %rax
    movq  %r14, %rdi
    movq  $0,   %rsi                   /* offset 0 = port 0x60 (data) */
    syscall
    jmp   .kbd_ps2_flush_loop
.kbd_ps2_flush_done:

    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'r',  4(%rdi)
    movb  $'e',  5(%rdi)
    movb  $'a',  6(%rdi)
    movb  $'d',  7(%rdi)
    movb  $'y',  8(%rdi)
    movb  $'\n', 9(%rdi)
    movb  $0,   10(%rdi)
    movq  $0,    %rax
    syscall

    /* "KBD ep ready" — endpoint recv side granted (Fase 7.4 smoke marker). */
    movl  kbd_ep_h(%rip), %eax
    testl %eax, %eax
    jz    .kbd_recv
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'e',  4(%rdi)
    movb  $'p',  5(%rdi)
    movb  $' ',  6(%rdi)
    movb  $'r',  7(%rdi)
    movb  $'e',  8(%rdi)
    movb  $'a',  9(%rdi)
    movb  $'d', 10(%rdi)
    movb  $'y', 11(%rdi)
    movb  $'\n',12(%rdi)
    movb  $0,   13(%rdi)
    movq  $0,    %rax
    syscall

/* ── Main service loop (Fase 7.4) ─────────────────────────────────────────
 * Multiplexes two sources without ever blocking event delivery:
 *   1. drain all pending endpoint requests (SYS_EP_NB_RECV);
 *   2. wait on the legacy service/IRQ KChannel with a 10 ms timeout so the
 *      endpoint drain keeps running even with no channel traffic.
 */
.kbd_recv:
.kbd_ep_drain:
    movl  kbd_ep_h(%rip), %edi
    testl %edi, %edi
    jz    .kbd_chan_wait               /* no endpoint granted: legacy only */
    leaq  kbd_ep_msg(%rip), %rsi       /* zero IrisMsg, stage bulk buffer  */
    movq  $0,  0(%rsi)
    movq  $0,  8(%rsi)
    movq  $0, 16(%rsi)
    movq  $0, 24(%rsi)
    movq  $0, 32(%rsi)
    movq  $0, 40(%rsi)
    movq  $0, 56(%rsi)
    leaq  kbd_ep_buf(%rip), %rax
    movq  %rax, 48(%rsi)               /* buf_uptr = kbd_ep_buf            */
    movq  $SYS_EP_NB_RECV, %rax
    syscall
    testq %rax, %rax
    jnz   .kbd_chan_wait               /* WOULD_BLOCK/err: nothing pending */

    leaq  kbd_ep_msg(%rip), %rsi
    movl  56(%rsi), %eax               /* per-call KReply capability       */
    movl  %eax, 92(%rsp)               /* stash reply cap                  */
    cmpl  $0, 44(%rsi)                 /* bulk payload attached?           */
    jne   .kbd_ep_inval
    movq  0(%rsi), %rax                /* dispatch on label                */
    cmpq  $KBD_EP_PING_OP, %rax
    je    .kbd_ep_ping
    cmpq  $KBD_EP_OP_POLL, %rax
    je    .kbd_ep_poll
    cmpq  $KBD_EP_OP_READ, %rax
    je    .kbd_ep_read
    movl  $KBD_EP_E_NOT_SUPPORTED, %edx
    jmp   .kbd_ep_reply_err
.kbd_ep_inval:
    movl  $KBD_EP_E_INVALID_ARG, %edx
    jmp   .kbd_ep_reply_err

.kbd_ep_ping:                          /* PING → OK, no event payload      */
    leaq  kbd_ep_msg(%rip), %rsi
    movq  $0,  0(%rsi)                 /* label = IRIS_EP_REPLY_OK         */
    movq  $0,  8(%rsi)                 /* words[0] = 0                     */
    movq  $0, 16(%rsi)
    movq  $0, 24(%rsi)
    movq  $0, 32(%rsi)
    movq  $0, 40(%rsi)
    movq  $0, 48(%rsi)
    movq  $0, 56(%rsi)
    movl  $1, 40(%rsi)                 /* word_count = 1                   */
    jmp   .kbd_ep_send_reply

.kbd_ep_poll:                          /* POLL → event or WOULD_BLOCK      */
    movl  kbd_ring_count(%rip), %ecx
    testl %ecx, %ecx
    jnz   .kbd_ep_pop_reply
    movl  $KBD_EP_E_WOULD_BLOCK, %edx
    jmp   .kbd_ep_reply_err

.kbd_ep_read:                          /* READ → event, park, or busy      */
    movl  kbd_ring_count(%rip), %ecx
    testl %ecx, %ecx
    jnz   .kbd_ep_pop_reply
    movl  kbd_park(%rip), %eax
    testl %eax, %eax
    jnz   .kbd_ep_read_busy            /* one parked reply max             */
    movl  92(%rsp), %eax               /* park the reply cap: answered     */
    movl  %eax, kbd_park(%rip)         /* from the next IRQ scancode       */
    jmp   .kbd_ep_drain
.kbd_ep_read_busy:
    movl  $KBD_EP_E_WOULD_BLOCK, %edx
    jmp   .kbd_ep_reply_err

.kbd_ep_pop_reply:                     /* ecx = ring count (nonzero)       */
    movl  kbd_ring_head(%rip), %eax    /* oldest = (head - count) & mask   */
    subl  %ecx, %eax
    andl  $(KBD_EP_RING_CAP - 1), %eax
    leaq  kbd_ring(%rip), %rdx
    movzbl (%rdx,%rax,1), %edx         /* edx = scancode                   */
    decl  %ecx
    movl  %ecx, kbd_ring_count(%rip)
    leaq  kbd_ep_msg(%rip), %rsi       /* OK reply: words[1] = scancode    */
    movq  $0,  0(%rsi)
    movq  $0,  8(%rsi)
    movq  $0, 16(%rsi)
    movq  $0, 24(%rsi)
    movq  $0, 32(%rsi)
    movq  $0, 40(%rsi)
    movq  $0, 48(%rsi)
    movq  $0, 56(%rsi)
    movl  %edx, 16(%rsi)               /* words[1]                         */
    movl  $2, 40(%rsi)                 /* word_count = 2                   */
    jmp   .kbd_ep_send_reply

.kbd_ep_reply_err:                     /* edx = uint32_t error image       */
    leaq  kbd_ep_msg(%rip), %rsi
    movq  $0,  0(%rsi)
    movq  $0,  8(%rsi)
    movq  $0, 16(%rsi)
    movq  $0, 24(%rsi)
    movq  $0, 32(%rsi)
    movq  $0, 40(%rsi)
    movq  $0, 48(%rsi)
    movq  $0, 56(%rsi)
    movq  $KBD_EP_REPLY_ERR_LABEL, %rax
    movq  %rax, 0(%rsi)                /* label = IRIS_EP_REPLY_ERR        */
    movl  %edx, 8(%rsi)                /* words[0] = error                 */
    movl  $1, 40(%rsi)                 /* word_count = 1                   */

.kbd_ep_send_reply:
    movl  92(%rsp), %edi
    leaq  kbd_ep_msg(%rip), %rsi
    movq  $SYS_REPLY, %rax
    syscall                            /* failure tolerated: caller died   */
    movl  92(%rsp), %edi
    movq  $15, %rax                    /* SYS_HANDLE_CLOSE: drop reply cap */
    syscall
    jmp   .kbd_ep_drain

/* ── Legacy service/IRQ channel (10 ms timeout keeps the EP drain alive) ── */
.kbd_chan_wait:
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
    movq  $0, 80(%rsp)
    movq  $SYS_CHAN_RECV_TIMEOUT, %rax
    movq  %r13,  %rdi
    movq  %rsp,  %rsi
    movq  $10000000, %rdx              /* 10 ms */
    syscall
    testq %rax,  %rax
    jz    .kbd_chan_msg
    cmpq  $-15,  %rax                  /* IRIS_ERR_TIMED_OUT: quiet loop   */
    je    .kbd_recv
    jmp   .kbd_recv_err
.kbd_chan_msg:

    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'r',  4(%rdi)
    movb  $'e',  5(%rdi)
    movb  $'c',  6(%rdi)
    movb  $'v',  7(%rdi)
    movb  $'\n', 8(%rdi)
    movb  $0,    9(%rdi)
    movq  $0,    %rax
    syscall

    /* Dispatch on message type */
    cmpl  $KBD_MSG_HELLO,        0(%rsp)
    je    .kbd_hello
    cmpl  $KBD_MSG_STATUS,       0(%rsp)
    je    .kbd_status
    cmpl  $KBD_MSG_SUBSCRIBE,    0(%rsp)
    je    .kbd_subscribe
    cmpl  $KBD_MSG_IRQ_SCANCODE, 0(%rsp)
    jne   .kbd_recv                    /* unknown type: silently ignore */

/* ── KBD_MSG_IRQ_SCANCODE: hardware event from IRQ routing layer ─────────
 * Kernel delivers the IRQ notification; ring-3 reads port 0x60 via KIoPort.
 * Key press:   scancode < 0x80
 * Key release: scancode >= 0x80 (bit 7 set; base key = scancode & 0x7F)
 * No reply is sent; print hex and loop.                              */
    movq  $SYS_IOPORT_IN, %rax        /* read PS/2 data register via KIoPort */
    movq  %r14, %rdi
    movq  $0,   %rsi                   /* offset 0 = port 0x60 */
    syscall
    testq %rax, %rax
    js    .kbd_recv                    /* I/O error: IRQ stays masked (stall) */
    movb  %al,  88(%rsp)               /* stash scancode above msg struct */
    movq  $SYS_IRQ_ACK, %rax          /* unmask IRQ1: byte consumed, safe to re-arm */
    movq  %rbx, %rdi
    syscall
    movzbl 88(%rsp), %ecx             /* ecx = scancode (reload; rax clobbered) */
    testb %cl,  %cl
    jz    .kbd_recv                    /* scancode 0 is spurious */

    leaq  96(%rsp), %rdi
    movb  $'[',  0(%rdi)
    movb  $'K',  1(%rdi)
    movb  $'B',  2(%rdi)
    movb  $'D',  3(%rdi)
    movb  $']',  4(%rdi)
    movb  $' ',  5(%rdi)
    movb  $'0',  6(%rdi)
    movb  $'x',  7(%rdi)

    movl  %ecx,  %eax
    shrl  $4,    %eax
    andl  $0xF,  %eax
    cmpl  $10,   %eax
    jl    .hi_dec
    addl  $('A' - 10), %eax
    jmp   .hi_store
.hi_dec:
    addl  $'0',  %eax
.hi_store:
    movb  %al,   8(%rdi)

    movl  %ecx,  %eax
    andl  $0xF,  %eax
    cmpl  $10,   %eax
    jl    .lo_dec
    addl  $('A' - 10), %eax
    jmp   .lo_store
.lo_dec:
    addl  $'0',  %eax
.lo_store:
    movb  %al,   9(%rdi)
    movb  $'\n', 10(%rdi)
    movb  $0,    11(%rdi)

    movq  $0,    %rax
    movq  $0,    %rsi
    movq  $0,    %rdx
    syscall

    /* ── EP delivery (Fase 7.4): parked READ reply first, else ring ────── */
    movl  kbd_park(%rip), %eax
    testl %eax, %eax
    jz    .kbd_irq_ring
    leaq  kbd_ep_msg(%rip), %rsi       /* OK reply: words[1] = scancode    */
    movq  $0,  0(%rsi)
    movq  $0,  8(%rsi)
    movq  $0, 16(%rsi)
    movq  $0, 24(%rsi)
    movq  $0, 32(%rsi)
    movq  $0, 40(%rsi)
    movq  $0, 48(%rsi)
    movq  $0, 56(%rsi)
    movzbl 88(%rsp), %eax
    movl  %eax, 16(%rsi)               /* words[1] = scancode              */
    movl  $2, 40(%rsi)                 /* word_count = 2                   */
    movl  kbd_park(%rip), %edi
    movq  $SYS_REPLY, %rax
    syscall                            /* failure tolerated: caller died   */
    movl  kbd_park(%rip), %edi
    movq  $15, %rax                    /* SYS_HANDLE_CLOSE: drop reply cap */
    syscall
    movl  $0, kbd_park(%rip)
    jmp   .kbd_irq_legacy
.kbd_irq_ring:                         /* push; drop OLDEST when full      */
    movzbl 88(%rsp), %eax
    movl  kbd_ring_head(%rip), %ecx
    leaq  kbd_ring(%rip), %rdx
    movb  %al, (%rdx,%rcx,1)
    incl  %ecx
    andl  $(KBD_EP_RING_CAP - 1), %ecx
    movl  %ecx, kbd_ring_head(%rip)
    movl  kbd_ring_count(%rip), %ecx
    cmpl  $KBD_EP_RING_CAP, %ecx
    jae   .kbd_irq_legacy              /* full: head advanced over oldest  */
    incl  %ecx
    movl  %ecx, kbd_ring_count(%rip)
.kbd_irq_legacy:
    /* Forward scancode to legacy subscriber if one is registered (init's
     * S7 path; sh is endpoint-only since Fase 7.4). */
    testq %r15,  %r15
    jz    .kbd_recv
    movzbl 88(%rsp), %r11d             /* reload scancode (captured from KIoPort) */
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
    movq  $0, 80(%rsp)
    movl  $KBD_MSG_SCANCODE_EVENT,     0(%rsp)   /* type                      */
    movb  %r11b,                       8(%rsp)   /* data[0] = raw scancode    */
    movl  $KBD_MSG_SCANCODE_EVENT_LEN, 72(%rsp)  /* data_len = 1              */
    movq  $13,   %rax                  /* SYS_CHAN_SEND */
    movq  %r15,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall
    testq %rax,  %rax
    jz    .kbd_recv                    /* send OK: loop                        */
    movq  $15,   %rax                  /* SYS_HANDLE_CLOSE: subscriber gone    */
    movq  %r15,  %rdi
    syscall
    movq  $0,    %r15
    jmp   .kbd_recv

/* ── KBD_MSG_HELLO: liveness probe ──────────────────────────────────────── */
.kbd_hello:
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'b',  4(%rdi)
    movb  $'o',  5(%rdi)
    movb  $'o',  6(%rdi)
    movb  $'t',  7(%rdi)
    movb  $'s',  8(%rdi)
    movb  $'t',  9(%rdi)
    movb  $'r', 10(%rdi)
    movb  $'a', 11(%rdi)
    movb  $'p', 12(%rdi)
    movb  $' ', 13(%rdi)
    movb  $'O', 14(%rdi)
    movb  $'K', 15(%rdi)
    movb  $'\n',16(%rdi)
    movb  $0,   17(%rdi)
    movq  $0,    %rax
    syscall

    /* Build KBD_MSG_HELLO_REPLY:
     *   type     = KBD_MSG_HELLO_REPLY  (0x80020001)
     *   data[0..3]: err = 0 (int32, OK)
     *   data_len = KBD_MSG_HELLO_REPLY_LEN (4)
     */
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
    movq  $0, 80(%rsp)
    movl  $KBD_MSG_HELLO_REPLY,       0(%rsp)   /* type                  */
    movl  $0,                         8(%rsp)   /* data[0..3]: err = 0   */
    movl  $KBD_MSG_HELLO_REPLY_LEN,  72(%rsp)   /* data_len = 4          */

    movq  $13,   %rax                  /* SYS_CHAN_SEND */
    movq  %r12,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall
    jmp   .kbd_recv

.kbd_recv_err:
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'r',  4(%rdi)
    movb  $'e',  5(%rdi)
    movb  $'c',  6(%rdi)
    movb  $'v',  7(%rdi)
    movb  $' ',  8(%rdi)
    movb  $'E',  9(%rdi)
    movb  $'R', 10(%rdi)
    movb  $'R', 11(%rdi)
    movb  $'\n',12(%rdi)
    movb  $0,   13(%rdi)
    movq  $0,    %rax
    syscall
    jmp   .kbd_recv

/* ── KBD_MSG_STATUS: controller status query ─────────────────────────────
 * Build KBD_MSG_STATUS_REPLY:
 *   type        = KBD_MSG_STATUS_REPLY  (0x80020002)
 *   data[0..3]: err   = 0 (int32, OK)
 *   data[4..7]: flags = KBD_STATUS_NORMAL (KBD_STATUS_READY | KBD_STATUS_PS2_OK)
 *   data_len    = KBD_MSG_STATUS_REPLY_LEN (8)
 */
.kbd_status:
    movl  76(%rsp), %r11d                /* preserve optional reply handle */
    movl  %r11d, 88(%rsp)
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
    movq  $0, 80(%rsp)
    movl  $KBD_MSG_STATUS_REPLY,          0(%rsp)  /* type                      */
    movl  $0,                             8(%rsp)  /* data[0..3]: err = 0       */
    movl  $KBD_STATUS_NORMAL,            12(%rsp)  /* data[4..7]: flags         */
    movl  $KBD_MSG_STATUS_REPLY_LEN,     72(%rsp)  /* data_len = 8              */

    movl  88(%rsp), %edi                 /* optional transferred reply handle */
    testl %edi, %edi
    jnz   .kbd_status_send
    movq  %r12, %rdi                     /* fallback: shared reply channel */

.kbd_status_send:
    movq  $13,   %rax                  /* SYS_CHAN_SEND */
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall
    testl %edi, %edi
    jz    .kbd_status_done
    cmpq  %r12, %rdi
    je    .kbd_status_done
    movq  $15,   %rax
    syscall

.kbd_status_done:
    jmp   .kbd_recv

/* ── KBD_MSG_SUBSCRIBE: register a scancode event subscriber ────────────────
 * attached_handle (76(%rsp)): write end of a KChannel the client reads from.
 * A new SUBSCRIBE replaces the previous subscriber (old handle closed first).
 * handle = 0 means unsubscribe: close existing subscriber.
 * Fire-and-forget: no reply is emitted, avoiding cross-client reply races on
 * the shared kbd reply channel.
 */
.kbd_subscribe:
    movl  76(%rsp), %r11d               /* incoming subscriber handle          */
    movl  %r11d,   88(%rsp)             /* stash in scratch slot (survives syscall) */
    testq %r15, %r15
    jz    .kbd_subscribe_store
    movq  $15,   %rax                   /* SYS_HANDLE_CLOSE: close old sub     */
    movq  %r15,  %rdi
    syscall
    movq  $0,    %r15
.kbd_subscribe_store:
    movl  88(%rsp), %r11d
    movq  %r11,    %r15                 /* r15 = new subscriber (0 = none)     */
    jmp   .kbd_recv

/* ── Bootstrap failure ───────────────────────────────────────────────────── */
.kbd_bootstrap_fail:
    leaq  96(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'b',  4(%rdi)
    movb  $'o',  5(%rdi)
    movb  $'o',  6(%rdi)
    movb  $'t',  7(%rdi)
    movb  $' ',  8(%rdi)
    movb  $'F',  9(%rdi)
    movb  $'A', 10(%rdi)
    movb  $'I', 11(%rdi)
    movb  $'L', 12(%rdi)
    movb  $'\n',13(%rdi)
    movb  $0,   14(%rdi)
    movq  $0,    %rax
    syscall

.kbd_dead:
    movq  $1,    %rax                  /* SYS_EXIT */
    movq  $0,    %rdi
    syscall
    hlt
    jmp   .kbd_dead

.section .note.GNU-stack,"",@progbits
