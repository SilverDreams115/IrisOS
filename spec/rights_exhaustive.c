/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rights_exhaustive.c — the rights algebra, over EVERY input.
 *
 * Stage V phase 2, the part of D-3 that is a definition rather than a
 * catalogue.  The ledger records IRIS's rights set as "deliberate, revisable"
 * and says the two models "are not translatable one-to-one, so 'IRIS has
 * seL4's rights' is never a correct statement".  That is an honest note and
 * it is not a specification: a verified kernel needs the ALGEBRA written
 * down, not the observation that it differs.
 *
 * ── Why exhaustive and not sampled ─────────────────────────────────────────
 *
 * The rights word is seven bits.  `rights_reduce` and `rights_check` are
 * total functions over 2^7 x 2^7 = 16384 pairs, plus the `RIGHT_SAME_RIGHTS`
 * flag, which is small enough to enumerate completely — so this is not a test
 * that samples the space, it is a proof by exhaustion over it.  Everywhere
 * else in `spec/` a disagreement is found with probability; here there is no
 * probability left.
 *
 * It prints the two functions' whole graph, and `spec/rights_model.py`
 * computes the same graph independently.  A disagreement is a disagreement
 * about the algebra itself.
 */
#include <iris/nc/rights.h>
#include <stdio.h>
#include <stdint.h>

/* The seven rights IRIS has, and nothing else is a right. */
#define RIGHTS_MASK (RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | \
                     RIGHT_TRANSFER | RIGHT_WAIT | RIGHT_ROUTE | RIGHT_MANAGE)
#define NVALUES 128u          /* 2^7 */

int main(void) {
    /*
     * Every (base, requested) pair, and then the same pairs with the
     * SAME_RIGHTS flag set — which is an OPERATION flag and never persistent
     * state, so it has to be enumerated separately rather than folded into
     * the value space.
     */
    for (uint32_t base = 0; base < NVALUES; base++) {
        for (uint32_t req = 0; req < NVALUES; req++) {
            printf("reduce %u %u %u\n", base, req,
                   (unsigned)(rights_reduce((iris_rights_t)base,
                                            (iris_rights_t)req) & RIGHTS_MASK));
        }
    }
    for (uint32_t base = 0; base < NVALUES; base++) {
        printf("reduce_same %u %u\n", base,
               (unsigned)(rights_reduce((iris_rights_t)base,
                                        RIGHT_SAME_RIGHTS) & RIGHTS_MASK));
    }
    for (uint32_t have = 0; have < NVALUES; have++) {
        for (uint32_t need = 0; need < NVALUES; need++) {
            printf("check %u %u %d\n", have, need,
                   rights_check((iris_rights_t)have, (iris_rights_t)need));
        }
    }
    return 0;
}
