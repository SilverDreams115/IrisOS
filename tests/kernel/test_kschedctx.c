#include "framework.h"
#include <iris/nc/kschedctx.h>
#include <iris/nc/kobject.h>
#include <stdatomic.h>

void test_kschedctx(void) {
    TEST_SUITE("kschedctx");

    /* ── alloc sets defaults ── */
    struct KSchedContext *sc = kschedctx_alloc();
    ASSERT_NOT_NULL(sc);
    ASSERT_EQ(atomic_load(&sc->base.refcount),    1u);
    ASSERT_EQ(atomic_load(&sc->base.active_refs), 0u);
    ASSERT_EQ(sc->budget_ticks,     (uint64_t)KSCHEDCTX_DEFAULT_BUDGET);
    ASSERT_EQ(sc->period_ticks,     (uint64_t)KSCHEDCTX_DEFAULT_PERIOD);
    ASSERT_EQ(sc->remaining_budget, (uint64_t)KSCHEDCTX_DEFAULT_BUDGET);

    /* ── configure rejects budget=0 ── */
    ASSERT_EQ(kschedctx_configure(sc, 0, 100), IRIS_ERR_INVALID_ARG);

    /* ── configure rejects period=0 ── */
    ASSERT_EQ(kschedctx_configure(sc, 10, 0), IRIS_ERR_INVALID_ARG);

    /* ── configure rejects budget >= period ── */
    ASSERT_EQ(kschedctx_configure(sc, 50, 50), IRIS_ERR_INVALID_ARG);
    ASSERT_EQ(kschedctx_configure(sc, 51, 50), IRIS_ERR_INVALID_ARG);

    /* ── configure accepts budget < period ── */
    ASSERT_EQ(kschedctx_configure(sc, 10, 100), IRIS_OK);
    ASSERT_EQ(sc->budget_ticks,     (uint64_t)10);
    ASSERT_EQ(sc->period_ticks,     (uint64_t)100);
    ASSERT_EQ(sc->remaining_budget, (uint64_t)10);  /* reset to new budget */

    /* ── reconfigure updates all three fields ── */
    ASSERT_EQ(kschedctx_configure(sc, 3, 30), IRIS_OK);
    ASSERT_EQ(sc->budget_ticks,     (uint64_t)3);
    ASSERT_EQ(sc->period_ticks,     (uint64_t)30);
    ASSERT_EQ(sc->remaining_budget, (uint64_t)3);

    /* ── release destroys ── */
    kobject_release(&sc->base);

    /* ── configure(NULL, ...) is safe ── */
    ASSERT_EQ(kschedctx_configure(NULL, 10, 100), IRIS_ERR_INVALID_ARG);
}
