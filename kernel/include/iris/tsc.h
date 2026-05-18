#ifndef IRIS_TSC_H
#define IRIS_TSC_H

#include <stdint.h>

/*
 * TSC calibration — populated by pit_init() via PIT CH2 one-shot (~10ms).
 * tsc_hz:   TSC ticks per second (CPU frequency as seen by RDTSC).
 * tsc_boot: TSC value captured at the start of the calibration window.
 *
 * sys_clock_get uses these to compute nanoseconds since boot without
 * the 10 ms resolution floor of the PIT-based wall_ticks counter.
 */
extern volatile uint64_t tsc_hz;
extern volatile uint64_t tsc_boot;

static inline uint64_t iris_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

#endif /* IRIS_TSC_H */
