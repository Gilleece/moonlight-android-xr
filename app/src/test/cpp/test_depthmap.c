// The CPU side of the depth map: the percentile range, the low pass, the map
// size check and the averages over time
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

    // The smoothed range divides by a floored span too, from a flat frame and
    // from one that eases toward it
    DepthRange s = { 0.0f, 0.0f, 0 };
    depthRangeStep(&s, 500.0f, 500.0f, 0.0f, 0.15f);
    CHECK(isfinite(depthRangeScale(&s)) && depthRangeScale(&s) > 0.0f);
    depthRangeStep(&s, 500.0f, nextafterf(500.0f, 1000.0f), 0.05f, 0.15f);
    CHECK(isfinite(depthRangeScale(&s)) && depthRangeScale(&s) > 0.0f);
    // and with its two ends crossed
    DepthRange crossed = { 2.0f, 1.0f, 1 };
    CHECK(isfinite(depthRangeScale(&crossed)) && depthRangeScale(&crossed) > 0.0f);
    // An ordinary span is left alone
    DepthRange plain = { 1.0f, 3.0f, 1 };
    CHECK_NEAR(depthRangeScale(&plain), 0.5f, 1e-6);

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

// The range the map is normalised against, a one pole over real time
static void testDepthRange(void) {
    DepthRange r = { 0.0f, 0.0f, 0 };

    // The first range is taken as it is
    depthRangeStep(&r, 1.0f, 2.0f, 0.02f, 0.15f);
    CHECK(r.valid);
    CHECK_NEAR(r.lo, 1.0f, 1e-6);
    CHECK_NEAR(r.hi, 2.0f, 1e-6);

    // One time constant covers 63 percent of a move, six maps 25 ms apart
    for (int i = 0; i < 6; i++) {
        depthRangeStep(&r, 1.0f, 2.3f, 0.025f, 0.15f);
    }
    CHECK_NEAR(r.hi, 2.0f + 0.3f * (1.0f - expf(-1.0f)), 1e-4);
    CHECK_NEAR(r.lo, 1.0f, 1e-6);

    // And the same at half the rate, so the settling time does not follow
    // the model's
    DepthRange slow = { 1.0f, 2.0f, 1 };
    for (int i = 0; i < 3; i++) {
        depthRangeStep(&slow, 1.0f, 2.3f, 0.05f, 0.15f);
    }
    CHECK_NEAR(slow.hi, r.hi, 1e-4);

    // It gets there in the end
    for (int i = 0; i < 200; i++) {
        depthRangeStep(&r, 1.0f, 2.3f, 0.025f, 0.15f);
    }
    CHECK_NEAR(r.hi, 2.3f, 1e-4);

    // A pause counts for no more than a second
    DepthRange paused = { 1.0f, 2.0f, 1 };
    DepthRange second = { 1.0f, 2.0f, 1 };
    depthRangeStep(&paused, 1.0f, 3.0f, 30.0f, 2.0f);
    depthRangeStep(&second, 1.0f, 3.0f, 1.0f, 2.0f);
    CHECK_NEAR(paused.hi, second.hi, 1e-6);
    CHECK(paused.hi < 3.0f);

    // A tau of 0 takes every map's range as it is
    depthRangeStep(&r, 5.0f, 7.0f, 0.025f, 0.0f);
    CHECK_NEAR(r.lo, 5.0f, 1e-6);
    CHECK_NEAR(r.hi, 7.0f, 1e-6);

    // And so does the first map after a reset
    r.valid = 0;
    depthRangeStep(&r, 3.0f, 4.0f, 0.025f, 0.15f);
    CHECK_NEAR(r.lo, 3.0f, 1e-6);
    CHECK_NEAR(r.hi, 4.0f, 1e-6);

    CHECK_NEAR(depthRangeScale(&r), 1.0f, 1e-6);
}

