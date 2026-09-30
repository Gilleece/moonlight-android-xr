// The CPU side of the depth map: the percentile range, the low pass, the map
// size check, the averages over time and the scene cut detector
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

static void fillRgb(float* rgb, int w, int h, float left, float right) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float v = x < w / 2 ? left : right;
            for (int c = 0; c < 3; c++) {
                rgb[((size_t)y * w + x) * 3 + c] = v;
            }
        }
    }
}

static void testThumb(void) {
    // 72 is not a multiple of 16, so the cells are 4 and 5 wide
    const int n = 72;
    float* rgb = malloc((size_t)n * n * 3 * sizeof(float));
    DepthThumb a, b;

    fillRgb(rgb, n, n, 0.5f, 0.5f);
    depthThumbMake(&a, rgb, n, n);
    CHECK_NEAR(a.grid[0], 0.5f, 1e-5);
    CHECK_NEAR(a.grid[DEPTH_CUT_GRID * DEPTH_CUT_GRID - 1], 0.5f, 1e-5);
    CHECK_NEAR(a.hist[DEPTH_CUT_BINS / 2], 1.0f, 1e-6);
    CHECK_NEAR(depthThumbDiff(&a, &a), 0.0f, 1e-9);
    CHECK_NEAR(depthThumbHistDiff(&a, &a), 0.0f, 1e-9);

    // Black on the left, white on the right: the grid splits down the middle
    // and the histogram holds half in each end bin
    fillRgb(rgb, n, n, 0.0f, 1.0f);
    depthThumbMake(&b, rgb, n, n);
    CHECK_NEAR(b.grid[0], 0.0f, 1e-6);
    CHECK_NEAR(b.grid[DEPTH_CUT_GRID - 1], 1.0f, 1e-5);
    CHECK_NEAR(b.hist[0], 0.5f, 0.05);
    CHECK_NEAR(b.hist[DEPTH_CUT_BINS - 1], 0.5f, 0.05);
    float sum = 0.0f;
    for (int i = 0; i < DEPTH_CUT_GRID * DEPTH_CUT_GRID; i++) {
        sum += b.grid[i];
    }
    CHECK_NEAR(sum / (DEPTH_CUT_GRID * DEPTH_CUT_GRID), 0.5f, 1e-5);
    // Every cell is 0.5 from the flat grey, and no bin is shared with it
    CHECK_NEAR(depthThumbDiff(&a, &b), 0.5f, 1e-5);
    CHECK_NEAR(depthThumbHistDiff(&a, &b), 1.0f, 1e-5);
    free(rgb);
}

// The grid covers a 16:9 input the same way: its cells split the width and
// the height each into 16, so the same picture reads the same whatever the
// map's shape
static void testThumbRect(void) {
    const int w = 512, h = 288;
    float* rgb = malloc((size_t)w * h * 3 * sizeof(float));
    DepthThumb t, sq;
    fillRgb(rgb, w, h, 0.0f, 1.0f);
    depthThumbMake(&t, rgb, w, h);
    for (int by = 0; by < DEPTH_CUT_GRID; by++) {
        CHECK_NEAR(t.grid[by * DEPTH_CUT_GRID + 0], 0.0f, 1e-6);
        CHECK_NEAR(t.grid[by * DEPTH_CUT_GRID + DEPTH_CUT_GRID / 2 - 1], 0.0f, 1e-6);
        CHECK_NEAR(t.grid[by * DEPTH_CUT_GRID + DEPTH_CUT_GRID / 2], 1.0f, 1e-5);
        CHECK_NEAR(t.grid[by * DEPTH_CUT_GRID + DEPTH_CUT_GRID - 1], 1.0f, 1e-5);
    }
    CHECK_NEAR(t.hist[0], 0.5f, 0.05);
    CHECK_NEAR(t.hist[DEPTH_CUT_BINS - 1], 0.5f, 0.05);

    // The same picture at the 256 square the other routes run at thumbs the
    // same
    const int n = 256;
    float* sqRgb = malloc((size_t)n * n * 3 * sizeof(float));
    fillRgb(sqRgb, n, n, 0.0f, 1.0f);
    depthThumbMake(&sq, sqRgb, n, n);
    CHECK_NEAR(depthThumbDiff(&t, &sq), 0.0f, 1e-5);

    // Too small on either side for the grid is an empty thumb
    depthThumbMake(&t, rgb, DEPTH_CUT_GRID - 1, h);
    CHECK_NEAR(t.grid[0], 0.0f, 1e-9);
    CHECK_NEAR(t.hist[0], 0.0f, 1e-9);
    free(rgb);
    free(sqRgb);
}

