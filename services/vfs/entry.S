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
    /* Seed stack canary: RDRAND (hardware TRNG) with CPUID check + RDTSC fallback */
    pushq %rbx
    movl  $1, %eax
    cpuid
    movl  %ecx, %eax
    popq  %rbx
    btl   $30, %eax
    jnc   2f
    rdrand %rax
    jc    3f
2:  rdtsc
    shlq  $32, %rdx
    orq   %rdx, %rax
3:  movq  %rax, __stack_chk_guard(%rip)
    movq  %rbx, %rdi       /* arg0 = bootstrap handle */
    call  vfs_server_main_c

.vfs_dead:
    movq  $1,  %rax        /* SYS_EXIT */
    movq  $0,  %rdi        /* exit code 0 */
    syscall
    hlt
    jmp   .vfs_dead

.section .note.GNU-stack,"",@progbits
