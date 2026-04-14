/*
 * entry.S — svcmgr service entry point.
 *
 * Entry contract (ELF process bootstrap):
 *   RBX = bootstrap KChannel handle delivered by the kernel via the standard
 *         task_set_bootstrap_arg0 mechanism (also at USER_STACK_TOP-8).
 *
 * We align the stack to 16 bytes, pass RBX as the first argument (RDI per
 * the System V ABI), call svcmgr_main_c, then perform a clean SYS_EXIT.
 */

.global _start
.extern svcmgr_main_c

.section .text._start
_start:
    andq  $~0xFULL, %rsp   /* align stack to 16 bytes */
    movq  %rbx, %rdi       /* arg0 = bootstrap handle */
    call  svcmgr_main_c

.svcmgr_dead:
    movq  $1,  %rax        /* SYS_EXIT */
    movq  $0,  %rdi        /* exit code 0 */
    syscall
    hlt
    jmp   .svcmgr_dead

.section .note.GNU-stack,"",@progbits
