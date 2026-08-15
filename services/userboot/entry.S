#include <iris/syscall.h>

.global _start
.extern iris_userboot_main

.section .text
_start:
    /* SysV x86-64 requires RSP to be 16-byte aligned at every `call`.  Every
     * other service entry stub does this; userboot did not, and the violation
     * stayed latent only because GCC had not yet emitted an aligned SSE store
     * for this translation unit.  The moment it did (a `= { 0 }` aggregate
     * initialiser compiled to `movaps`), userboot took a #GP at its first
     * store.  Align before the call — RBX (the bootstrap cap) is untouched. */
    andq  $~0xF, %rsp
    movq  %rbx, %rdi
    call  iris_userboot_main
    movq  $SYS_EXIT, %rax
    movq  $0, %rdi
    syscall
.L_dead:
    hlt
    jmp   .L_dead

.section .note.GNU-stack,"",@progbits
