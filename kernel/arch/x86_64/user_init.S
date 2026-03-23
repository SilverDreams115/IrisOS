#include <iris/vfs_proto.h>

#define UI_STACK_BYTES      256
#define UI_OFF_SELF_PROC    120
#define UI_OFF_CHILD_PROC   128
#define UI_OFF_CHILD_BOOT   136
#define UI_OFF_RECLAIM_IDS  144

#define UI_BOOT_MSG_VFS_REQ   0x70010001
#define UI_BOOT_MSG_VFS_REPLY 0x70010002
#define UI_BOOT_MSG_SELF_PROC 0x70010003

.global user_init

.section .rodata
msg_start:      .asciz "init bootstrap start\n"
msg_svcmgr_boot_ok: .asciz "svcmgr bootstrap handle OK\n"
msg_svcmgr_boot_err: .asciz "svcmgr bootstrap FAILED\n"
msg_lookup_ok:  .asciz "kbd lookup OK\n"
msg_reply_ok:   .asciz "kbd.reply lookup OK\n"
msg_hello_ok:   .asciz "kbd hello reply OK\n"
msg_status_ok:  .asciz "kbd status reply OK\n"
msg_vfs_lookup_ok: .asciz "vfs lookup OK\n"
msg_vfs_reply_ok:  .asciz "vfs.reply lookup OK\n"
msg_vfs_open_ok:   .asciz "vfs open reply OK\n"
msg_vfs_read_ok:   .asciz "vfs read reply OK\n"
msg_vfs_close_ok:  .asciz "vfs close reply OK\n"
msg_vfs_stale_ok:  .asciz "vfs stale id reject OK\n"
msg_spawn_reject_ok: .asciz "spawn reject OK\n"
msg_spawn_reject_err: .asciz "spawn reject FAILED\n"
msg_vmo_ok:     .asciz "vmo create OK\n"
msg_vmo_zero_ok: .asciz "vmo zero reject OK\n"
msg_vmo_huge_ok: .asciz "vmo huge reject OK\n"
msg_vmo_err:    .asciz "vmo checks FAILED\n"
msg_handle_ok:  .asciz "handle transfer/dup OK\n"
msg_handle_dup_ok: .asciz "handle dup rights OK\n"
msg_handle_type_ok: .asciz "handle wrong-type OK\n"
msg_handle_spawn_ok: .asciz "handle target spawn OK\n"
msg_handle_xfer_ok: .asciz "handle transfer move OK\n"
msg_handle_err: .asciz "handle transfer/dup FAILED\n"
msg_spawn_loop_ok: .asciz "spawn exit loop OK\n"
msg_spawn_loop_err: .asciz "spawn exit loop FAILED\n"
msg_lookup_err: .asciz "kbd lookup FAILED\n"
msg_reply_err:  .asciz "kbd.reply lookup FAILED\n"
msg_hello_err:  .asciz "kbd hello FAILED\n"
msg_status_err: .asciz "kbd status FAILED\n"
msg_vfs_err:    .asciz "vfs read FAILED\n"
vfs_boot_file:  .asciz "iris.txt"
vfs_boot_data:  .asciz "Hello from IrisOS VFS!\n"