// A grid of one flat level over a histogram that never changes, so a step is
// read on the grid alone
static DepthThumb flatThumb(float level) {
    DepthThumb t;
    for (int i = 0; i < DEPTH_CUT_GRID * DEPTH_CUT_GRID; i++) {
        t.grid[i] = level;
    }
    for (int i = 0; i < DEPTH_CUT_BINS; i++) {
        t.hist[i] = 1.0f / DEPTH_CUT_BINS;
    }
    return t;
}

// Runs a sequence of flat levels through a fresh detector, the flags per step
static void cutRun(const float* levels, int count, int* flags) {
    DepthCut c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < count; i++) {
        DepthThumb t = flatThumb(levels[i]);
        float d, h;
        flags[i] = depthCutStep(&c, &t, &d, &h);
    }
}

static void testCut(void) {
    int f[8];

    // Under the jump threshold nothing happens
    const float drift[] = { 0.50f, 0.50f, 0.50f + DEPTH_CUT_JUMP_DIFF - 0.01f,
                            0.50f + DEPTH_CUT_JUMP_DIFF - 0.01f };
    cutRun(drift, 4, f);
    CHECK(f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0);

    // A cut: settled, one jump, and the next capture stays with it. The jump
    // starts the per texel average again, the confirmation the range, and
    // what it reports is the jump's difference.
    const float cut[] = { 0.2f, 0.2f, 0.7f, 0.7f, 0.7f };
    cutRun(cut, 5, f);
    CHECK(f[0] == 0 && f[1] == 0);
    CHECK(f[2] == DEPTH_CUT_JUMP);
    CHECK(f[3] == DEPTH_CUT_CONFIRMED);
    CHECK(f[4] == 0);
    DepthCut c;
    memset(&c, 0, sizeof(c));
    float d, h;
    DepthThumb t = flatThumb(0.2f);
    depthCutStep(&c, &t, &d, &h);
    t = flatThumb(0.7f);
    depthCutStep(&c, &t, &d, &h);
    t = flatThumb(0.72f);
    CHECK(depthCutStep(&c, &t, &d, &h) == DEPTH_CUT_CONFIRMED);
    CHECK_NEAR(d, 0.5f, 1e-5);
    CHECK_NEAR(h, 0.0f, 1e-6);

    // Just over the jump threshold on the grid alone is a jump
    const float edge[] = { 0.50f, 0.50f, 0.50f + DEPTH_CUT_JUMP_DIFF + 0.005f,
                           0.50f + DEPTH_CUT_JUMP_DIFF + 0.005f };
    cutRun(edge, 4, f);
    CHECK(f[2] == DEPTH_CUT_JUMP);
    CHECK(f[3] == DEPTH_CUT_CONFIRMED);

    // A flash: a jump there and a jump back, and the range is left alone
    const float flash[] = { 0.3f, 0.3f, 0.95f, 0.3f, 0.3f };
    cutRun(flash, 5, f);
    CHECK(f[2] == DEPTH_CUT_JUMP);
    CHECK(f[3] == DEPTH_CUT_JUMP);
    CHECK(f[4] == 0);

    // A fast pan: every step a jump, none confirmed, and none after it
    // settles either, since the last step of it did not come out of a settled
    // picture
    const float pan[] = { 0.1f, 0.1f, 0.25f, 0.4f, 0.55f, 0.7f, 0.7f, 0.7f };
    cutRun(pan, 8, f);
    for (int i = 2; i < 6; i++) {
        CHECK(f[i] == DEPTH_CUT_JUMP);
    }
    CHECK(f[6] == 0 && f[7] == 0);

    // The capture after the jump moving on by more than the same threshold
    // is not a confirmation, and by less is
    const float moving[] = { 0.2f, 0.2f, 0.6f, 0.6f + DEPTH_CUT_SAME_DIFF + 0.01f };
    cutRun(moving, 4, f);
    CHECK(f[2] == DEPTH_CUT_JUMP);
    CHECK(!(f[3] & DEPTH_CUT_CONFIRMED));
    const float steady[] = { 0.2f, 0.2f, 0.6f, 0.6f + DEPTH_CUT_SAME_DIFF - 0.01f };
    cutRun(steady, 4, f);
    CHECK(f[3] == DEPTH_CUT_CONFIRMED);

    // The histogram on its own: the same grid, the whole of the luma moved to
    // another bin, is a jump, and one that holds is a cut
    memset(&c, 0, sizeof(c));
    DepthThumb x = flatThumb(0.5f);
    DepthThumb y = x;
    memset(x.hist, 0, sizeof(x.hist));
    x.hist[8] = 1.0f;
    memset(y.hist, 0, sizeof(y.hist));
    y.hist[2] = 1.0f;
    CHECK(depthCutStep(&c, &x, &d, &h) == 0);
    CHECK(depthCutStep(&c, &x, &d, &h) == 0);
    CHECK(depthCutStep(&c, &y, &d, &h) == DEPTH_CUT_JUMP);
    CHECK_NEAR(d, 0.0f, 1e-9);
    CHECK(h > DEPTH_CUT_JUMP_HIST);
    CHECK(depthCutStep(&c, &y, &d, &h) == DEPTH_CUT_CONFIRMED);

    // A jump that does not stand out from the step before it is motion, even
    // against the way that step went
    const float drifting[] = { 0.35f, 0.30f, 0.39f, 0.39f };
    cutRun(drifting, 4, f);
    CHECK(f[2] == DEPTH_CUT_JUMP);
    CHECK(f[3] == 0);
    // One that does, but lands further still from the capture two back,
    // carries the motion on; the same jump against motion the other way is
    // a cut
    const float onward[] = { 0.30f, 0.34f, 0.50f, 0.50f };
    cutRun(onward, 4, f);
    CHECK(f[2] == DEPTH_CUT_JUMP);
    CHECK(f[3] == 0);
    const float against[] = { 0.34f, 0.30f, 0.50f, 0.50f };
    cutRun(against, 4, f);
    CHECK(f[3] == DEPTH_CUT_CONFIRMED);

    // Cleared, the next capture only seeds it
    memset(&c, 0, sizeof(c));
    t = flatThumb(0.2f);
    depthCutStep(&c, &t, &d, &h);
    depthCutClear(&c);
    t = flatThumb(0.9f);
    CHECK(depthCutStep(&c, &t, &d, &h) == 0);
}

