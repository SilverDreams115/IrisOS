/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_BOOT_SELFTEST_H
#define IRIS_BOOT_SELFTEST_H

/*
 * What the kernel checks about ITSELF, before ring 3 exists to check anything.
 *
 * Two things, and both are things no ring-3 test could reach: that a
 * notification signals, waits, closes and cancels as its object promises, and
 * that a fault taken inside the kernel's own copy-to-user instruction is
 * caught by the exception table instead of halting the machine.
 */
int boot_selftest_run(void);

#endif
