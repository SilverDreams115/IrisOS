.global vfs_server
.global vfs_probe_child
.extern vfs_server_main_c
.extern vfs_probe_child_main_c

.section .text
vfs_server:
    andq  $~0xFULL, %rsp
    movq  %rbx, %rdi
    call  vfs_server_main_c

vfs_probe_child:
    andq  $~0xFULL, %rsp
    movq  %rbx, %rdi
    call  vfs_probe_child_main_c

.vfs_dead:
    movq  $1, %rax
    movq  $1, %rdi
    syscall
    hlt
    jmp   .vfs_dead

.section .note.GNU-stack,"",@progbits
