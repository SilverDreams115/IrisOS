/* SPDX-License-Identifier: Apache-2.0 */
#include "prog_stack.h"

static uint64_t ps_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/*
 * The layout, from the top of the region down:
 *
 *      (top)   the STRINGS argv and envp point at
 *              [padding to 16]
 *              AT_NULL, 0
 *              aux[auxc-1] .. aux[0]
 *              NULL
 *              envp[envc-1] .. envp[0]
 *              NULL
 *              argv[argc-1] .. argv[0]
 *   rsp →      argc
 *
 * Built top-down because the strings have to be placed before the pointers to
 * them can be written, and bottom-up because `%rsp` must land 16-byte aligned
 * on `argc`.  Doing it in two passes — measure, then write — is what keeps
 * those two requirements from arguing: the size is known before a byte moves.
 */
uint64_t prog_stack_build(void *map_base, uint64_t child_base, uint64_t bytes,
                          const char *const *argv, uint32_t argc,
                          const char *const *envp, uint32_t envc,
                          const struct prog_auxv *aux, uint32_t auxc) {
    if (!map_base || bytes < 64u) return 0;
    if (argc && !argv) return 0;
    if (envc && !envp) return 0;
    if (auxc && !aux)  return 0;

    uint8_t *base = (uint8_t *)map_base;

    /* ── pass 1: how much do the strings take ──────────────────────────── */
    uint64_t str_bytes = 0;
    for (uint32_t i = 0; i < argc; i++) str_bytes += ps_strlen(argv[i]) + 1u;
    for (uint32_t i = 0; i < envc; i++) str_bytes += ps_strlen(envp[i]) + 1u;

    /* The vectors: argc, argv + NULL, envp + NULL, aux pairs + AT_NULL pair. */
    uint64_t vec_words = 1u
                       + (uint64_t)argc + 1u
                       + (uint64_t)envc + 1u
                       + ((uint64_t)auxc + 1u) * 2u;
    uint64_t vec_bytes = vec_words * 8u;

    /*
     * Where the strings start, and where `%rsp` lands.
     *
     * The strings sit at the very top; the vectors sit below them; `%rsp` is
     * the bottom of the vectors and must be 16-byte aligned.  Rounding the
     * string block DOWN to eight keeps the vectors word-aligned, and the final
     * alignment of `%rsp` is done by lowering it, never by raising it into the
     * vectors it is supposed to point at.
     */
    if (str_bytes > bytes) return 0;
    uint64_t str_off = (bytes - str_bytes) & ~7ULL;
    if (vec_bytes > str_off) return 0;
    uint64_t rsp_off = (str_off - vec_bytes) & ~15ULL;

    /* ── pass 2: the strings, top-down, recording each child address ───── */
    uint64_t cursor = str_off;
    uint64_t *argp = (uint64_t *)(base + rsp_off + 8u);
    uint64_t *envpp = argp + argc + 1u;
    uint64_t *auxp  = envpp + envc + 1u;

    for (uint32_t i = 0; i < argc; i++) {
        uint64_t n = ps_strlen(argv[i]) + 1u;
        for (uint64_t k = 0; k < n; k++) base[cursor + k] = (uint8_t)argv[i][k];
        argp[i] = child_base + cursor;
        cursor += n;
    }
    argp[argc] = 0;

    for (uint32_t i = 0; i < envc; i++) {
        uint64_t n = ps_strlen(envp[i]) + 1u;
        for (uint64_t k = 0; k < n; k++) base[cursor + k] = (uint8_t)envp[i][k];
        envpp[i] = child_base + cursor;
        cursor += n;
    }
    envpp[envc] = 0;

    for (uint32_t i = 0; i < auxc; i++) {
        auxp[i * 2u]      = aux[i].type;
        auxp[i * 2u + 1u] = aux[i].val;
    }
    auxp[auxc * 2u]      = 0;   /* AT_NULL */
    auxp[auxc * 2u + 1u] = 0;

    *(uint64_t *)(base + rsp_off) = (uint64_t)argc;

    return child_base + rsp_off;
}
