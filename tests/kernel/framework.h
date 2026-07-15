/* framework.h — minimal test harness for IRIS kernel unit tests */
#ifndef IRIS_TEST_FRAMEWORK_H
#define IRIS_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>

extern int g_pass;
extern int g_fail;

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    if ((a) == (b)) { \
        fprintf(stderr, "  FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        g_fail++; \
    } else { \
        g_pass++; \
    } \
} while (0)

#define ASSERT_NULL(p)    ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

/*
 * Fase S1 — untyped-child test fixtures.
 *
 * The kslab-backed kendpoint_alloc / knotification_alloc / kreply_alloc are
 * RETIRED: production objects are placement-initialized inside untyped-backed
 * blocks.  These host-test helpers fabricate an equivalent block on the heap:
 * KUNTYPED_ALIGN header (NULL parent pointer — kuntyped_release_child then
 * only zeroes the block on destroy) followed by the zeroed payload.  The
 * block itself is intentionally leaked (the test process exits).
 */
#include <string.h>

static inline void *test_untyped_child_block(unsigned long payload) {
    unsigned long total = 64u /* KUNTYPED_ALIGN */ + payload;
    unsigned char *blk = aligned_alloc(64u, (total + 63u) & ~63ul);
    if (!blk) return NULL;
    memset(blk, 0, (total + 63u) & ~63ul);
    return blk + 64u;
}

#define TEST_UT_ALLOC(type_struct, alloc_at_fn) \
    alloc_at_fn(test_untyped_child_block(sizeof(type_struct)))

#define TEST_SUITE(name) \
    do { printf("  suite: %s\n", name); } while (0)

#endif
