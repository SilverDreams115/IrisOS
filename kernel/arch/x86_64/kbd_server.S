/*
 * kbd_server.S — keyboard server in ring 3.
 *
 * Entry contract (set by SYS_SPAWN via svcmgr):
 *   RBX = svcmgr bootstrap channel handle (not used for service logic).
 *
 * Startup sequence:
 *   1. SYS_NS_LOOKUP("kbd.reply", RIGHT_WRITE=2) → r12 (created and
 *      registered by svcmgr before this task is spawned; kbd_server
 *      sends responses here).
 *   2. SYS_NS_LOOKUP("kbd",       RIGHT_READ=1)  → r13 (pre-created by
 *      the kernel, registered by svcmgr; IRQ routing + client requests).
 *   3. Enter recv loop on r13.
 *
 * Both channels are registered by svcmgr before kbd_server is spawned.
 * "kbd.reply" is created by svcmgr via SYS_CHAN_CREATE and registered
 * via SYS_NS_REGISTER as part of the SVCMGR_MSG_SPAWN_SERVICE handler.
 * "kbd" is the kernel-pre-created IRQ channel registered by svcmgr.
 *
 * Protocol (minimal userland service contract):
 *   Request types:
 *     1 = KBD_OP_HELLO
 *     2 = KBD_OP_GET_STATUS
 *     4 = IRQ byte from kernel routing path
 *
 *   Response types:
 *     0x80000001 = KBD_RSP_HELLO
 *       data[0] = protocol version (1)
 *
 *     0x80000002 = KBD_RSP_STATUS
 *       data[0] = status flags
 *                 bit0 = service alive
 *                 bit1 = IRQ routing active
 *
 * KChanMsg layout (76 bytes):
 *   [0]   type      (4 bytes)
 *   [4]   sender_id (4 bytes)
 *   [8]   data[64]  ← scancode in data[0]
 *   [72]  data_len  (4 bytes)
 */
.global kbd_server

.section .text
kbd_server:
    /* r12 = reply channel handle (self-created via SYS_CHAN_CREATE) */
    movq  $0, %r12
    /* r13 = service/IRQ channel handle (NS lookup of "kbd") */
    movq  $0, %r13

    /* stack frame:
     *   [rsp+0  .. rsp+75]  KChanMsg  (76 bytes)
     *   [rsp+80 .. rsp+95]  print buffer "[KBD] 0xXX\n\0" (16 bytes)
     *   [rsp+96 .. rsp+111] name buffer (16 bytes)
     * Total = 112 bytes; align to 16.
     */
    subq  $112, %rsp
    andq  $~0xFULL, %rsp

    /* ── print "KBD start\n" ──────────────────────────────────────── */
    leaq  80(%rsp), %rdi
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

    /* ── SYS_NS_LOOKUP("kbd.reply", RIGHT_WRITE=2) → r12 ─────────── */
    leaq  96(%rsp), %rdi
    movb  $'k', 0(%rdi)
    movb  $'b', 1(%rdi)
    movb  $'d', 2(%rdi)
    movb  $'.', 3(%rdi)
    movb  $'r', 4(%rdi)
    movb  $'e', 5(%rdi)
    movb  $'p', 6(%rdi)
    movb  $'l', 7(%rdi)
    movb  $'y', 8(%rdi)
    movb  $0,   9(%rdi)
    movq  $25,  %rax                   /* SYS_NS_LOOKUP */
    movq  $2,   %rsi                   /* RIGHT_WRITE */
    syscall
    testq %rax, %rax
    js    .kbd_reply_fail               /* reply channel not in NS — fatal */
    movq  %rax, %r12

    /* ── SYS_NS_LOOKUP("kbd", RIGHT_READ=1) → r13 ────────────────── */
    leaq  96(%rsp), %rdi
    movb  $'k', 0(%rdi)
    movb  $'b', 1(%rdi)
    movb  $'d', 2(%rdi)
    movb  $0,   3(%rdi)
    movq  $25,  %rax                   /* SYS_NS_LOOKUP */
    movq  $1,   %rsi                   /* RIGHT_READ */
    syscall
    testq %rax, %rax
    js    .kbd_reply_fail               /* service channel not in NS — fatal */
    movq  %rax, %r13

    /* ── print "KBD ready\n" ──────────────────────────────────────── */
    leaq  80(%rsp), %rdi
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