.section .text
user_init:
    movq $0, %rax
    leaq msg_start(%rip), %rdi
    syscall

    movq %rbx, %r15
    testq %r15, %r15
    jz .svcmgr_boot_fail

    movq $0, %rax
    leaq msg_svcmgr_boot_ok(%rip), %rdi
    syscall

    subq $UI_STACK_BYTES, %rsp
    andq $~0xFULL, %rsp
    movq %rsp, %rbp

    movl $1, %edi
    movl $2, %esi
    call .ui_lookup_service
    testq %rax, %rax
    js .lookup_fail
    movq %rax, %rbx

    movq $0, %rax
    leaq msg_lookup_ok(%rip), %rdi
    syscall

    movl $2, %edi
    movl $1, %esi
    call .ui_lookup_service
    testq %rax, %rax
    js .reply_fail
    movq %rax, %r12

    movq $0, %rax
    leaq msg_reply_ok(%rip), %rdi
    syscall

    call .ui_zero_msg
    movl $1,  0(%rbp)
    movb $'H', 8(%rbp)
    movl $1, 72(%rbp)

    movq $13, %rax
    movq %rbx, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .hello_fail

    movq $14, %rax
    movq %r12, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .hello_fail

    cmpl $0x80000001, 0(%rbp)
    jne .hello_fail
    cmpb $1, 8(%rbp)
    jne .hello_fail

    movq $0, %rax
    leaq msg_hello_ok(%rip), %rdi
    syscall

    call .ui_zero_msg
    movl $2,  0(%rbp)
    movl $0, 72(%rbp)

    movq $13, %rax
    movq %rbx, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .status_fail

    movq $14, %rax
    movq %r12, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .status_fail

    cmpl $0x80000002, 0(%rbp)
    jne .status_fail
    cmpb $0x03, 8(%rbp)
    jne .status_fail

    movq $0, %rax
    leaq msg_status_ok(%rip), %rdi
    syscall

    movl $3, %edi
    movl $6, %esi
    call .ui_lookup_service
    testq %rax, %rax
    js .vfs_fail
    movq %rax, %r13

    movq $0, %rax
    leaq msg_vfs_lookup_ok(%rip), %rdi
    syscall

    movl $4, %edi
    movl $5, %esi
    call .ui_lookup_service
    testq %rax, %rax
    js .vfs_fail
    movq %rax, %r14

    movq $0, %rax
    leaq msg_vfs_reply_ok(%rip), %rdi
    syscall

    movq $28, %rax
    syscall
    testq %rax, %rax
    js .vfs_fail
    movq %rax, UI_OFF_SELF_PROC(%rbp)

    call .ui_vfs_open_boot_file
    testq %rax, %rax
    js .vfs_fail

    movl %eax, 116(%rbp)

    movq $0, %rax
    leaq msg_vfs_open_ok(%rip), %rdi
    syscall

    call .ui_zero_msg
    movl $VFS_MSG_READ, 0(%rbp)
    movl 116(%rbp), %eax
    movl %eax, 8(%rbp)
    movl $52, 12(%rbp)
    movl $8, 72(%rbp)

    movq $13, %rax
    movq %r13, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    movq $14, %rax
    movq %r14, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    cmpl $VFS_MSG_READ_REPLY, 0(%rbp)
    jne .vfs_fail
    cmpl $0, 8(%rbp)
    jne .vfs_fail
    movl 116(%rbp), %eax
    cmpl %eax, 12(%rbp)
    jne .vfs_fail
    cmpl $23, 16(%rbp)
    jne .vfs_fail
    movq vfs_boot_data(%rip), %rax
    cmpq %rax, 20(%rbp)
    jne .vfs_fail
    movq vfs_boot_data+8(%rip), %rax
    cmpq %rax, 28(%rbp)
    jne .vfs_fail

    movq $0, %rax
    leaq msg_vfs_read_ok(%rip), %rdi
    syscall

    call .ui_zero_msg
    movl $VFS_MSG_CLOSE, 0(%rbp)
    movl 116(%rbp), %eax
    movl %eax, 8(%rbp)
    movl $4, 72(%rbp)

    movq $13, %rax
    movq %r13, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    movq $14, %rax
    movq %r14, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    cmpl $VFS_MSG_CLOSE_REPLY, 0(%rbp)
    jne .vfs_fail
    cmpl $0, 8(%rbp)
    jne .vfs_fail
    movl 116(%rbp), %eax
    cmpl %eax, 12(%rbp)
    jne .vfs_fail

    movq $0, %rax
    leaq msg_vfs_close_ok(%rip), %rdi
    syscall

