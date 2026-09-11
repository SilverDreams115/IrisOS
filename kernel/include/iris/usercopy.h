#ifndef IRIS_USERCOPY_H
#define IRIS_USERCOPY_H

#include <stdint.h>

/* WRITE-BACK ONLY.  The read side (user_range_readable,
 * copy_from_user_checked, copy_user_cstr_bounded) is retired: since ledger
 * A-33 the kernel reads no input from a user pointer.  See usercopy.c. */
int      user_range_writable(uint64_t ptr, uint32_t len);
int      copy_to_user_checked(uint64_t dst_uptr, const void *src, uint32_t len);

#endif
