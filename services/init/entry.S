/*
 * entry.S — init service entry point.
 *
 * Entry contract (ELF process bootstrap):
 *   RBX = private bootstrap channel handle from the ring-3 userboot loader
 *
 * Calls init_main(bootstrap_ch_h) and exits with SYS_EXIT(0).
 * init_main is responsible for all teardown; it must not return on failure
 * (it calls SYS_EXIT directly).
 */
#include <iris/syscall.h>

.global _start

.section .text._start
_start:
    movq  %rbx, %rdi        /* arg0: bootstrap channel handle */
    call  init_main
    movq  $SYS_EXIT, %rax
    movq  $0, %rdi
    syscall
.L_dead:
    hlt
    jmp   .L_dead

.section .note.GNU-stack,"",@progbits
