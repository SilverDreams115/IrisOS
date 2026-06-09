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

#define TEST_SUITE(name) \
    do { printf("  suite: %s\n", name); } while (0)

#endif