// A grid ramping left to right, or top to bottom, over a histogram that
// never changes
static DepthThumb rampThumb(float lo, float span, int down) {
    DepthThumb t = flatThumb(0.0f);
    for (int y = 0; y < DEPTH_CUT_GRID; y++) {
        for (int x = 0; x < DEPTH_CUT_GRID; x++) {
            float at = (float)(down ? y : x) / (DEPTH_CUT_GRID - 1);
            t.grid[y * DEPTH_CUT_GRID + x] = lo + span * at;
        }
    }
    return t;
}

// The layout the correlation sees: a brighter copy of a picture is the same
// picture, a new layout is a cut
static void testCutLayout(void) {
    DepthThumb ramp = rampThumb(0.2f, 0.4f, 0);
    DepthThumb flat = flatThumb(0.5f);
    CHECK_NEAR(depthThumbCorr(&ramp, &ramp), 1.0f, 1e-5);
    CHECK_NEAR(depthThumbCorr(&ramp, &flat), 0.0f, 1e-9);
    DepthThumb brighter = rampThumb(0.4f, 0.6f, 0);
    CHECK_NEAR(depthThumbCorr(&ramp, &brighter), 1.0f, 1e-5);
    DepthThumb flipped = rampThumb(0.6f, -0.4f, 0);
    CHECK_NEAR(depthThumbCorr(&ramp, &flipped), -1.0f, 1e-5);
    DepthThumb down = rampThumb(0.2f, 0.4f, 1);
    CHECK_NEAR(depthThumbCorr(&ramp, &down), 0.0f, 1e-5);

    // An exposure change jumps and holds, and is not a cut
    DepthCut c;
    memset(&c, 0, sizeof(c));
    float d, h;
    CHECK(depthCutStep(&c, &ramp, &d, &h) == 0);
    CHECK(depthCutStep(&c, &ramp, &d, &h) == 0);
    CHECK(depthCutStep(&c, &brighter, &d, &h) == DEPTH_CUT_JUMP);
    CHECK(depthCutStep(&c, &brighter, &d, &h) == 0);

    // The same ramp turned on its side is
    memset(&c, 0, sizeof(c));
    depthCutStep(&c, &ramp, &d, &h);
    depthCutStep(&c, &ramp, &d, &h);
    CHECK(depthCutStep(&c, &down, &d, &h) == DEPTH_CUT_JUMP);
    CHECK(depthCutStep(&c, &down, &d, &h) == DEPTH_CUT_CONFIRMED);
    CHECK_NEAR(c.corr, 0.0f, 1e-5);
}

