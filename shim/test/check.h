/* The host tests' check-and-report macro. */
#ifndef CHAM_TEST_CHECK_H
#define CHAM_TEST_CHECK_H

#include <stdio.h>

static int failures;
#define CHECK(cond, ...)                         \
    do {                                         \
        int ok_ = (cond);                        \
        printf("  %s  ", ok_ ? "ok  " : "FAIL"); \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
        failures += !ok_;                        \
    } while (0)

#endif
