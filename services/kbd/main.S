/*
 * main.S — keyboard service, ring 3 (standalone ELF binary).
 *
 * Entry contract (ELF process bootstrap):
 *   RBX = bootstrap channel handle from the loader (closed at startup, unused —
 *         every cap is a pre-start CSpace slot, see endpoint_proto.h IRIS_CPTR_*).
 *
 * Fase 13/Track I: kbd is endpoint + IRQ-notification only — the legacy
 * service/reply KChannel pair (HELLO/STATUS/SUBSCRIBE) is fully retired.
 *
 * Startup sequence:
 *   1. close the (unused) bootstrap channel
 *   2. adopt the CPtr slots: kbd.ep recv (5), IRQ notification (7),
 *      KIoPort (10, r14), KIrqCap (11, rbx)
 *   3. enter the endpoint+notification loop
 *
 * Protocol: iris/kbd_ep_proto.h (KBD_EP_OP_POLL/READ + IRIS_EP_OP_PING).
 *   Clients pull scancodes via the endpoint; the IRQ KNotification feeds the
 *   scancode ring and answers a parked blocking READ.  Service discovery and
 *   cap delivery is owned entirely by svcmgr/init (no nameserver calls).
 */
#include <iris/syscall.h>
#include <iris/kbd_ep_proto.h>

.global _start

/* ── Endpoint state (Fase 7.4) ─────────────────────────────────────────────
 * kbd_ep_msg:  struct IrisMsg (72 B, Fase 9) staging area for EP recv/reply.
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
kbd_ep_msg:     .skip 80               /* struct IrisMsg (80 B, Fase 11)   */
kbd_ep_buf:     .skip 256
kbd_ring:       .skip KBD_EP_RING_CAP
kbd_ring_head:  .skip 4
kbd_ring_count: .skip 4
kbd_park:       .skip 4
kbd_ep_h:       .skip 4
kbd_notif_h:    .skip 4
kbd_notif_bits: .skip 8

.section .text._start
_start:
    movq  $0, %r14                     /* KIoPort CPtr (set in bootstrap)     */

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

/* ── Fase 13 (Track I): endpoint/notification-only — all caps are CPtr slots ──
 * kbd_ep_h    = IRIS_CPTR_OWN_EP   (5):  recv side of "kbd.ep"
 * kbd_notif_h = IRIS_CPTR_IRQ_NOTIFY (7): WAIT side of the IRQ notification
 * r14         = IRIS_CPTR_IOPORT  (10):  KIoPort (CPtr; SYS_IOPORT_IN/OUT)
 * rbx         = IRIS_CPTR_IRQ_CAP (11):  KIrqCap (CPtr; SYS_IRQ_ACK)
 * The legacy service/reply KChannel pair (slots 8/9) is retired — kbd no
 * longer serves HELLO/STATUS/SUBSCRIBE; clients use the endpoint (POLL/READ/
 * PING) and the IRQ notification feeds the scancode ring. */
    movl  $5, kbd_ep_h(%rip)
    movl  $7, kbd_notif_h(%rip)

    /* release the unused bootstrap KChannel handle (entry RBX) BEFORE rbx is
     * reused for the irqcap CPtr — the bootstrap channel carries nothing now. */
    movq  $SYS_HANDLE_CLOSE, %rax
    movq  %rbx, %rdi
    syscall

    movq  $10, %r14                    /* IRIS_CPTR_IOPORT  (CPtr) */
    movq  $11, %rbx                    /* IRIS_CPTR_IRQ_CAP (CPtr) */

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

/* ── Main service loop (Fase 13/Track I: endpoint + IRQ notification only) ──
 *   1. drain all pending endpoint requests (SYS_EP_NB_RECV);
 *   2. block on the IRQ KNotification (10 ms timeout) so the endpoint drain
 *      keeps running even with no key traffic.
 */
