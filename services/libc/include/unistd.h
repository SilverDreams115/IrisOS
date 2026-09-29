/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
typedef long ssize_t;
ssize_t write(int, const void *, size_t);
_Noreturn void _exit(int);
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif
