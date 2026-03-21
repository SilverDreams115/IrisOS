.global user_init

/* user_init: first user-space function
 * Runs in ring 3. No kernel access.
 * Just loops forever — validates ring 3 entry works.
 */
user_init:
    /* infinite loop in user space */
.loop:
    pause
    jmp .loop

.section .note.GNU-stack,"",@progbits
