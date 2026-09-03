// The whole of the test harness: a counter and two checks. Enough for tests
// on pure functions, and nothing to install.
#ifndef CHECK_H
#define CHECK_H

#include <math.h>
#include <stdio.h>

static int checksFailed;
static int checksRun;

#define CHECK(cond) do { \
        checksRun++; \
        if (!(cond)) { \
            checksFailed++; \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_NEAR(a, b, tol) do { \
        double checkA = (a), checkB = (b); \
        checksRun++; \
        if (fabs(checkA - checkB) > (tol)) { \
            checksFailed++; \
            fprintf(stderr, "%s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, \
                    #a, checkA, checkB); \
        } \
    } while (0)

static int checksDone(const char* suite) {
    printf("%s: %d checks, %d failed\n", suite, checksRun, checksFailed);
    return checksFailed == 0 ? 0 : 1;
}

#endif
