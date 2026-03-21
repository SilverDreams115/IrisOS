.global user_entry_trampoline

/* user_entry_trampoline:
 * Called via context_switch ret. The kernel stack at this point
 * has an iretq frame pushed by task_create_user:
 *   [rsp+0]  = RIP  (user entry)
 *   [rsp+8]  = CS   (0x1B = user code | RPL3)
 *   [rsp+16] = RFLAGS (0x202 = IF set)
 *   [rsp+24] = RSP  (user stack)
 *   [rsp+32] = SS   (0x23 = user data | RPL3)
 *
 * Zero all general-purpose registers before jumping to user space.
 */
user_entry_trampoline:
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
    xorq %r8,  %r8
    xorq %r9,  %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    xorq %rbp, %rbp
    iretq

.section .note.GNU-stack,"",@progbits