.kbd_recv:
.kbd_ep_drain:
    movl  kbd_ep_h(%rip), %edi
    testl %edi, %edi
    jz    .kbd_wait                    /* no endpoint granted: wait on IRQ */
    leaq  kbd_ep_msg(%rip), %rsi       /* zero IrisMsg, stage bulk buffer  */
    movq  $0,  0(%rsi)
    movq  $0,  8(%rsi)
    movq  $0, 16(%rsi)
    movq  $0, 24(%rsi)
    movq  $0, 32(%rsi)
    movq  $0, 40(%rsi)
    movq  $0, 56(%rsi)
    movq  $0, 64(%rsi)                 /* sender_badge (kernel overwrites) */
    leaq  kbd_ep_buf(%rip), %rax
    movq  %rax, 48(%rsi)               /* buf_uptr = kbd_ep_buf            */
    /* Fase S1: pass a FREE explicit reply object in arg2 (rdx).  kbd owns
     * two reply objects (slots 13/14, minted by svcmgr): the one currently
     * parked (kbd_park) is bound to a waiting READ caller, so recv with the
     * other.  0 parked → use 13. */
    movl  $13, %edx
    cmpl  $13, kbd_park(%rip)
    jne   .kbd_reply_pick_done
    movl  $14, %edx
.kbd_reply_pick_done:
    movq  $SYS_EP_NB_RECV, %rax
    syscall
    testq %rax, %rax
    jnz   .kbd_wait               /* WOULD_BLOCK/err: nothing pending */

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

.kbd_ep_ping:                          /* PING → OK; words[1] = sender badge */
    leaq  kbd_ep_msg(%rip), %rsi
    movq  64(%rsi), %rcx               /* req sender_badge (Fase 9) — read */
                                       /* BEFORE the reply reuses the buf  */
    movq  $0,  0(%rsi)                 /* label = IRIS_EP_REPLY_OK         */
    movq  $0,  8(%rsi)                 /* words[0] = 0                     */
    movq  %rcx, 16(%rsi)               /* words[1] = observed badge        */
    movq  $0, 24(%rsi)
    movq  $0, 32(%rsi)
    movq  $0, 40(%rsi)
    movq  $0, 48(%rsi)
    movq  $0, 56(%rsi)
    movq  $0, 64(%rsi)                 /* sender_badge: kernel-owned       */
    movl  $2, 40(%rsi)                 /* word_count = 2                   */
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
    /* Fase S1: the reply CPtr (slot 13/14) is a reusable object — no close. */
    jmp   .kbd_ep_drain

/* ── Wait on the IRQ KNotification (Fase 13/Track I) ──────────────────────
 * The endpoint drain above is non-blocking; the only blocking point is the IRQ
 * KNotification (10 ms timeout so the EP drain keeps running with no key
 * traffic).  No legacy service KChannel — clients use the endpoint. */
.kbd_wait:
    movl  kbd_notif_h(%rip), %edi      /* block on the IRQ notification    */
    testl %edi, %edi
    jz    .kbd_recv
    leaq  kbd_notif_bits(%rip), %rsi
    movq  $SYS_NOTIFY_WAIT_TIMEOUT, %rax
    movq  $10000000, %rdx              /* 10 ms */
    syscall
    testq %rax,  %rax
    jz    .kbd_scancode                /* IRQ fired: consume the byte      */
    jmp   .kbd_recv                    /* TIMED_OUT/err: quiet loop        */

/* ── Scancode path: woken by the IRQ KNotification (Fase 7.6) ────────────
 * The kernel masks IRQ1, signals the notification and EOIs; ring-3 reads
 * port 0x60 via KIoPort and re-arms with SYS_IRQ_ACK.
 * Key press:   scancode < 0x80
 * Key release: scancode >= 0x80 (bit 7 set; base key = scancode & 0x7F) */
.kbd_scancode:
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
    /* Fase S1: parked reply CPtr is a reusable object — no close. */
    movl  $0, kbd_park(%rip)
    jmp   .kbd_recv
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
    jae   .kbd_recv                    /* full: head advanced over oldest  */
    incl  %ecx
    movl  %ecx, kbd_ring_count(%rip)
    jmp   .kbd_recv

/* ── Fase 13 (Track I): legacy KChannel handlers retired ──────────────────
 * The HELLO/STATUS/SUBSCRIBE handlers, the .kbd_irq_legacy subscriber push and
 * .kbd_recv_err are gone — kbd serves only the endpoint (POLL/READ/PING) and
 * the IRQ notification.  Liveness is covered by clients' kbd.ep PING. */
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
