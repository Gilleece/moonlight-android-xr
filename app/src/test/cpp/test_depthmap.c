// The CPU side of the depth map: the percentile range, the low pass and the
// map size check
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "xr_depthmap.h"

#define N 64

static void testRobustRange(void) {
    float* v = malloc(N * N * sizeof(float));
    // A ramp from 0 to 1
    for (int i = 0; i < N * N; i++) {
        v[i] = i / (float)(N * N - 1);
    }
    float lo, hi;
    robustRange(v, N * N, &lo, &hi);
    CHECK_NEAR(lo, 0.02f, 0.01);
    CHECK_NEAR(hi, 0.98f, 0.01);

    // A few wild pixels must not own the mapping the way the raw min and max
    // would
    v[0] = -100.0f;
    v[1] = 100.0f;
    robustRange(v, N * N, &lo, &hi);
    CHECK(lo > -1.0f && lo < 1.0f);
    CHECK(hi > 0.0f && hi < 2.0f);

    // A flat map still comes back with a range to divide by
    for (int i = 0; i < N * N; i++) {
        v[i] = 0.25f;
    }
    robustRange(v, N * N, &lo, &hi);
    CHECK_NEAR(lo, 0.25f, 1e-6);
    CHECK(hi > lo);
    free(v);
}

static void testLowPass(void) {
    float* src = calloc(N * N, sizeof(float));
    float* dst = calloc(N * N, sizeof(float));
    float* scratch = calloc(N * N, sizeof(float));
    float* colSums = calloc(N, sizeof(float));

    // A flat field passes through untouched
    for (int i = 0; i < N * N; i++) {
        src[i] = 0.6f;
    }
    lowPass(src, dst, scratch, colSums, N, N, 3, 3);
    CHECK_NEAR(dst[0], 0.6f, 1e-5);
    CHECK_NEAR(dst[N * N / 2 + N / 2], 0.6f, 1e-5);
    CHECK_NEAR(dst[N * N - 1], 0.6f, 1e-5);

    // A single bright pixel in the middle is spread out without any of it
    // being lost
    memset(src, 0, N * N * sizeof(float));
    src[(N / 2) * N + N / 2] = 1.0f;
    lowPass(src, dst, scratch, colSums, N, N, 2, 2);
    double sum = 0.0;
    for (int i = 0; i < N * N; i++) {
        sum += dst[i];
    }
    CHECK_NEAR(sum, 1.0, 1e-4);
    CHECK(dst[(N / 2) * N + N / 2] < 1.0f);
    CHECK(dst[(N / 2) * N + N / 2 + 1] > 0.0f);

    free(src);
    free(dst);
    free(scratch);
    free(colSums);
}

// The same three box passes by hand, clamped at the edges, one radius per axis
static void naiveBox(const float* src, float* dst, int w, int h, int r, int across) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sum = 0.0f;
            for (int i = -r; i <= r; i++) {
                int sx = across ? x + i : x;
                int sy = across ? y : y + i;
                sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
                sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
                sum += src[sy * w + sx];
            }
            dst[y * w + x] = sum / (float)(2 * r + 1);
        }
    }
}

// 16:9 like the ZipDepth map, with its own radius on each axis, against the
// passes done the slow way
static void testLowPassRect(void) {
    const int w = 48, h = 27, rx = 4, ry = 2;
    float* src = malloc((size_t)w * h * sizeof(float));
    float* dst = malloc((size_t)w * h * sizeof(float));
    float* scratch = malloc((size_t)w * h * sizeof(float));
    float* colSums = malloc((size_t)w * sizeof(float));
    float* a = malloc((size_t)w * h * sizeof(float));
    float* b = malloc((size_t)w * h * sizeof(float));
    for (int i = 0; i < w * h; i++) {
        src[i] = (float)((i * 7919) % 101) / 100.0f;
    }
    lowPass(src, dst, scratch, colSums, w, h, rx, ry);

    memcpy(a, src, (size_t)w * h * sizeof(float));
    for (int pass = 0; pass < 3; pass++) {
        naiveBox(a, b, w, h, rx, 1);
        naiveBox(b, a, w, h, ry, 0);
    }
    float worst = 0.0f;
    for (int i = 0; i < w * h; i++) {
        float d = fabsf(dst[i] - a[i]);
        worst = d > worst ? d : worst;
    }
    CHECK(worst < 1e-4f);

    free(src);
    free(dst);
    free(scratch);
    free(colSums);
    free(a);
    free(b);
}

