/*
 * entry.S — vfs service entry point.
 *
 * Entry contract (ELF process bootstrap):
 *   RBX = bootstrap KChannel handle (arg0 via task_set_bootstrap_arg0).
 */

.global _start
.extern vfs_server_main_c

.section .text._start
_start:
    andq  $~0xFULL, %rsp   /* align stack to 16 bytes */
    movq  %rbx, %rdi       /* arg0 = bootstrap handle */
    call  vfs_server_main_c

.vfs_dead:
    movq  $1,  %rax        /* SYS_EXIT */
    movq  $0,  %rdi        /* exit code 0 */
    syscall
    hlt
    jmp   .vfs_dead

.section .note.GNU-stack,"",@progbits
