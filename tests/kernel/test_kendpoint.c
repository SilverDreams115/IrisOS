#include "framework.h"
#include <iris/nc/kendpoint.h>
#include <iris/nc/kobject.h>
#include <stdatomic.h>

void test_kendpoint(void) {
    TEST_SUITE("kendpoint");

    /* alloc returns non-null and sets initial state */
    struct KEndpoint *ep = kendpoint_alloc();
    ASSERT_NOT_NULL(ep);
    ASSERT_EQ(ep->ep_state, EP_STATE_IDLE);
    ASSERT_EQ(ep->closed,   0);
    ASSERT_NULL(ep->queue_head);
    ASSERT_NULL(ep->queue_tail);

    /* initial lifecycle refcount is 1 */
    ASSERT_EQ((int)atomic_load(&ep->base.refcount), 1);

    /* kobject_retain increments refcount */
    kobject_retain(&ep->base);
    ASSERT_EQ((int)atomic_load(&ep->base.refcount), 2);

    /* kobject_release decrements; not yet at 0 so no destroy */
    kobject_release(&ep->base);
    ASSERT_EQ((int)atomic_load(&ep->base.refcount), 1);

    /* close on empty endpoint (no waiters): must not crash */
    kendpoint_close(ep);   /* drops last ref → triggers obj_close + destroy */

    /* alloc two more endpoints */
    struct KEndpoint *ep2 = kendpoint_alloc();
    ASSERT_NOT_NULL(ep2);
    struct KEndpoint *ep3 = kendpoint_alloc();
    ASSERT_NOT_NULL(ep3);
    ASSERT_NE(ep2, ep3);
    kendpoint_close(ep2);
    kendpoint_close(ep3);
}
