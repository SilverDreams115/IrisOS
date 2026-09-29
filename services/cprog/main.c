/* SPDX-License-Identifier: Apache-2.0 */
/*
 * cprog/main.c — an ordinary C program.
 *
 * There is nothing about IRIS in this file.  It includes standard headers,
 * calls standard functions, and returns a status from `main`.  That is the
 * whole of Stage 10-run step 6's close condition: a C program nobody modified,
 * compiled against this libc, loaded from the filesystem at runtime, printing
 * to the console and exiting with a status somebody reads.
 *
 * What it costs the system to run this is: a `PT_INTERP` naming the C library,
 * an interpreter that relocates itself and then resolves this program's
 * `JUMP_SLOT` for `__libc_start_main` against the object table its spawner
 * filled, a thread pointer so the ordinary stack protector works, a heap out
 * of this program's own budget, and a console endpoint it was handed.  None of
 * that is visible from here, which is the point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

/*
 * The rest of what "an ordinary C program" means.
 *
 * Every call below is one a program written for any other system would make
 * without thinking, and each of them had to exist here before that sentence
 * was true.  They are checked rather than merely called: a `strtol` that
 * links and returns the wrong number is worse than one that does not link.
 *
 * It returns 0, or the first thing that was wrong.
 */
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int check_surface(void) {
    char buf[64];
    char *save = 0, *tok;
    int nums[5] = { 5, 3, 9, 1, 7 };
    const int want[5] = { 1, 3, 5, 7, 9 };
    char *dup;

    /* conversions, including the end pointer and the saturation */
    { char *end = 0;
      if (strtol("  -1234xyz", &end, 10) != -1234L) return 20;
      if (!end || *end != 'x') return 21;
      if (strtoul("0x1f", 0, 0) != 31u) return 22;
      if (atoi("42") != 42) return 23;
      errno = 0;
      if (strtoul("99999999999999999999999999", 0, 10) != (unsigned long)-1)
          return 24;
      if (errno != ERANGE) return 25; }

    /* searching and splitting */
    if (!strstr("abcdef", "cde")) return 26;
    if (strstr("abcdef", "xyz")) return 27;
    if (strstr("abc", "") != (char *)"abc" && strcmp(strstr("abc", ""), "abc"))
        return 28;
    if (strrchr("a/b/c", '/') == 0 || strcmp(strrchr("a/b/c", '/'), "/c"))
        return 29;
    if (!memchr("abc", 'b', 3)) return 30;
    if (strspn("aaab", "a") != 3u) return 31;
    if (strcspn("abc:d", ":") != 3u) return 32;

    strcpy(buf, "one two three");
    tok = strtok_r(buf, " ", &save);
    if (!tok || strcmp(tok, "one")) return 33;
    tok = strtok_r(0, " ", &save);
    if (!tok || strcmp(tok, "two")) return 34;

    /* building a string, and the precision that used to print itself */
    strcpy(buf, "ab");
    strcat(buf, "cd");
    strncat(buf, "efgh", 2);
    if (strcmp(buf, "abcdef")) return 35;
    if (snprintf(buf, sizeof buf, "[%.*s|%#x|%-4d]", 3, "abcdef", 255, 7) < 0)
        return 36;
    if (strcmp(buf, "[abc|0xff|7   ]")) return 37;

    /* sorting and searching over the result */
    qsort(nums, 5, sizeof nums[0], cmp_int);
    for (int i = 0; i < 5; i++) if (nums[i] != want[i]) return 38;
    { int key = 7;
      int *hit = bsearch(&key, nums, 5, sizeof nums[0], cmp_int);
      if (!hit || *hit != 7) return 39;
      key = 4;
      if (bsearch(&key, nums, 5, sizeof nums[0], cmp_int)) return 40; }

    /* the heap, through a function that uses it on the program's behalf */
    dup = strdup("copied");
    if (!dup || strcmp(dup, "copied")) return 41;
    free(dup);

    /* the environment is empty here, and that is the answer, not a failure */
    if (getenv("PATH")) return 42;

    /* errno is readable and strerror says something about it */
    errno = 0;
    if (!strerror(0) || errno != 0) return 43;

    assert(1);              /* it links, and does not fire */
    return 0;
}

int main(int argc, char **argv) {
    char *buf = malloc(64);
    size_t n;
    int bad;

    if (!buf) return 3;
    bad = check_surface();
    if (bad) { free(buf); return bad; }

    strcpy(buf, "IRIS");
    n = strlen(buf);
    if (n != 4u || strcmp(buf, "IRIS") != 0) { free(buf); return 4; }

    snprintf(buf, 64, "%s/%d", "libc", 6);
    if (strcmp(buf, "libc/6") != 0) { free(buf); return 5; }

    printf("[CPROG] hello from a C program: %s, argc=%d, argv[0]=%s\n",
           buf, argc, argc > 0 ? argv[0] : "?");
    printf("[CPROG] %d %u %x %-6s|%6s| %+d %05d %%\n",
           -42, 42u, 0xBEEFu, "left", "right", 7, 99);

    free(buf);

    /* A status somebody reads.  Seven, because zero is what a program that did
     * nothing would also return. */
    return 7;
}