// The map sizes nativeInit takes from Java
static void testDepthSize(void) {
    CHECK(depthSizeOk(512, 288));
    CHECK(depthSizeOk(256, 256));
    CHECK(depthSizeOk(64, 64));
    CHECK(!depthSizeOk(640, 352));
    CHECK(!depthSizeOk(384, 384));
    CHECK(!depthSizeOk(512, 60));
    CHECK(!depthSizeOk(516, 288));
    CHECK(!depthSizeOk(512, 284));
    CHECK(!depthSizeOk(0, 0));
    CHECK(!depthSizeOk(-256, 256));
}

// A NaN or an infinity out of the model counts for nothing in the range, and
// is read as lo, so none of it reaches the smoothing where it would stay
static void testNonFinite(void) {
    float* v = malloc(N * N * sizeof(float));
    for (int i = 0; i < N * N; i++) {
        v[i] = i / (float)(N * N - 1);
    }
    float cleanLo, cleanHi;
    robustRange(v, N * N, &cleanLo, &cleanHi);

    v[0] = NAN;
    v[N * N / 2] = NAN;
    v[1] = INFINITY;
    v[N * N - 1] = -INFINITY;
    float lo, hi;
    robustRange(v, N * N, &lo, &hi);
    CHECK(isfinite(lo) && isfinite(hi));
    CHECK_NEAR(lo, cleanLo, 0.01);
    CHECK_NEAR(hi, cleanHi, 0.01);
    CHECK(depthRead(NAN, lo) == lo);
    CHECK(depthRead(INFINITY, lo) == lo);
    CHECK(depthRead(-INFINITY, lo) == lo);
    CHECK(depthRead(0.5f, lo) == 0.5f);

    // Nothing finite at all still gives a range to divide by
    for (int i = 0; i < N * N; i++) {
        v[i] = NAN;
    }
    robustRange(v, N * N, &lo, &hi);
    CHECK(isfinite(lo) && isfinite(hi) && hi > lo);
    free(v);
}

// A range narrower than a float step can hold at values in the hundreds. A
// floor of 1e-6 added to 500 is 500 again, so the span would come back as
// nothing and one over it would make the whole map NaN.
static void testNarrowSpan(void) {
    float* v = malloc(N * N * sizeof(float));
    // All but a few at 500, the rest a hair above, so both percentiles land
    // in the first bin
    for (int i = 0; i < N * N; i++) {
        v[i] = i % 100 == 0 ? 500.001f : 500.0f;
    }
    float lo, hi;
    robustRange(v, N * N, &lo, &hi);
    CHECK(hi - lo > 0.0f);
    CHECK(isfinite(1.0f / (hi - lo)));
    CHECK(hi - lo >= depthSpanFloor(lo));

    // And a flat map there
    for (int i = 0; i < N * N; i++) {
        v[i] = 500.0f;
    }
    robustRange(v, N * N, &lo, &hi);
    CHECK(hi - lo > 0.0f);

    // The smoothed range divides by a floored span too, whether its two ends
    // have met or crossed
    CHECK(isfinite(depthSpanScale(500.0f, 500.0f)) && depthSpanScale(500.0f, 500.0f) > 0.0f);
    CHECK(isfinite(depthSpanScale(500.0f, nextafterf(500.0f, 1000.0f))));
    CHECK(isfinite(depthSpanScale(2.0f, 1.0f)) && depthSpanScale(2.0f, 1.0f) > 0.0f);
    // An ordinary span is left alone
    CHECK_NEAR(depthSpanScale(1.0f, 3.0f), 0.5f, 1e-6);

    // Near zero the floor is the one it always was
    CHECK_NEAR(depthSpanFloor(0.0f), 1e-6, 1e-12);
    CHECK_NEAR(depthSpanFloor(0.05f), 1e-6, 1e-12);
    free(v);
}

// The range of a 512x288 ramp, the size ZipDepth runs at, comes out the same
// as a square one's
static void testRangeRect(void) {
    const int w = 512, h = 288;
    float* v = malloc((size_t)w * h * sizeof(float));
    for (int i = 0; i < w * h; i++) {
        v[i] = i / (float)(w * h - 1);
    }
    float lo, hi;
    robustRange(v, w * h, &lo, &hi);
    CHECK_NEAR(lo, 0.02f, 0.01);
    CHECK_NEAR(hi, 0.98f, 0.01);
    free(v);
}

int main(void) {
    testRobustRange();
    testLowPass();
    testLowPassRect();
    testDepthSize();
    testNonFinite();
    testNarrowSpan();
    testRangeRect();
    return checksDone("xr_depthmap");
}
