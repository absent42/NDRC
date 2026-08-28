/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test.h - minimal dependency-free test harness for NDRC.
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_TEST_H
#define NDRC_TEST_H

#include <stdio.h>
#include <string.h>

static int ndrc_tests_run = 0;
static int ndrc_tests_failed = 0;
static const char *ndrc_current = "";

#define TEST(name) static void name(void)

#define RUN(name)                                                          \
    do {                                                                   \
        ndrc_current = #name;                                              \
        ndrc_tests_run++;                                                  \
        name();                                                            \
    } while (0)

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ndrc_tests_failed++;                                           \
            printf("FAIL %s (%s:%d): %s\n",                                \
                   ndrc_current, __FILE__, __LINE__, #cond);               \
        }                                                                  \
    } while (0)

#define CHECK_INT(got, want)                                               \
    do {                                                                   \
        long long ndrc_g = (long long)(got);                               \
        long long ndrc_w = (long long)(want);                              \
        if (ndrc_g != ndrc_w) {                                            \
            ndrc_tests_failed++;                                           \
            printf("FAIL %s (%s:%d): %s == %lld, want %lld\n",             \
                   ndrc_current, __FILE__, __LINE__, #got,                 \
                   ndrc_g, ndrc_w);                                        \
        }                                                                  \
    } while (0)

#define CHECK_STR(got, want)                                               \
    do {                                                                   \
        const char *ndrc_g = (got);                                        \
        const char *ndrc_w = (want);                                       \
        if (ndrc_g == NULL || ndrc_w == NULL ||                            \
            strcmp(ndrc_g, ndrc_w) != 0) {                                 \
            ndrc_tests_failed++;                                           \
            printf("FAIL %s (%s:%d): %s == \"%s\", want \"%s\"\n",         \
                   ndrc_current, __FILE__, __LINE__, #got,                 \
                   ndrc_g ? ndrc_g : "(null)",                             \
                   ndrc_w ? ndrc_w : "(null)");                            \
        }                                                                  \
    } while (0)

#define CHECK_MEM(got, want, n)                                            \
    do {                                                                   \
        if (memcmp((got), (want), (size_t)(n)) != 0) {                     \
            ndrc_tests_failed++;                                           \
            printf("FAIL %s (%s:%d): %s bytes differ\n",                   \
                   ndrc_current, __FILE__, __LINE__, #got);                \
        }                                                                  \
    } while (0)

static int test_summary(const char *suite)
{
    printf("%s: %d tests, %d failures\n",
           suite, ndrc_tests_run, ndrc_tests_failed);
    return ndrc_tests_failed == 0 ? 0 : 1;
}

#endif /* NDRC_TEST_H */
