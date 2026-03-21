#ifndef IRIS_SYSCALL_H
#define IRIS_SYSCALL_H

#include <stdint.h>

/* Syscall numbers */
#define SYS_WRITE   0
#define SYS_EXIT    1
#define SYS_GETPID  2
#define SYS_YIELD   3
#define SYS_OPEN    4
#define SYS_READ    5
#define SYS_CLOSE   6
#define SYS_BRK     7
#define SYS_SLEEP   8

void syscall_init(void);
void syscall_set_kstack(uint64_t kstack_top);

/* Called from ASM handler */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2);

#endif
