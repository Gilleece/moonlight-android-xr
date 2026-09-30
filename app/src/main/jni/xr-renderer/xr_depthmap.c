#include <math.h>
#include <string.h>

#include "xr_depthmap.h"
#include "xr_shared.h"

float depthSpanFloor(float lo) {
    // About eighty float steps at lo
    float floor = fabsf(lo) * 1e-5f;
    return floor > 1e-6f ? floor : 1e-6f;
}

float depthSpanScale(float lo, float hi) {
    float span = hi - lo;
    float least = depthSpanFloor(lo);
    return 1.0f / (span >= least ? span : least);
}

// 2nd and 98th percentile of the model output, via a histogram. Using the
// raw min and max lets one stray pixel own the whole mapping: on a measured
// frame the 2..98 span was 638 of an 805 wide min/max range, so a fifth of
// the output range was being spent on a handful of pixels.
void robustRange(const float* v, int count, float* outLo, float* outHi) {
    float lo = INFINITY, hi = -INFINITY;
    int finite = 0;
    for (int i = 0; i < count; i++) {
        float x = v[i];
        if (!isfinite(x)) {
            continue;
        }
        if (x < lo) lo = x;
        if (x > hi) hi = x;
        finite++;
    }
    if (finite == 0) {
        *outLo = 0.0f;
        *outHi = 1.0f;
        return;
    }
    if (hi <= lo) {
        *outLo = lo;
        *outHi = lo + fmaxf(1.0f, depthSpanFloor(lo));
        return;
    }

    int hist[DEPTH_HIST_BINS];
    memset(hist, 0, sizeof(hist));
    float scale = DEPTH_HIST_BINS / (hi - lo);
    for (int i = 0; i < count; i++) {
        if (!isfinite(v[i])) {
            continue;
        }
        int b = (int)((v[i] - lo) * scale);
        if (b < 0) b = 0;
        if (b >= DEPTH_HIST_BINS) b = DEPTH_HIST_BINS - 1;
        hist[b]++;
    }

    int loTarget = (int)(finite * 0.02f);
    int hiTarget = (int)(finite * 0.98f);
    int acc = 0, loBin = 0, hiBin = DEPTH_HIST_BINS - 1;
    for (int b = 0; b < DEPTH_HIST_BINS; b++) {
        acc += hist[b];
        if (acc >= loTarget) {
            loBin = b;
            break;
        }
    }
    acc = 0;
    for (int b = 0; b < DEPTH_HIST_BINS; b++) {
        acc += hist[b];
        if (acc >= hiTarget) {
            hiBin = b;
            break;
        }
    }

    *outLo = lo + loBin / scale;
    *outHi = lo + (hiBin + 1) / scale;
    // Written so a NaN span takes the floor too
    if (!(*outHi - *outLo >= depthSpanFloor(*outLo))) {
        *outHi = *outLo + depthSpanFloor(*outLo);
    }
}

static void boxBlurH(const float* src, float* dst, int w, int h, int r) {
    float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < h; y++) {
        const float* s = src + (size_t)y * w;
        float* d = dst + (size_t)y * w;
        float sum = 0.0f;
        for (int i = -r; i <= r; i++) {
            int x = i < 0 ? 0 : (i >= w ? w - 1 : i);
            sum += s[x];
        }
        for (int x = 0; x < w; x++) {
            d[x] = sum * inv;
            int add = x + r + 1;
            int sub = x - r;
            sum += s[add >= w ? w - 1 : add] - s[sub < 0 ? 0 : sub];
        }
    }
}

// Column sums carried a row at a time. The obvious version, one column at a
// time, strides a whole row between reads and misses cache on every access,
// which cost 15 ms here rather than 1.
static void boxBlurV(const float* src, float* dst, int w, int h, int r, float* colSums) {
    float inv = 1.0f / (float)(2 * r + 1);
    memset(colSums, 0, (size_t)w * sizeof(float));
    for (int i = -r; i <= r; i++) {
        int y = i < 0 ? 0 : (i >= h ? h - 1 : i);
        const float* s = src + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            colSums[x] += s[x];
        }
    }
    for (int y = 0; y < h; y++) {
        float* d = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            d[x] = colSums[x] * inv;
        }
        int add = y + r + 1;
        int sub = y - r;
        const float* a = src + (size_t)(add >= h ? h - 1 : add) * w;
        const float* b = src + (size_t)(sub < 0 ? 0 : sub) * w;
        for (int x = 0; x < w; x++) {
            colSums[x] += a[x] - b[x];
        }
    }
}

// Three box passes is close enough to a gaussian
void lowPass(const float* src, float* dst, float* scratch, float* colSums, int w, int h,
             int rx, int ry) {
    boxBlurH(src, scratch, w, h, rx);
    boxBlurV(scratch, dst, w, h, ry, colSums);
    boxBlurH(dst, scratch, w, h, rx);
    boxBlurV(scratch, dst, w, h, ry, colSums);
    boxBlurH(dst, scratch, w, h, rx);
    boxBlurV(scratch, dst, w, h, ry, colSums);
}

int depthSizeOk(int w, int h) {
    return w >= 64 && h >= 64 && w <= DEPTH_TEX_W_MAX && h <= DEPTH_TEX_H_MAX
            && w % 8 == 0 && h % 8 == 0;
}