.kbd_recv:
    /* SYS_CHAN_RECV = 14 */
    movq  $14,   %rax
    movq  %r13,  %rdi                  /* service/IRQ channel (was %rbx) */
    movq  %rsp,  %rsi                  /* &KChanMsg */
    movq  $0,    %rdx
    syscall
    testq %rax,  %rax
    jnz   .kbd_recv_err      /* error — diagnose then retry */

    leaq  80(%rsp), %rdi
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

    /* KBD_OP_HELLO from userland */
    cmpl  $1,    0(%rsp)
    je    .kbd_hello

    /* KBD_OP_GET_STATUS from userland */
    cmpl  $2,    0(%rsp)
    je    .kbd_status

    /* hardware IRQ payload */
    cmpl  $4,    0(%rsp)
    jne   .kbd_recv

    /* scancode is at KChanMsg.data[0] = offset 8 */
    movzbl 8(%rsp), %ecx
    testb  %cl,  %cl
    jz    .kbd_recv           /* ignore scancode 0 */

    /* build "[KBD] 0xXX\n\0" at rsp+80 */
    leaq  80(%rsp), %rdi
    movb  $'[',  0(%rdi)
    movb  $'K',  1(%rdi)
    movb  $'B',  2(%rdi)
    movb  $'D',  3(%rdi)
    movb  $']',  4(%rdi)
    movb  $' ',  5(%rdi)
    movb  $'0',  6(%rdi)
    movb  $'x',  7(%rdi)

    /* high nibble */
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

    /* low nibble */
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

    /* SYS_WRITE = 0; rdi already points to buffer */
    movq  $0,    %rax
    movq  $0,    %rsi
    movq  $0,    %rdx
    syscall

    jmp   .kbd_recv

.kbd_hello:
    leaq  80(%rsp), %rdi
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

    movq  $0,    %rax        /* SYS_WRITE */
    syscall

    /* response: KBD_RSP_HELLO, version=1 */
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
    movl  $0x80000001, 0(%rsp)
    movb  $1,          8(%rsp)
    movl  $1,         72(%rsp)

    movq  $13,   %rax       /* SYS_CHAN_SEND */
    movq  %r12,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall

    movq  $8,    %rax       /* SYS_SLEEP(1) */
    movq  $1,    %rdi
    syscall
    jmp   .kbd_recv

.kbd_recv_err:
    leaq  80(%rsp), %rdi
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

.kbd_status:
    leaq  80(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'s',  4(%rdi)
    movb  $'t',  5(%rdi)
    movb  $'a',  6(%rdi)
    movb  $'t',  7(%rdi)
    movb  $'u',  8(%rdi)
    movb  $'s',  9(%rdi)
    movb  $' ', 10(%rdi)
    movb  $'O', 11(%rdi)
    movb  $'K', 12(%rdi)
    movb  $'\n',13(%rdi)
    movb  $0,   14(%rdi)

    movq  $0,    %rax       /* SYS_WRITE */
    syscall

    /* response: KBD_RSP_STATUS, flags=alive|irq-routed = 0x03 */
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
    movl  $0x80000002, 0(%rsp)
    movb  $0x03,       8(%rsp)
    movl  $1,         72(%rsp)

    movq  $13,   %rax       /* SYS_CHAN_SEND */
    movq  %r12,  %rdi
    movq  %rsp,  %rsi
    movq  $0,    %rdx
    syscall

    movq  $8,    %rax       /* SYS_SLEEP(1) */
    movq  $1,    %rdi
    syscall
    jmp   .kbd_recv

.kbd_reply_fail:
    /* SYS_NS_LOOKUP("kbd") failed — cannot find service channel */
    leaq  80(%rsp), %rdi
    movb  $'K',  0(%rdi)
    movb  $'B',  1(%rdi)
    movb  $'D',  2(%rdi)
    movb  $' ',  3(%rdi)
    movb  $'c',  4(%rdi)
    movb  $'h',  5(%rdi)
    movb  $'a',  6(%rdi)
    movb  $'n',  7(%rdi)
    movb  $' ',  8(%rdi)
    movb  $'F',  9(%rdi)
    movb  $'A', 10(%rdi)
    movb  $'I', 11(%rdi)
    movb  $'L', 12(%rdi)
    movb  $'\n',13(%rdi)
    movb  $0,   14(%rdi)
    movq  $0,    %rax
    syscall
    jmp   .kbd_dead

.kbd_dead:
    movq  $1,    %rax        /* SYS_EXIT */
    movq  $0,    %rdi
    syscall
    hlt
    jmp   .kbd_dead

.section .note.GNU-stack,"",@progbits
