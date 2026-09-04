/*
 * tests/kernel/include/iris/paging.h — the REAL header, plus test-only hooks.
 *
 * This used to be a hand-written copy, and it had drifted: USER_VMO_BASE and
 * USER_VMO_TOP carried invented values that did not match the kernel's, so a
 * test could compile a range check against different bounds than the code it
 * was checking.  That is the same failure the `struct task` copy had, found
 * the same way — by a link error, not by a test.
 *
 * `#include_next` picks up kernel/include/iris/paging.h, which compiles on the
 * host unchanged.  What remains here is only what the host suite ADDS: the
 * fault-injection hooks and the paging stub's own controls, which exist
 * nowhere in the kernel.
 */
#ifndef IRIS_TEST_PAGING_H
#define IRIS_TEST_PAGING_H

#include_next <iris/paging.h>

/*
 * The ONE thing the host cannot take from the real header.
 *
 * PHYS_TO_VIRT adds the kernel's physical-window base, which is correct in a
 * kernel and meaningless here: a unit test's "physical" addresses are
 * malloc'd host pointers, so the mapping has to be identity or every
 * translation produces a pointer into nothing.  Taking the real macro was
 * exactly that mistake and it segfaulted the kuntyped suite, which translates
 * a carve offset back to a usable pointer on every allocation.
 *
 * Everything else — the address-space constants, the flags, the declarations —
 * now comes from the kernel's own header, which is the point: the copy that
 * used to live here had invented USER_VMO_BASE and USER_VMO_TOP values that
 * did not match the kernel's, so a range check could be tested against
 * different bounds than it enforces.
 */
#undef  PHYS_TO_VIRT
#undef  VIRT_TO_PHYS
#define PHYS_TO_VIRT(p)   ((uint64_t)(p))
#define VIRT_TO_PHYS(v)   ((uint64_t)(v))

/* Test-only: the stub paging implementation's controls (tests/kernel/stubs.c). */
void paging_stub_reset(void);
void paging_stub_strict_levels(int on);
void kslab_fail_after(int n);
void kslab_clear_fail(void);
void paging_force_fail_next(void);
void paging_clear_force_fail(void);

#endif /* IRIS_TEST_PAGING_H */