// The per texel average: one pole over real time with the time clamped, and
// nothing that is not finite kept in it
static void testDepthTau(void) {
    // One time constant covers about 63 percent of a step
    CHECK_NEAR(depthTauAlpha(0.1f, 0.1f), 0.632f, 1e-3);
    CHECK_NEAR(depthTauAlpha(0.05f, 0.2f), 1.0f - expf(-0.25f), 1e-6);
    // Held to 1/60 s below and 1 s above
    CHECK_NEAR(depthTauAlpha(0.0f, 0.1f), 1.0f - expf(-(1.0f / 60.0f) / 0.1f), 1e-6);
    CHECK_NEAR(depthTauAlpha(5.0f, 2.0f), 1.0f - expf(-0.5f), 1e-6);
    // No time constant is no averaging
    CHECK_NEAR(depthTauAlpha(0.05f, 0.0f), 1.0f, 1e-9);

    float avg[4];
    float a[4] = { 1.0f, 2.0f, NAN, 4.0f };
    float b[4] = { 3.0f, INFINITY, 5.0f, 4.0f };
    depthTauBlend(avg, a, 4, 0.25f, 1);
    CHECK_NEAR(avg[0], 1.0f, 1e-6);
    CHECK(isnan(avg[2]));
    depthTauBlend(avg, b, 4, 0.25f, 0);
    CHECK_NEAR(avg[0], 1.5f, 1e-6);
    // A value that is not finite leaves the texel alone
    CHECK_NEAR(avg[1], 2.0f, 1e-6);
    // and a texel that is not finite takes the next one that is
    CHECK_NEAR(avg[2], 5.0f, 1e-6);
    CHECK_NEAR(avg[3], 4.0f, 1e-6);
}

// A step in the raw map, and one in the range, settle on the same clock at
// the rates the model runs at: 1 - exp(-t / tau) of the way after t seconds
// of maps, so within 1 percent after 4.6 time constants, give or take a map.
// Past 60 maps a second each map counts as 1/60 s, which runs the average a
// little ahead of the clock rather than behind it.
static void testSettleRates(void) {
    const float rates[] = { 24.0f, 45.0f, 66.0f };
    const float tauTexel = DEPTH_TAU_DEFAULT_MS / 1000.0f;
    const float tauRange = DEPTH_RANGE_TAU_DEFAULT_MS / 1000.0f;
    const float settled = -logf(0.01f);
    for (int r = 0; r < 3; r++) {
        const float dt = 1.0f / rates[r];
        const float counted = dt > DEPTH_TAU_DT_MIN_S ? dt : DEPTH_TAU_DT_MIN_S;
        // The clamp costs at most a tenth of the time constant at 66
        CHECK(dt / counted > 0.9f);

        // The per texel average, a step from 0 to 1 in every texel
        float avg[8];
        const float zero[8] = { 0 };
        const float one[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
        depthTauBlend(avg, zero, 8, 1.0f, 1);
        float worst = 0.0f, doneAt = -1.0f;
        for (int k = 1; k <= 60; k++) {
            depthTauBlend(avg, one, 8, depthTauAlpha(dt, tauTexel), 0);
            float expect = 1.0f - expf(-k * counted / tauTexel);
            for (int i = 0; i < 8; i++) {
                worst = fmaxf(worst, fabsf(avg[i] - expect));
            }
            if (doneAt < 0.0f && avg[0] > 0.99f) {
                doneAt = k * dt;
            }
        }
        CHECK(worst < 1e-5f);
        CHECK_NEAR(doneAt, settled * tauTexel * dt / counted, dt);

        // The range, its far end stepping from 2 to 3
        DepthRange range = { 1.0f, 2.0f, 1 };
        worst = 0.0f;
        doneAt = -1.0f;
        for (int k = 1; k <= 200; k++) {
            depthRangeStep(&range, 1.0f, 3.0f, dt, tauRange);
            float expect = 3.0f - expf(-k * counted / tauRange);
            worst = fmaxf(worst, fabsf(range.hi - expect));
            if (doneAt < 0.0f && range.hi > 2.99f) {
                doneAt = k * dt;
            }
        }
        CHECK(worst < 1e-4f);
        CHECK_NEAR(doneAt, settled * tauRange * dt / counted, dt);
        CHECK_NEAR(range.lo, 1.0f, 1e-6);
    }
}

int main(void) {
    testRobustRange();
    testLowPass();
    testLowPassRect();
    testDepthSize();
    testNonFinite();
    testNarrowSpan();
    testRangeRect();
    testDepthRange();
    testDepthTau();
    testSettleRates();
    return checksDone("xr_depthmap");
}