#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
    call .ui_zero_msg
    movl $VFS_MSG_READ, 0(%rbp)
    movl 116(%rbp), %eax
    movl %eax, 8(%rbp)
    movl $16, 12(%rbp)
    movl $8, 72(%rbp)

    movq $13, %rax
    movq %r13, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    movq $14, %rax
    movq %r14, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    cmpl $VFS_MSG_READ_REPLY, 0(%rbp)
    jne .vfs_fail
    cmpl $-4, 8(%rbp)
    jne .vfs_fail

    movq $0, %rax
    leaq msg_vfs_stale_ok(%rip), %rdi
    syscall

    call .ui_zero_msg
    movl $VFS_MSG_RECLAIM_PROBE, 0(%rbp)
    movl $VFS_MSG_RECLAIM_PROBE_LEN, 72(%rbp)

    movq $13, %rax
    movq %r13, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    movq $14, %rax
    movq %r14, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .vfs_fail

    cmpl $VFS_MSG_RECLAIM_PROBE_REPLY, 0(%rbp)
    jne .vfs_fail
    cmpl $0, 8(%rbp)
    jne .vfs_fail

    movq $15, %rax
    movq %r13, %rdi
    syscall
    movq $15, %rax
    movq %r14, %rdi
    syscall
    xorq %r13, %r13
    xorq %r14, %r14

    movq $18, %rax
    movq $1,  %rdi
    movq $0,  %rsi
    syscall
    cmpq $-1, %rax
    jne .spawn_reject_fail

    movq $0, %rax
    leaq msg_spawn_reject_ok(%rip), %rdi
    syscall

    movq $16,   %rax
    movq $4096, %rdi
    syscall
    testq %rax, %rax
    js .vmo_fail
    movq %rax, %r13

    movq $15,  %rax
    movq %r13, %rdi
    syscall
    xorq %r13, %r13

    movq $0, %rax
    leaq msg_vmo_ok(%rip), %rdi
    syscall

    movq $16, %rax
    movq $0,  %rdi
    syscall
    cmpq $-1, %rax
    jne .vmo_fail

    movq $0, %rax
    leaq msg_vmo_zero_ok(%rip), %rdi
    syscall

    movq $16, %rax
    movq $0xFFFFFFFFFFFFF000, %rdi
    syscall
    cmpq $-1, %rax
    jne .vmo_fail

    movq $0, %rax
    leaq msg_vmo_huge_ok(%rip), %rdi
    syscall

    movq $19, %rax
    syscall
    testq %rax, %rax
    js .handle_fail
    movq %rax, %r13

    movq $22, %rax
    movq %r13, %rdi
    movq $1,   %rsi
    syscall
    testq %rax, %rax
    js .handle_fail
    movq %rax, %r14

    movq $20, %rax
    movq %r14, %rdi
    movq $1,   %rsi
    syscall
    cmpq $-5, %rax
    jne .handle_fail

    movq $0, %rax
    leaq msg_handle_dup_ok(%rip), %rdi
    syscall

    movq $26, %rax
    movq %r14, %rdi
    syscall
    cmpq $-9, %rax
    jne .handle_fail

    movq $0, %rax
    leaq msg_handle_type_ok(%rip), %rdi
    syscall

    movq $15, %rax
    movq %r14, %rdi
    syscall
    xorq %r14, %r14

    movq $18, %rax
    leaq phase3_sleep_child(%rip), %rdi
    movq $0, %rsi
    syscall
    testq %rax, %rax
    js .handle_fail
    movq %rax, %r14

    movq $0, %rax
    leaq msg_handle_spawn_ok(%rip), %rdi
    syscall

    movq $23, %rax
    movq %r13, %rdi
    movq %r14, %rsi
    movq $16,  %rdx
    syscall
    testq %rax, %rax
    js .handle_fail

    movq $0, %rax
    leaq msg_handle_xfer_ok(%rip), %rdi
    syscall

    movq $15, %rax
    movq %r13, %rdi
    syscall
    cmpq $-4, %rax
    jne .handle_fail
    xorq %r13, %r13

    movq $128, %r9
.handle_wait_dead:
    movq $26, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .handle_fail
    testq %rax, %rax
    jz .handle_dead
    movq $8, %rax
    movq $1, %rdi
    syscall
    decq %r9
    jnz .handle_wait_dead
    jmp .handle_fail

.handle_dead:
    movq $15, %rax
    movq %r14, %rdi
    syscall
    xorq %r14, %r14

    movq $0, %rax
    leaq msg_handle_ok(%rip), %rdi
    syscall

    movq $8, %r9
.spawn_loop:
    movq $18, %rax
    leaq phase3_exit_child(%rip), %rdi
    movq $0, %rsi
    syscall
    testq %rax, %rax
    js .spawn_loop_fail
    movq %rax, %r14
    movq $64, %r10
.spawn_loop_wait:
    movq $26, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    js .spawn_loop_fail
    testq %rax, %rax
    jz .spawn_loop_close
    movq $8, %rax
    movq $1, %rdi
    syscall
    decq %r10
    jnz .spawn_loop_wait
    jmp .spawn_loop_fail
.spawn_loop_close:
    movq $15, %rax
    movq %r14, %rdi
    syscall
    xorq %r14, %r14
    decq %r9
    jnz .spawn_loop

    movq $0, %rax
    leaq msg_spawn_loop_ok(%rip), %rdi
    syscall

    movq $15, %rax
    movq %rbx, %rdi
    syscall
    movq $15, %rax
    movq %r12, %rdi
    syscall
    movq $15, %rax
    movq %r15, %rdi
    syscall
    call .ui_close_local_slots
    xorq %rbx, %rbx
    xorq %r12, %r12
    xorq %r15, %r15

    movq $8, %rax
    movq $5, %rdi
    syscall
    jmp .exit_ok
