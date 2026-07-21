#ifndef IRIS_NC_KSCHEDCTX_H
#define IRIS_NC_KSCHEDCTX_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

#define KSCHEDCTX_DEFAULT_BUDGET  5u   /* 50ms at 100Hz */
#define KSCHEDCTX_DEFAULT_PERIOD  20u  /* 200ms at 100Hz */

struct task;

struct KSchedContext {
    struct KObject  base;              /* must be first */
    irq_spinlock_t  lock;
    uint64_t        budget_ticks;      /* configured ticks per period */
    uint64_t        period_ticks;      /* period length in ticks */
    uint64_t        remaining_budget;  /* ticks left in current period */
    /* Fase S2: one-to-one binding (S2.9).  bound_task != NULL ⇒ this SC is
     * ligado a exactamente esa task; cleared on unbind / task death. */
    struct task    *bound_task;
    uint8_t         configured;        /* Fase S2: 0 hasta SC_CONFIGURE (B2) */
};

/* Fase S2: Untyped retype is the ONLY creation path (kslab kschedctx_alloc
 * retired; SYS_SC_CREATE returns NOT_SUPPORTED). */
struct KSchedContext *kschedctx_alloc_at(void *mem); /* untyped-backed */
void                  kschedctx_close(struct KSchedContext *sc);
iris_error_t          kschedctx_configure(struct KSchedContext *sc,
                                           uint64_t budget, uint64_t period);
/* Fase S2: bind/unbind a task (one-to-one, atomic). */
iris_error_t          kschedctx_bind(struct KSchedContext *sc, struct task *t);
void                  kschedctx_unbind(struct KSchedContext *sc, struct task *t);
/* Fase 17/S2: live count + high-water/retype/destroy diagnostics. */
uint32_t              kschedctx_live_count(void);
void                  kschedctx_stats(uint32_t *live, uint32_t *hwm,
                                      uint32_t *retyped, uint32_t *destroyed);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KSCHEDCTX_H */
