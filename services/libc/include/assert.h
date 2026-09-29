/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _ASSERT_H
#define _ASSERT_H

/*
 * assert — say where, then stop.
 *
 * It prints and then calls `abort`, which is `_exit(134)`: 128 + SIGABRT, by
 * the convention a supervisor reading the status expects, even though there
 * are no signals here for it to be the number OF.
 *
 * `NDEBUG` removes it entirely, as C requires, and the expression is then not
 * evaluated at all — which is why an assert must never be the only place
 * something happens.
 */

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void __assert_fail(const char *expr, const char *file, int line);
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__))
#endif

#endif /* _ASSERT_H */
