/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
_Noreturn void exit(int);
_Noreturn void abort(void);
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void  free(void *);
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif
