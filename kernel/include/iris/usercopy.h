#ifndef IRIS_USERCOPY_H
#define IRIS_USERCOPY_H

#include <stdint.h>

/* WRITE-BACK ONLY.  The read side (user_range_readable,
 * copy_from_user_checked, copy_user_cstr_bounded) is retired: since ledger
 * A-33 the kernel reads no input from a user pointer.  See usercopy.c. */
int      user_range_writable(uint64_t ptr, uint32_t len);
int      copy_to_user_checked(uint64_t dst_uptr, const void *src, uint32_t len);

/*
 * The exception table: given the address of a faulting instruction, where to
 * continue instead, or 0 if that instruction is not one the kernel is
 * prepared to take a fault on.
 *
 * The fault path asks this BEFORE it decides to halt.  See usercopy.c for what
 * the table is for and ledger A-37 for what it replaces.
 */
uint64_t exfixup_lookup(uint64_t fault_rip);

/* Counted so the mechanism can be observed: the path it protects is a race a
 * test cannot schedule, so a test fires the protected instruction at an
 * address certain to fault and watches this number move. */
void     exfixup_stat_taken(void);
uint32_t exfixup_taken_count(void);

/* The protected store itself (usercopy_asm.S): returns the number of bytes it
 * did NOT write, which is 0 on success and the whole remainder on a fault. */
unsigned long usercopy_store(void *dst, const void *src, unsigned long len);

#endif
