.global user_init

/* COM1 serial direct output — no syscalls, ring0 diagnostic */
.macro serial_char ch
    movb  $\ch, %al
    movw  $0x3F8, %dx
1:  movw  $0x3FD, %dx
    inb   %dx, %al
    testb $0x20, %al
    jz    1b
    movw  $0x3F8, %dx
    movb  $\ch, %al
    outb  %al, %dx
.endm

.section .text
user_init:
    /* write "U" to COM1 to confirm we reached USER_TEXT_BASE */
    serial_char 'U'
    serial_char 'S'
    serial_char 'R'
    serial_char '\r'
    serial_char '\n'

    /* halt loop — no syscall, we're ring0 diagnostic */
.dead:
    hlt
    jmp .dead

.section .note.GNU-stack,"",@progbits
