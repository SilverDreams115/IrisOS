/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _STRING_H
#define _STRING_H
#include <stddef.h>
void  *memcpy(void *, const void *, size_t);
void  *memmove(void *, const void *, size_t);
void  *memset(void *, int, size_t);
int    memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
char  *strcpy(char *, const char *);
char  *strncpy(char *, const char *, size_t);
int    strcmp(const char *, const char *);
int    strncmp(const char *, const char *, size_t);
char  *strchr(const char *, int);
char  *strrchr(const char *, int);
char  *strstr(const char *, const char *);
char  *strcat(char *, const char *);
char  *strncat(char *, const char *, size_t);
void  *memchr(const void *, int, size_t);
size_t strnlen(const char *, size_t);
size_t strspn(const char *, const char *);
size_t strcspn(const char *, const char *);
char  *strpbrk(const char *, const char *);
/* The reentrant form is the implementation; `strtok` is a wrapper over a
 * static, which is the only thread-unsafe thing in <string.h> here. */
char  *strtok_r(char *, const char *, char **);
char  *strtok(char *, const char *);
/* `strdup` allocates, so it is the one function in this header that can fail
 * for a reason that has nothing to do with strings: the program's budget. */
char  *strdup(const char *);
/* What an `errno` value means, in this system's own words rather than POSIX's
 * — what EACCES denied here is a capability, and saying so is more use to
 * somebody reading a program's output on this machine. */
const char *strerror(int);
#endif
