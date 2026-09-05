#ifndef IRIS_USER_CTX_H
#define IRIS_USER_CTX_H

#include <stdint.h>

/*
 * The complete ring-3 register state of a thread, as an object.
 *
 * Ledger D-1, step 3.  An event kernel has ONE kernel stack per core, which
 * means an interrupted thread's context cannot live on the stack the handler
 * runs on: the handler may hand the CPU to another thread, and that thread is
 * about to use the same stack.  So the context has to live somewhere that
 * belongs to the thread, and the only such place is its TCB.
 *
 * The layout is exactly what `isr_common` pushes, in push order — r15 lowest —
 * so the save and the restore are one `rep movsq` each rather than 22 field
 * copies with 22 chances to transpose two of them.  That coupling is the
 * reason this is a shared header and not a struct private to idt.c: the
 * assembly, the C handler and the TCB now all have to agree, and agreeing
 * through one definition is the only version of that which stays true.
 *
 * `vector` and `error_code` are part of the frame the CPU and the stubs build
 * and are carried here for the same reason the rest is: the restore rebuilds
 * the frame whole, and a frame missing two words is not a frame.
 */
struct iris_user_ctx {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

_Static_assert(sizeof(struct iris_user_ctx) == 22u * sizeof(uint64_t),
               "iris_user_ctx must match the isr_common frame exactly");

#endif /* IRIS_USER_CTX_H */
