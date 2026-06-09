#ifndef IRIS_KPAGE_H
#define IRIS_KPAGE_H
#include <stdint.h>
/* Test stub: kpage_alloc/kpage_free are provided by tests/kernel/stubs.c */
void *kpage_alloc(uint32_t size);
void  kpage_free(void *ptr, uint32_t size);
#endif
