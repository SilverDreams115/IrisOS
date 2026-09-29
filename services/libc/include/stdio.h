/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

/*
 * What this stdio is, and is not.
 *
 * `stdout` and `stderr` are the SAME place — the console endpoint the spawner
 * minted into slot 3 — because that is how many capabilities a program is
 * given.  Separating them is a decision made at the spawn, where it can be
 * seen, not something this library can manufacture.
 *
 * `stdin` is the end-of-file descriptor: this system has no path from a
 * keyboard to a program that did not ask the keyboard service itself, so a
 * read returns zero rather than an error and a program that reads until zero
 * terminates normally.
 *
 * `fopen` is read-only, and a mode that asks to write is REFUSED rather than
 * accepted and then failing later — see `<fcntl.h>` for the same rule applied
 * to the flags.
 *
 * There is no floating point in the conversion, and an unsupported conversion
 * returns negative instead of guessing: `services/libc/fmt.c` says why.
 */
typedef struct _IRIS_FILE FILE;

extern FILE *const stdin;
extern FILE *const stdout;
extern FILE *const stderr;

#define EOF (-1)

int printf(const char *, ...);
int vprintf(const char *, va_list);
int fprintf(FILE *, const char *, ...);
int vfprintf(FILE *, const char *, va_list);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int puts(const char *);
int putchar(int);
int fputs(const char *, FILE *);
int fputc(int, FILE *);
#define putc(c, f) fputc((c), (f))
size_t fwrite(const void *, size_t, size_t, FILE *);
size_t fread(void *, size_t, size_t, FILE *);
int fgetc(FILE *);
#define getc(f) fgetc(f)
char *fgets(char *, int, FILE *);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
int fileno(FILE *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
int fflush(FILE *);
void perror(const char *);
#endif
