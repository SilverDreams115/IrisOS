/*
 * kbd_server.S — keyboard server in ring 3.
 *
 * The kernel creates a KChannel for IRQ1, registers it in the routing
 * table, and spawns this server passing the channel handle as arg0.
 *
 * Entry: RSP points to arg0 = handle_id (KChannel handle).
 *        First instruction: popq to receive it.
 *
 * Protocol (capability model):
 *   SYS_CHAN_RECV(handle, &msg)  →  msg.data[0] = scancode
 *   SYS_WRITE(&buf)              →  prints "[KBD] 0xXX\n"
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
    /* rbx = channel handle  (callee-saved across syscalls) */
    popq  %rbx

    /* stack frame:
     *   [rsp+0  .. rsp+75]  KChanMsg  (76 bytes)
     *   [rsp+80 .. rsp+95]  print buffer "[KBD] 0xXX\n\0" (16 bytes)
     * Total = 96 bytes; align to 16.
     */
    subq  $96, %rsp
    andq  $~0xFULL, %rsp

.kbd_recv:
    /* SYS_CHAN_RECV = 14 */
    movq  $14,   %rax
    movq  %rbx,  %rdi       /* handle_id */
    movq  %rsp,  %rsi       /* &KChanMsg */
    movq  $0,    %rdx
    syscall
    testq %rax,  %rax
    jnz   .kbd_recv          /* error — retry */

    /* bootstrap handshake from userland */
    cmpl  $1,    0(%rsp)
    je    .kbd_bootstrap

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

.kbd_bootstrap:
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
    jmp   .kbd_recv

.kbd_dead:
    movq  $1,    %rax        /* SYS_EXIT */
    movq  $0,    %rdi
    syscall
    hlt
    jmp   .kbd_dead

.section .note.GNU-stack,"",@progbits
