/* SPDX-License-Identifier: Apache-2.0 */
/*
 * entry.S — PCI bus service entry point (Stage 10).
 *
 * RBX = 0 under the CPtr-mint bootstrap model (svc_loader delivers every cap as
 * a pre-start CSpace mint).  Aligns the stack, seeds the stack canary, passes
 * RBX as the first argument (RDI), and calls pci_main — a persistent service
 * loop that returns only on a SHUTDOWN request, after which we SYS_EXIT.
 *
 * It holds ONLY the caps its supervisor mints into its manifest: the endpoint
 * it serves on, an I/O-port capability for 0xCF8..0xCFF and nothing else, a
 * reply object, the PCI-hole device Untyped and a budget.  No spawn cap, no
 * IRQ, no VFS, no KDEBUG — a service that can describe the bus and hand out
 * one device's registers, and do nothing else at all.
 */

.global _start
.extern pci_main

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
    call  pci_main

.pci_dead:
    movq  $1, %rax      /* SYS_EXIT */
    movq  $0, %rdi
    syscall
    hlt
    jmp   .pci_dead

.section .note.GNU-stack,"",@progbits
