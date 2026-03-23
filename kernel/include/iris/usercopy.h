#ifndef IRIS_USERCOPY_H
#define IRIS_USERCOPY_H

#include <stdint.h>

int      user_range_readable(uint64_t ptr, uint32_t len);
int      user_range_writable(uint64_t ptr, uint32_t len);
int      copy_from_user_checked(void *dst, uint64_t src_uptr, uint32_t len);
int      copy_to_user_checked(uint64_t dst_uptr, const void *src, uint32_t len);
uint32_t copy_user_cstr_bounded(uint64_t uptr, char *dst, uint32_t cap);

#endif
