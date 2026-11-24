/* check.h - minimal assert-style test helpers. Each test file is its own binary. */
#ifndef OF_CHECK_H
#define OF_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures;
static int check_total;

#define CHECK(cond)                                                              \
    do {                                                                         \
        check_total++;                                                           \
        if (!(cond)) {                                                           \
            check_failures++;                                                    \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,   \
                    #cond);                                                      \
        }                                                                        \
    } while (0)

#define CHECK_STR(a, b) CHECK((a) != NULL && strcmp((a), (b)) == 0)

#define CHECK_DONE()                                                             \
    do {                                                                         \
        printf("%-28s %d/%d checks passed\n", __FILE__, check_total - check_failures, \
               check_total);                                                     \
        return check_failures ? 1 : 0;                                           \
    } while (0)

#endif
