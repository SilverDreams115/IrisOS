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

int   abs(int);
long  labs(long);
int   atoi(const char *);
long  atol(const char *);
long  strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
void  qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t,
              int (*)(const void *, const void *));
/*
 * The environment is EMPTY on this system, and that is a design statement
 * rather than a gap: an inherited name-value table that changes what a program
 * can reach is the mechanism charter §6 objected to, and `LD_LIBRARY_PATH` is
 * the canonical case this system's linker refuses to have.  A program is told
 * things in `argv`, where the spawner can be seen saying them.
 */
extern char **environ;
char *getenv(const char *);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif
