#include <stdint.h>

/* Canary seeded by RDTSC in each service's entry.S before the first C frame.
 * Non-zero initial value catches null-write overwrites before seeding runs. */
uintptr_t __stack_chk_guard = (uintptr_t)0xDEADBEEFCAFEBABEULL;

__attribute__((noreturn))
void __stack_chk_fail(void) {
    __asm__ volatile (
        "movq $1,   %%rax\n"   /* SYS_EXIT */
        "movq $255, %%rdi\n"
        "syscall\n"
        "hlt\n"
        ::: "rax", "rdi", "memory"
    );
    __builtin_unreachable();
}
