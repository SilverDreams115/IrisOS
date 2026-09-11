#ifndef IRIS_DOMAIN_H
#define IRIS_DOMAIN_H

/*
 * domain.h — scheduling domains, seL4's top-level time partition.
 *
 * WHAT A DOMAIN IS, AND WHY IT IS NOT A PRIORITY
 *
 * Priority orders threads that COMPETE for the CPU.  A domain decides whether
 * they compete at all: a fixed, cyclic schedule says which domain owns the
 * processor for how long, and a thread runs only while its own domain is the
 * current one — whatever its priority, and whatever any other domain's threads
 * are doing.  A priority-255 thread in domain 1 does not preempt a priority-0
 * thread in domain 0; it waits for domain 1's slot.
 *
 * That is the whole point.  Priority is a scheduling policy and it leaks:
 * two threads at different priorities can measure each other through when they
 * get to run, which is a covert channel whose bandwidth depends on how busy
 * the other one is.  A time partition does not leak that way, because the
 * boundary is a SCHEDULE rather than a comparison — domain 0 gets its slot
 * whether or not domain 1 has anything to run, so what domain 1 does is not
 * observable in domain 0's timing.  This is why seL4 has domains at all, and
 * why the schedule is fixed at build time rather than computed: a schedule
 * somebody can influence is a schedule that carries information.
 *
 * IRIS'S SCHEDULE IS FIXED, LIKE seL4'S.  There is no invocation that edits
 * it, for the reason above.  What IS an invocation is `Domain_Set`, which
 * places a THREAD in a domain — seL4's `seL4_DomainSet_Set`, gated by its own
 * boot capability (IRIS_BOOTCAP_DOMAIN_CONTROL).
 *
 * THE DEFAULT IS ONE DOMAIN, and a system that never configures anything
 * behaves exactly as it did before domains existed: every thread is in domain
 * 0, the schedule has one entry, and the dispatcher searches one set of
 * queues.  seL4 ships CONFIG_NUM_DOMAINS = 1 for the same reason.
 */

/* How many domains exist.  Each costs a set of run queues per CPU, so this is
 * a real memory number and not a free ceiling: 256 priorities x 2 pointers x
 * MAX_CPUS per domain. */
#define IRIS_NUM_DOMAINS 4u

/* The fixed schedule's length.  A schedule is a cycle of (domain, ticks)
 * entries; the kernel walks it forever. */
#define IRIS_DOM_SCHEDULE_MAX 16u

#endif /* IRIS_DOMAIN_H */
