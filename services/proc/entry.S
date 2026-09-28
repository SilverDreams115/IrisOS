/* SPDX-License-Identifier: Apache-2.0 */
/*
 * entry.S — the program spawner's entry point.
 *
 * A SERVICE entry, not a program's: RBX carries nothing, the stack is bare,
 * and every capability arrived as a pre-start CSpace mint.  `proc` is the
 * thing that builds the other kind of entry — see services/hello/entry.S for
 * what it hands a program, and docs/contracts/program.md §4 for why the two
 * differ.
 *
 * What it holds: the endpoint it serves on, a reply object, an endpoint to the
 * VFS (to read an ELF by path), an endpoint to the console (handed on to what
 * it spawns), and a budget.  No ports, no device Untyped, no initrd
 * capability — a program comes from a FILE, and a spawner that could also
 * reach the kernel's boot images would be two mechanisms wearing one name.
 */

.global _start
.extern proc_main

.section .text._start
_start:
    andq  $-16, %rsp
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
    call  proc_main

.proc_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .proc_dead

.section .note.GNU-stack,"",@progbits
