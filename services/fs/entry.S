/* SPDX-License-Identifier: Apache-2.0 */
/*
 * entry.S — persistent filesystem service entry point (Stage 10).
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers every cap as
 * a pre-start CSpace mint).  Aligns the stack, seeds the stack canary, passes
 * RBX as the first argument (RDI), and calls fs_main — a persistent service
 * loop that returns only on a SHUTDOWN request, after which we SYS_EXIT.
 *
 * It holds the least of any service here: the endpoint it serves on, a reply
 * object, an endpoint to the BLOCK service, and a budget.  No disk, no
 * controller, no device Untyped, no DMA authority — it cannot find storage,
 * only ask for it.  A compromised filesystem can corrupt the files on the one
 * disk it was given and nothing else.
 */

.global _start
.extern fs_main

.section .text._start
_start:
    andq  $~0xFULL, %rsp
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
    movq  %rbx, %rdi
    call  fs_main

.fs_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .fs_dead

.section .note.GNU-stack,"",@progbits
