/* test.h - minimal test harness */
#ifndef GQL_TEST_H
#define GQL_TEST_H

#include <stdio.h>
#include <string.h>

extern int t_failures, t_checks;

#define CHECK(cond) do { \
    t_checks++; \
    if (!(cond)) { t_failures++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(got, want) do { \
    const char *g_ = (got), *w_ = (want); \
    t_checks++; \
    if (!g_ || strcmp(g_, w_) != 0) { \
        t_failures++; \
        fprintf(stderr, "  FAIL %s:%d:\n    got:  %s\n    want: %s\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_); \
    } \
} while (0)

#define CHECK_CONTAINS(got, needle) do { \
    const char *g_ = (got), *n_ = (needle); \
    t_checks++; \
    if (!g_ || !strstr(g_, n_)) { \
        t_failures++; \
        fprintf(stderr, "  FAIL %s:%d:\n    %s\n    does not contain: %s\n", __FILE__, __LINE__, g_ ? g_ : "(null)", n_); \
    } \
} while (0)

#define TEST(name) static void name(void)
#define RUN(name) do { fprintf(stderr, "%s\n", #name); name(); } while (0)

#endif
