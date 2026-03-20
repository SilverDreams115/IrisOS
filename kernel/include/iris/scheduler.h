#ifndef IRIS_SCHEDULER_H
#define IRIS_SCHEDULER_H

#include <stdint.h>

void scheduler_init(void);
void scheduler_tick(void);
void scheduler_add_task(void (*entry)(void));

#endif