// Two made up scenes, each a world wider than the frame so it can be panned
// across: soft shapes, a diagonal band and some fine texture, in pixels
static float sceneLuma(int scene, float u, float v) {
    const float tau = 6.2831853f;
    if (scene == 0) {
        return 0.5f + 0.18f * sinf(tau * u / 700.0f + 0.4f) * cosf(tau * v / 400.0f)
                + 0.12f * sinf(tau * (0.8f * u + 0.6f * v) / 150.0f)
                + 0.06f * sinf(tau * u / 37.0f) * sinf(tau * v / 29.0f);
    }
    return 0.42f + 0.2f * cosf(tau * v / 250.0f + 1.1f) * sinf(tau * u / 900.0f + 2.0f)
            + 0.14f * sinf(tau * (0.3f * u - 0.95f * v) / 120.0f + 0.7f)
            + 0.06f * cosf(tau * u / 23.0f + v / 17.0f);
}

// The model input a capture of the scene gives, panned pan pixels across
static void renderScene(float* rgb, int w, int h, int scene, float pan) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float l = sceneLuma(scene, x + pan, (float)y);
            float* p = rgb + ((size_t)y * w + x) * 3;
            p[0] = fminf(fmaxf(1.05f * l, 0.0f), 1.0f);
            p[1] = fminf(fmaxf(l, 0.0f), 1.0f);
            p[2] = fminf(fmaxf(0.9f * l, 0.0f), 1.0f);
        }
    }
}

// Pictures through the whole detector, at both map sizes: a steady pan
// neither jumps nor cuts, so nothing is reset, and a cut to an unrelated
// scene resets the per texel average on its first capture and the range on
// the next
static void testCutPictures(void) {
    const int sizes[2][2] = { { 512, 288 }, { 256, 256 } };
    for (int s = 0; s < 2; s++) {
        const int w = sizes[s][0], h = sizes[s][1];
        float* rgb = malloc((size_t)w * h * 3 * sizeof(float));
        DepthCut c;
        memset(&c, 0, sizeof(c));
        DepthThumb t;
        float d, hd;

        // Three pixels a capture, about a map width every four seconds at 45
        // captures a second
        int flags = 0;
        for (int i = 0; i < 12; i++) {
            renderScene(rgb, w, h, 0, 3.0f * i);
            depthThumbMake(&t, rgb, w, h);
            flags |= depthCutStep(&c, &t, &d, &hd);
        }
        CHECK(flags == 0);

        // The cut, into a scene that goes on panning
        int cut[4];
        for (int i = 0; i < 4; i++) {
            renderScene(rgb, w, h, 1, 3.0f * i);
            depthThumbMake(&t, rgb, w, h);
            cut[i] = depthCutStep(&c, &t, &d, &hd);
        }
        CHECK(cut[0] == DEPTH_CUT_JUMP);
        CHECK(cut[1] == DEPTH_CUT_CONFIRMED);
        CHECK(c.corr < DEPTH_CUT_CORR_MAX);
        CHECK(cut[2] == 0 && cut[3] == 0);
        free(rgb);
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
    testThumb();
    testThumbRect();
    testCut();
    testCutLayout();
    testCutPictures();
    return checksDone("xr_depthmap");
}
