/* Test checks shared by the host suites (C and C++).
 *
 * CHECK(expr) always evaluates expr, unlike assert() it does not vanish under
 * -DNDEBUG (some checks call the code under test), and a failure is counted and
 * reported with file:line instead of stopping at the first one. It is an
 * expression worth 1 when expr held, so a check whose failure would crash the
 * next line can stop the test: `if (!CHECK(f)) return;`.
 * REQUIRE(expr) is for the few that must stop at once (a mock fed a bad
 * pointer, a setup step everything else needs).
 * CHECK_DONE("...") ends main(): prints the success line when nothing failed,
 * the failure count otherwise, and returns the exit status. */
#ifndef BL_TESTS_CHECK_H
#define BL_TESTS_CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* One counter per test program: each suite is a single translation unit. */
static inline int *check_failures_(void)
{
    static int failures;
    return &failures;
}

static inline int check_report_(int ok, const char *file, int line, const char *expr)
{
    if (!ok) {
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
        ++*check_failures_();
    }
    return ok;
}

static inline void check_require_(int ok, const char *file, int line, const char *expr)
{
    if (!ok) {
        fprintf(stderr, "%s:%d: required check failed, stopping: %s\n", file, line, expr);
        abort();
    }
}

static inline int check_done_(const char *success)
{
    const int failures = *check_failures_();
    if (failures == 0) {
        puts(success);
        return 0;
    }
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
}

#define CHECK(expr) check_report_(!!(expr), __FILE__, __LINE__, #expr)
#define REQUIRE(expr) check_require_(!!(expr), __FILE__, __LINE__, #expr)
#define CHECK_DONE(success) check_done_(success)

#endif
