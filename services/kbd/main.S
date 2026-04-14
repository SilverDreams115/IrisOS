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
#include <iris/svcmgr_proto.h>
#include <iris/kbd_proto.h>

.global _start

.section .text._start
_start:
    movq  $0, %r12                     /* reply channel handle (from svcmgr) */
    movq  $0, %r13                     /* service/IRQ channel handle          */

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

/* ── Bootstrap: receive service handle and reply handle ─────────────────── */
.kbd_bootstrap_recv:
    testq %r12, %r12
    jz    .kbd_bootstrap_need
    testq %r13, %r13
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
    cmpl  $SVCMGR_BOOTSTRAP_KIND_SERVICE, 8(%rsp)
    je    .kbd_bootstrap_service
    cmpl  $SVCMGR_BOOTSTRAP_KIND_REPLY,   8(%rsp)
    je    .kbd_bootstrap_reply
    jmp   .kbd_bootstrap_fail

.kbd_bootstrap_service:
    movq  %rax, %r13                   /* r13 = service/IRQ channel handle */
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_reply:
    movq  %rax, %r12                   /* r12 = reply channel handle        */
    jmp   .kbd_bootstrap_recv

.kbd_bootstrap_done:
    movq  $15,  %rax                   /* SYS_HANDLE_CLOSE: release bootstrap */
    movq  %rbx, %rdi
    syscall

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

/* ── Main service loop ───────────────────────────────────────────────────── */
.kbd_recv:
    movq  $14,   %rax                  /* SYS_CHAN_RECV */
    movq  %r13,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall
    testq %rax,  %rax
    jnz   .kbd_recv_err

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
    cmpl  $KBD_MSG_IRQ_SCANCODE, 0(%rsp)
    jne   .kbd_recv                    /* unknown type: silently ignore */

/* ── KBD_MSG_IRQ_SCANCODE: hardware event from IRQ routing layer ─────────
 * data[KBD_MSG_OFF_IRQ_SCANCODE] = raw PS/2 scancode byte.
 * Key press:   scancode < 0x80
 * Key release: scancode >= 0x80 (bit 7 set; base key = scancode & 0x7F)
 * No reply is sent; print hex and loop.                              */
    movzbl 8(%rsp), %ecx               /* data[0]: raw scancode */
    testb  %cl,  %cl
    jz    .kbd_recv                    /* scancode=0 is spurious; ignore */

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

    movq  $8,    %rax                  /* SYS_SLEEP */
    movq  $1,    %rdi
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
    movq  $8,    %rax                  /* SYS_SLEEP */
    movq  $1,    %rdi
    syscall
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
