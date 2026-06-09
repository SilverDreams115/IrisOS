#ifndef IRIS_NC_KSCHEDCTX_H
#define IRIS_NC_KSCHEDCTX_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

#define KSCHEDCTX_DEFAULT_BUDGET  5u   /* 50ms at 100Hz */
#define KSCHEDCTX_DEFAULT_PERIOD  20u  /* 200ms at 100Hz */

struct KSchedContext {
    struct KObject  base;              /* must be first */
    irq_spinlock_t  lock;
    uint64_t        budget_ticks;      /* configured ticks per period */
    uint64_t        period_ticks;      /* period length in ticks */
    uint64_t        remaining_budget;  /* ticks left in current period */
};

struct KSchedContext *kschedctx_alloc(void);
struct KSchedContext *kschedctx_alloc_at(void *mem); /* Ph79: untyped-backed */
void                  kschedctx_close(struct KSchedContext *sc);
iris_error_t          kschedctx_configure(struct KSchedContext *sc,
                                           uint64_t budget, uint64_t period);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KSCHEDCTX_H */
