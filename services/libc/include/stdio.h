/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

/*
 * What this stdio is, and is not.
 *
 * It writes to the CONSOLE, over the endpoint capability the spawner minted
 * into slot 3, and there is no file behind it — `stdout` and `stderr` are the
 * same place and there is no `fopen`.  Files arrive in step 7, where a
 * descriptor is a CPtr and `open` is a VFS grant; until then this is the half
 * of stdio that a program needs to say something and no more.
 */
int printf(const char *, ...);
int vprintf(const char *, va_list);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int puts(const char *);
int putchar(int);
int fflush(void *);
#endif
