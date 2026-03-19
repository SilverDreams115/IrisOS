.global _start
.extern iris_kernel_main

.section .text
_start:
    cli
    leaq kernel_stack_top(%rip), %rsp
    xorq %rbp, %rbp
    call iris_kernel_main

1:
    hlt
    jmp 1b

.section .bss
.align 16
kernel_stack:
    .skip 16384
kernel_stack_top:

.section .note.GNU-stack,"",@progbits
