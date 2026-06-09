#include "framework.h"
#include <iris/nc/rights.h>

void test_rights(void) {
    TEST_SUITE("rights");

    /* rights_check: zero required always fails */
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_NONE));

    /* rights_check: exact match */
    ASSERT_TRUE(rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_READ));
    ASSERT_TRUE(rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_WRITE));
    ASSERT_TRUE(rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE));

    /* rights_check: missing bit fails */
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_WRITE));
    ASSERT_TRUE(!rights_check(RIGHT_READ, RIGHT_READ | RIGHT_DUPLICATE));

    /* rights_reduce: intersection is never an elevation */
    iris_rights_t full = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
    ASSERT_EQ(rights_reduce(full, RIGHT_READ), RIGHT_READ);
    ASSERT_EQ(rights_reduce(full, RIGHT_READ | RIGHT_WRITE), RIGHT_READ | RIGHT_WRITE);
    ASSERT_EQ(rights_reduce(RIGHT_READ, RIGHT_WRITE), RIGHT_NONE);

    /* rights_reduce: SAME_RIGHTS strips the flag and returns base */
    iris_rights_t base = RIGHT_READ | RIGHT_DUPLICATE;
    iris_rights_t r = rights_reduce(base, base | RIGHT_SAME_RIGHTS);
    ASSERT_EQ(r, base);
    ASSERT_TRUE(!(r & RIGHT_SAME_RIGHTS));

    /* rights_reduce: monotonic — result is always a subset of base */
    iris_rights_t res = rights_reduce(RIGHT_READ, RIGHT_READ | RIGHT_WRITE);
    ASSERT_TRUE((res & ~RIGHT_READ) == 0);

    /* RIGHT_NONE passthrough */
    ASSERT_EQ(rights_reduce(RIGHT_NONE, RIGHT_READ), RIGHT_NONE);
    ASSERT_EQ(rights_reduce(RIGHT_READ, RIGHT_NONE), RIGHT_NONE);
}
