#ifndef IRIS_SERIAL_H
#define IRIS_SERIAL_H
/* Test stub: serial output goes to stdout for tests */
#include <stdio.h>
static inline void serial_write(const char *s) { fputs(s, stdout); }
#endif
