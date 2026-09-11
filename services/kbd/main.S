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
#include <iris/invoke.h>
#include <iris/ipc_msg.h>
#include <iris/kbd_ep_proto.h>

.global _start

/* ── Endpoint state (Fase 7.4) ─────────────────────────────────────────────
 * Ledger A-33: there is no message STRUCT here any more.  A message arrives in
 * registers and leaves in registers, so what the driver keeps between the
 * receive and the reply is three words it actually uses — and it keeps them
 * because the reply path needs registers for other things, not because the
 * kernel has any idea they exist.
 *
 * kbd_msg_*:   what the last receive delivered: its label, the badge the
 *              kernel stamped, and the reply capability it echoed back.
 * kbd_ep_buf:  bulk landing zone; requests carrying bulk are rejected, and
 *              this only keeps the kernel's copy in bounds.
 * kbd_ring:    scancode ring, KBD_EP_RING_CAP deep, drop-oldest on overflow.
 * kbd_park:    parked KReply cap for a blocking KBD_EP_OP_READ (0 = none).
 * kbd_ep_h:    recv side of the service KEndpoint (0 = not granted).
 */
.section .bss
.align 8
kbd_msg_label:  .skip 8
kbd_msg_badge:  .skip 8
kbd_msg_reply:  .skip 8
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

    /* The bootstrap KChannel handle in entry RBX had a close here.  The handle
     * namespace is gone and SYS_HANDLE_CLOSE answers NOT_SUPPORTED, so the
     * call did nothing; it goes with the numbered ABI (A-32) rather than being
     * carried across as an invocation of a method that does not exist. */

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
    movq  $SYS_INVOKE, %rax
    movq  %r14, %rdi                   /* the KIoPort capability */
    movq  $INV_IOPORT_IN, %rsi
    movq  $4,   %rdx                   /* offset 4 = port 0x64 (status) */
    syscall
    testq %rax, %rax
    js    .kbd_ps2_flush_done          /* syscall error → stop */
    testb $1,   %al                    /* OBF set? */
    jz    .kbd_ps2_flush_done          /* no pending data → done */
    movq  $SYS_INVOKE, %rax
    movq  %r14, %rdi
    movq  $INV_IOPORT_IN, %rsi
    movq  $0,   %rdx                   /* offset 0 = port 0x60 (data) */
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
 *
 * Ledger A-23/A-24: ONE blocking receive takes both kinds of event.
 *
 * It used to drain the endpoint non-blockingly and then sleep 10 ms on the IRQ
 * notification, because a thread blocked receiving on an endpoint was deaf to
 * signals and the only way to serve both was to keep waking up.  A hundred
 * wakeups a second, to find nothing, forever.
 *
 * The notification is BOUND to this thread now, so a keystroke wakes it out of
 * the endpoint receive and arrives as a message labelled
 * IRIS_MSG_LABEL_NOTIFICATION.  Idle costs nothing.
 */
    movl  kbd_notif_h(%rip), %edx      /* bind the IRQ notification to us  */
    testl %edx, %edx
    jz    .kbd_recv
    movl  $19, %edi                    /* IRIS_CPTR_OWN_TCB */
    movq  $INV_TCB_BIND_NOTIFICATION, %rsi
    movq  $SYS_INVOKE, %rax
    syscall

.kbd_recv:
.kbd_ep_drain:
    movl  kbd_ep_h(%rip), %edi
    testl %edi, %edi
    jz    .kbd_no_ep                   /* no endpoint granted              */
    /* A-33: nothing to zero and no buffer to name.  A receive says where it
     * wants a capability put and which reply object to stage, and that is all
     * it says; the message comes back in registers.
     *
     * Fase S1: pass a FREE explicit reply object.  kbd owns
     * two reply objects (slots 13/14, minted by svcmgr): the one currently
     * parked (kbd_park) is bound to a waiting READ caller, so recv with the
     * other.  0 parked → use 13. */
    movl  $13, %r10d
    cmpl  $13, kbd_park(%rip)
    jne   .kbd_reply_pick_done
    movl  $14, %r10d
.kbd_reply_pick_done:
    /* A-33: rdi = the endpoint, rsi = the method, rdx = the slot a delivered
     * capability should land in (none), r10 = the reply object picked above. */
    xorl  %edx, %edx
    movq  $INV_EP_RECV, %rsi
    movq  $SYS_INVOKE, %rax            /* BLOCKING: the bound notification  */
    syscall                            /* wakes us for a keystroke too      */
    testq %rax, %rax
    jnz   .kbd_recv               /* error: retry rather than spin     */

    /*
     * A-33: unpack.  rdi = the badge the kernel stamped, rsi = the
     * MessageInfo, rdx = the reply capability this call is owed, and
     * r10..r15 = the message words.  The label is the MessageInfo's high
     * field and the bulk byte count its middle one — `iris/ipc_msg.h` is
     * where that layout is written down for both ends.
     */
    movq  %rdi, kbd_msg_badge(%rip)
    movq  %rdx, kbd_msg_reply(%rip)
    movq  %rsi, %rax
    shrq  $IRIS_MI_LABEL_SHIFT, %rax
    movq  %rax, kbd_msg_label(%rip)

    movq  $IRIS_MSG_LABEL_NOTIFICATION, %rcx   /* a SIGNAL, not a call?    */
    cmpq  %rcx, %rax
    je    .kbd_scancode

    movq  %rsi, %rcx                   /* bulk payload attached?           */
    shrq  $IRIS_MI_BUF_SHIFT, %rcx
    andq  $IRIS_MI_BUF_MASK, %rcx
    jnz   .kbd_ep_inval

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

.kbd_ep_ping:                          /* PING → OK; mr1 = sender badge    */
    /* A-33: a reply is a MessageInfo and message registers.  Label
     * IRIS_EP_REPLY_OK is 0, so the MessageInfo is just the word count. */
    movq  $2, %rdx                     /* label 0 (OK), length 2           */
    xorl  %r10d, %r10d                 /* mr0 = 0                          */
    movq  kbd_msg_badge(%rip), %r8     /* mr1 = the badge we observed      */
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
    movl  kbd_msg_reply(%rip), %eax    /* park the reply cap: answered     */
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
    movl  %edx, %r8d                   /* mr1 = scancode                   */
    xorl  %r10d, %r10d                 /* mr0 = 0                          */
    movq  $2, %rdx                     /* label 0 (OK), length 2           */
    jmp   .kbd_ep_send_reply

.kbd_ep_reply_err:                     /* edx = uint32_t error image       */
    movl  %edx, %r10d                  /* mr0 = the error                  */
    xorl  %r8d, %r8d
    movq  $((KBD_EP_REPLY_ERR_LABEL << IRIS_MI_LABEL_SHIFT) | 1), %rdx

.kbd_ep_send_reply:                    /* rdx = MessageInfo, r10/r8 = mr0/1 */
    movl  kbd_msg_reply(%rip), %edi
    xorl  %r9d, %r9d                   /* mr2 */
    xorl  %r15d, %r15d                 /* mr3 */
    xorl  %r14d, %r14d                 /* no capability travels            */
    xorl  %r13d, %r13d                 /* no receive slot                  */
    movq  $INV_REPLY_SEND, %rsi
    movq  $SYS_INVOKE, %rax
    syscall                            /* failure tolerated: caller died   */
    /* Fase S1: the reply CPtr (slot 13/14) is a reusable object — no close. */
    jmp   .kbd_ep_drain

/* No endpoint was granted: the IRQ notification is all there is to wait for,
 * so wait on it directly.  A driver with nothing to serve still has a line to
 * watch. */
.kbd_no_ep:
    movl  kbd_notif_h(%rip), %edi
    testl %edi, %edi
    jz    .kbd_recv
    leaq  kbd_notif_bits(%rip), %rdx
    movq  $INV_NOTIFY_WAIT, %rsi
    movq  $SYS_INVOKE, %rax
    syscall
    testq %rax, %rax
    jz    .kbd_scancode
    jmp   .kbd_recv

/* ── Scancode path: woken by the IRQ KNotification (Fase 7.6) ────────────
 * The kernel masks IRQ1, signals the notification and EOIs; ring-3 reads
 * port 0x60 via KIoPort and re-arms with SYS_IRQ_ACK.
 * Key press:   scancode < 0x80
 * Key release: scancode >= 0x80 (bit 7 set; base key = scancode & 0x7F) */
.kbd_scancode:
    movq  $SYS_INVOKE, %rax           /* read PS/2 data register via KIoPort */
    movq  %r14, %rdi
    movq  $INV_IOPORT_IN, %rsi
    movq  $0,   %rdx                   /* offset 0 = port 0x60 */
    syscall
    testq %rax, %rax
    js    .kbd_recv                    /* I/O error: IRQ stays masked (stall) */
    movb  %al,  88(%rsp)               /* stash scancode above msg struct */
    movq  $SYS_INVOKE, %rax           /* unmask IRQ1: byte consumed, safe to re-arm */
    movq  %rbx, %rdi
    movq  $INV_IRQ_ACK, %rsi
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
    /* A-33: the parked caller's answer, in registers.  Label OK is 0, so the
     * MessageInfo is the length alone; the scancode rides in mr1. */
    movzbl 88(%rsp), %eax
    movl  %eax, %r8d                   /* mr1 = scancode                   */
    xorl  %r10d, %r10d                 /* mr0 = 0                          */
    xorl  %r9d,  %r9d
    xorl  %r15d, %r15d
    xorl  %r14d, %r14d
    xorl  %r13d, %r13d
    movq  $2, %rdx                     /* label 0 (OK), length 2           */
    movl  kbd_park(%rip), %edi
    movq  $INV_REPLY_SEND, %rsi
    movq  $SYS_INVOKE, %rax
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
