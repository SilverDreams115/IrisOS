/* SPDX-License-Identifier: Apache-2.0 */
/*
 * usercopy_asm.S — the one store the kernel makes into a user address, and
 * the landing pad it uses when that store faults.
 *
 * ── Why this is in assembly ────────────────────────────────────────────────
 *
 * An exception table maps a FAULTING INSTRUCTION to the instruction that
 * should run instead.  That is a statement about a specific machine
 * instruction, so the instruction has to be one this file names and the
 * compiler cannot move, duplicate or reorder.  A C loop of byte stores gives
 * no such guarantee: the compiler may vectorise it, unroll it, or emit the
 * store at several addresses, and an exception table with one entry would
 * then cover one of them.
 *
 * `rep movsb` is one instruction, it is the whole copy, and on a fault it
 * leaves %rcx holding exactly how many bytes were NOT transferred.  So the
 * landing pad needs no bookkeeping: it returns what the hardware already put
 * in the register.
 */

    .section .text
    .globl usercopy_store
    .type usercopy_store, @function

/*
 * usercopy_store(void *dst_user, const void *src_kernel, unsigned long len)
 *   -> 0 if every byte was written, otherwise the number NOT written.
 *
 * The caller has already validated the range and, when SMAP is on, already
 * executed STAC.  A fault here means the mapping went away UNDER the check —
 * which is what this exists for.
 */
usercopy_store:
    movq    %rdx, %rcx
1:  rep movsb
    xorq    %rax, %rax
    ret
2:  movq    %rcx, %rax          /* bytes still owed; non-zero means it failed */
    ret
    .size usercopy_store, . - usercopy_store

/*
 * The entry itself.  `.ex_table` is collected by the linker script between
 * __ex_table_start and __ex_table_end, and the fault path walks it.
 */
    .section .ex_table, "a"
    .balign 8
    .quad   1b                  /* the instruction that may fault */
    .quad   2b                  /* where to continue instead      */
    .previous
