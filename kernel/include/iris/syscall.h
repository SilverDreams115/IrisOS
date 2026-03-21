#ifndef IRIS_SYSCALL_H
#define IRIS_SYSCALL_H

#include <stdint.h>

/* Syscall numbers */
#define SYS_WRITE   0
#define SYS_EXIT    1
#define SYS_GETPID  2
#define SYS_YIELD   3

void syscall_init(void);

/* Called from ASM handler */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2);

#endif