#else
    movq $15, %rax
    movq %r13, %rdi
    syscall
    movq $15, %rax
    movq %r14, %rdi
    syscall
    movq $15, %rax
    movq %r15, %rdi
    syscall
    call .ui_close_local_slots
    xorq %rbx, %rbx
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15

    movq $8, %rax
    movq $5, %rdi
    syscall
    jmp .exit_ok
#endif

.svcmgr_boot_fail:
    movq $0, %rax
    leaq msg_svcmgr_boot_err(%rip), %rdi
    syscall
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $10, %rdi
    syscall

.lookup_fail:
    movq $0, %rax
    leaq msg_lookup_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $1, %rdi
    syscall

.reply_fail:
    movq $0, %rax
    leaq msg_reply_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $2, %rdi
    syscall

.hello_fail:
    movq $0, %rax
    leaq msg_hello_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $3, %rdi
    syscall

.status_fail:
    movq $0, %rax
    leaq msg_status_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $4, %rdi
    syscall

.vfs_fail:
    movq $0, %rax
    leaq msg_vfs_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $9, %rdi
    syscall

.spawn_reject_fail:
    movq $0, %rax
    leaq msg_spawn_reject_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $5, %rdi
    syscall

.vmo_fail:
    movq $0, %rax
    leaq msg_vmo_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $6, %rdi
    syscall

.handle_fail:
    movq $0, %rax
    leaq msg_handle_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $7, %rdi
    syscall

.spawn_loop_fail:
    movq $0, %rax
    leaq msg_spawn_loop_err(%rip), %rdi
    syscall
    call .ui_close_local_slots
    call .ui_close_runtime_handles
    movq $1, %rax
    movq $8, %rdi
    syscall

.exit_ok:
    movq $1, %rax
    movq $0, %rdi
    syscall

.dead:
    hlt
    jmp .dead

.ui_zero_msg:
    movq $0,  0(%rbp)
    movq $0,  8(%rbp)
    movq $0, 16(%rbp)
    movq $0, 24(%rbp)
    movq $0, 32(%rbp)
    movq $0, 40(%rbp)
    movq $0, 48(%rbp)
    movq $0, 56(%rbp)
    movq $0, 64(%rbp)
    movq $0, 72(%rbp)
    movq $0, 80(%rbp)
    ret

.ui_close_if_valid:
    testq %rdi, %rdi
    jz .ui_close_if_valid_done
    movq $15, %rax
    syscall
.ui_close_if_valid_done:
    ret

.ui_close_local_slots:
    movq UI_OFF_SELF_PROC(%rbp), %rdi
    call .ui_close_if_valid
    movq $0, UI_OFF_SELF_PROC(%rbp)
    movq UI_OFF_CHILD_PROC(%rbp), %rdi
    call .ui_close_if_valid
    movq $0, UI_OFF_CHILD_PROC(%rbp)
    movq UI_OFF_CHILD_BOOT(%rbp), %rdi
    call .ui_close_if_valid
    movq $0, UI_OFF_CHILD_BOOT(%rbp)
    ret

.ui_close_runtime_handles:
    movq %rbx, %rdi
    call .ui_close_if_valid
    movq %r12, %rdi
    call .ui_close_if_valid
    movq %r13, %rdi
    call .ui_close_if_valid
    movq %r14, %rdi
    call .ui_close_if_valid
    movq %r15, %rdi
    call .ui_close_if_valid
    ret

.ui_vfs_open_boot_file:
    movq $22, %rax
    movq UI_OFF_SELF_PROC(%rbp), %rdi
    movq $9, %rsi
    syscall
    testq %rax, %rax
    js .ui_vfs_open_ret
    movl %eax, 104(%rbp)

    call .ui_zero_msg
    movl $VFS_MSG_OPEN, 0(%rbp)
    movl $1,            8(%rbp)
    movq vfs_boot_file(%rip), %rax
    movq %rax,         12(%rbp)
    movl $13,          72(%rbp)
    movl 104(%rbp), %eax
    movl %eax,        76(%rbp)
    movl $1,          80(%rbp)

    movq $13, %rax
    movq %r13, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_vfs_open_send_fail

    movq $14, %rax
    movq %r14, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_vfs_open_ret

    cmpl $VFS_MSG_OPEN_REPLY, 0(%rbp)
    jne .ui_vfs_open_bad_reply
    cmpl $0, 8(%rbp)
    jne .ui_vfs_open_reply_err
    movl 12(%rbp), %eax
    testl %eax, %eax
    jz .ui_vfs_open_bad_reply
    ret

.ui_vfs_open_reply_err:
    movslq 8(%rbp), %rax
    ret

.ui_vfs_open_bad_reply:
    movq $-14, %rax
    ret

.ui_vfs_open_send_fail:
    movl 104(%rbp), %edi
    call .ui_close_if_valid
    movq $-14, %rax
.ui_vfs_open_ret:
    ret

.ui_vfs_close_file:
    movl %edi, 104(%rbp)
    call .ui_zero_msg
    movl $VFS_MSG_CLOSE, 0(%rbp)
    movl 104(%rbp), %eax
    movl %eax, 8(%rbp)
    movl $4, 72(%rbp)

    movq $13, %rax
    movq %r13, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_vfs_close_ret

    movq $14, %rax
    movq %r14, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_vfs_close_ret

    cmpl $VFS_MSG_CLOSE_REPLY, 0(%rbp)
    jne .ui_vfs_close_bad_reply
    cmpl $0, 8(%rbp)
    jne .ui_vfs_close_reply_err
    movl 104(%rbp), %eax
    cmpl %eax, 12(%rbp)
    jne .ui_vfs_close_bad_reply
    xorq %rax, %rax
    ret

.ui_vfs_close_reply_err:
    movslq 8(%rbp), %rax
    ret

.ui_vfs_close_bad_reply:
    movq $-14, %rax
.ui_vfs_close_ret:
    ret

.ui_send_boot_dup:
    movq %rdi, 104(%rbp)
    movl %edx, 112(%rbp)
    movl %ecx, 116(%rbp)

    movq $22, %rax
    movq %rsi, %rdi
    movl 116(%rbp), %esi
    orq  $8, %rsi
    syscall
    testq %rax, %rax
    js .ui_send_boot_dup_ret
    movl %eax, 108(%rbp)

    call .ui_zero_msg
    movl 112(%rbp), %eax
    movl %eax, 0(%rbp)
    movl 108(%rbp), %eax
    movl %eax, 76(%rbp)
    movl 116(%rbp), %eax
    movl %eax, 80(%rbp)

    movq $13, %rax
    movq 104(%rbp), %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_send_boot_dup_fail
    xorq %rax, %rax
    ret

.ui_send_boot_dup_fail:
    movl 108(%rbp), %edi
    call .ui_close_if_valid
    movq $-14, %rax
.ui_send_boot_dup_ret:
    ret

.ui_wait_proc_dead:
    movq %rdi, 104(%rbp)
    movq $64, %r10
.ui_wait_proc_dead_loop:
    movq $26, %rax
    movq 104(%rbp), %rdi
    syscall
    testq %rax, %rax
    js .ui_wait_proc_dead_fail
    testq %rax, %rax
    jz .ui_wait_proc_dead_ok
    movq $8, %rax
    movq $1, %rdi
    syscall
    decq %r10
    jnz .ui_wait_proc_dead_loop
.ui_wait_proc_dead_fail:
    movq $-14, %rax
    ret

.ui_wait_proc_dead_ok:
    xorq %rax, %rax
    ret

.ui_recv_boot_handle:
    movl %edi, 104(%rbp)
    call .ui_zero_msg
    movq $14, %rax
    movq %r15, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_recv_boot_handle_fail
    movl 104(%rbp), %eax
    cmpl %eax, 0(%rbp)
    jne .ui_recv_boot_handle_fail
    movl 76(%rbp), %eax
    testl %eax, %eax
    jz .ui_recv_boot_handle_fail
    ret

.ui_recv_boot_handle_fail:
    movq $-14, %rax
    ret

.ui_lookup_service:
    movl %edi,  96(%rbp)               /* endpoint */
    movl %esi, 100(%rbp)               /* requested rights */

    movq $12, %rax
    syscall
    testq %rax, %rax
    js .ui_lookup_fail_ret
    movl %eax, 104(%rbp)               /* base channel */

    movq $22, %rax
    movl 104(%rbp), %edi
    movq $1,  %rsi
    syscall
    testq %rax, %rax
    js .ui_lookup_fail_close_base
    movl %eax, 108(%rbp)               /* local recv handle */

    movq $22, %rax
    movl 104(%rbp), %edi
    movq $10, %rsi                     /* RIGHT_WRITE | RIGHT_TRANSFER */
    syscall
    testq %rax, %rax
    js .ui_lookup_fail_close_recv
    movl %eax, 112(%rbp)               /* transferred reply handle */

    movq $15, %rax
    movl 104(%rbp), %edi
    syscall

    call .ui_zero_msg
    movl $0x0003, 0(%rbp)
    movl  96(%rbp), %eax
    movl  %eax,      8(%rbp)
    movl 100(%rbp), %eax
    movl  %eax,     12(%rbp)
    movl $8,   72(%rbp)
    movl 112(%rbp), %eax
    movl  %eax,     76(%rbp)
    movl $2,   80(%rbp)

    movq $13, %rax
    movq %r15, %rdi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_lookup_fail_close_xfer

    movq $14, %rax
    movl 108(%rbp), %edi
    movq %rbp, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    jnz .ui_lookup_fail_close_recv_only

    movq $15, %rax
    movl 108(%rbp), %edi
    syscall

    cmpl $0x8003, 0(%rbp)
    jne  .ui_lookup_bad_reply
    movl 96(%rbp), %eax
    cmpl %eax, 12(%rbp)
    jne  .ui_lookup_bad_reply
    cmpl $0, 8(%rbp)
    jne  .ui_lookup_reply_err
    movl 76(%rbp), %eax
    testl %eax, %eax
    jz   .ui_lookup_bad_reply
    ret

.ui_lookup_reply_err:
    movslq 8(%rbp), %rax
    ret

.ui_lookup_bad_reply:
    movq $-14, %rax
    ret

.ui_lookup_fail_close_xfer:
    movl 112(%rbp), %edi
    call .ui_close_if_valid
.ui_lookup_fail_close_recv_only:
    movq $15, %rax
    movl 108(%rbp), %edi
    syscall
    movq $-14, %rax
    ret

.ui_lookup_fail_close_recv:
    movq $15, %rax
    movl 108(%rbp), %edi
    syscall
.ui_lookup_fail_close_base:
    movq $15, %rax
    movl 104(%rbp), %edi
    syscall
.ui_lookup_fail_ret:
    ret

.global phase3_exit_child
phase3_exit_child:
    movq $1, %rax
    movq $0, %rdi
    syscall
.phase3_dead:
    hlt
    jmp .phase3_dead

.global phase3_sleep_child
phase3_sleep_child:
    movq $8, %rax
    movq $10, %rdi
    syscall
    movq $1, %rax
    movq $0, %rdi
    syscall
.phase3_sleep_dead:
    hlt
    jmp .phase3_sleep_dead

.global vfs_leak_child
vfs_leak_child:
    movq %rbx, %r15
    subq $UI_STACK_BYTES, %rsp
    andq $~0xFULL, %rsp
    movq %rsp, %rbp

    movl $0, UI_OFF_SELF_PROC(%rbp)
    movl $0, UI_OFF_CHILD_PROC(%rbp)
    movl $0, UI_OFF_CHILD_BOOT(%rbp)

    movl $UI_BOOT_MSG_VFS_REQ, %edi
    call .ui_recv_boot_handle
    testq %rax, %rax
    js .vfs_leak_child_fail
    movq %rax, %r13

    movl $UI_BOOT_MSG_VFS_REPLY, %edi
    call .ui_recv_boot_handle
    testq %rax, %rax
    js .vfs_leak_child_fail
    movq %rax, %r14

    movl $UI_BOOT_MSG_SELF_PROC, %edi
    call .ui_recv_boot_handle
    testq %rax, %rax
    js .vfs_leak_child_fail
    movq %rax, UI_OFF_SELF_PROC(%rbp)

    movq %r15, %rdi
    call .ui_close_if_valid
    xorq %r15, %r15

    call .ui_vfs_open_boot_file
    testq %rax, %rax
    js .vfs_leak_child_fail

    movq $1, %rax
    movq $0, %rdi
    syscall

.vfs_leak_child_fail:
    movq $1, %rax
    movq $11, %rdi
    syscall

.vfs_leak_child_dead:
    hlt
    jmp .vfs_leak_child_dead

.section .note.GNU-stack,"",@progbits
