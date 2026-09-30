#include <math.h>
#include <string.h>

#include "xr_depthmap.h"
#include "xr_shared.h"

float depthSpanFloor(float lo) {
    // About eighty float steps at lo
    float floor = fabsf(lo) * 1e-5f;
    return floor > 1e-6f ? floor : 1e-6f;
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

float depthTauAlpha(float dtSec, float tauSec) {
    if (!(tauSec > 0.0f)) {
        return 1.0f;
    }
    float dt = dtSec < DEPTH_TAU_DT_MIN_S ? DEPTH_TAU_DT_MIN_S
             : dtSec > DEPTH_TAU_DT_MAX_S ? DEPTH_TAU_DT_MAX_S : dtSec;
    return 1.0f - expf(-dt / tauSec);
}

void depthTauBlend(float* avg, const float* v, int count, float alpha, int seed) {
    for (int i = 0; i < count; i++) {
        float x = v[i];
        if (seed) {
            avg[i] = x;
        }
        else if (isfinite(x)) {
            float a = avg[i];
            avg[i] = isfinite(a) ? a + alpha * (x - a) : x;
        }
    }
}

void depthRangeStep(DepthRange* r, float lo, float hi, float dtSec, float tauSec) {
    if (!r->valid || !(tauSec > 0.0f)) {
        r->lo = lo;
        r->hi = hi;
        r->valid = 1;
        return;
    }
    float a = depthTauAlpha(dtSec, tauSec);
    r->lo += a * (lo - r->lo);
    r->hi += a * (hi - r->hi);
}

float depthRangeScale(const DepthRange* r) {
    float span = r->hi - r->lo;
    float least = depthSpanFloor(r->lo);
    return 1.0f / (span >= least ? span : least);
}

// Cell edges come from h * i / 16 and w * i / 16, so a side 16 does not divide
// still has every pixel in a cell. Each band of rows is summed a column at a
// time first, which vectorises, and the luma weights go on once per cell. The
// histogram samples a lattice of about 64 by 64 pixels, plenty for 16 bins,
// into four sets of counts in turn, so a run of pixels in one bin is not a
// chain of increments each waiting on the one before.
void depthThumbMake(DepthThumb* t, const float* rgb, int w, int h) {
    if (w < DEPTH_CUT_GRID || h < DEPTH_CUT_GRID) {
        memset(t, 0, sizeof(*t));
        return;
    }
    float cols[3 * w];
    for (int by = 0; by < DEPTH_CUT_GRID; by++) {
        int y0 = by * h / DEPTH_CUT_GRID;
        int y1 = (by + 1) * h / DEPTH_CUT_GRID;
        memset(cols, 0, sizeof(cols));
        for (int y = y0; y < y1; y++) {
            const float* row = rgb + (size_t)y * w * 3;
            for (int i = 0; i < 3 * w; i++) {
                cols[i] += row[i];
            }
        }
        for (int bx = 0; bx < DEPTH_CUT_GRID; bx++) {
            int x0 = bx * w / DEPTH_CUT_GRID;
            int x1 = (bx + 1) * w / DEPTH_CUT_GRID;
            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (int x = x0; x < x1; x++) {
                r += cols[x * 3 + 0];
                g += cols[x * 3 + 1];
                b += cols[x * 3 + 2];
            }
            int count = (y1 - y0) * (x1 - x0);
            t->grid[by * DEPTH_CUT_GRID + bx] = count > 0
                    ? (0.299f * r + 0.587f * g + 0.114f * b) / (float)count : 0.0f;
        }
    }

    int hist[4][DEPTH_CUT_BINS];
    memset(hist, 0, sizeof(hist));
    int stepX = w / 64 > 1 ? w / 64 : 1;
    int stepY = h / 64 > 1 ? h / 64 : 1;
    int samples = 0;
    for (int y = stepY / 2; y < h; y += stepY) {
        const float* row = rgb + (size_t)y * w * 3;
        for (int x = stepX / 2, i = 0; x < w; x += stepX, i++) {
            const float* q = row + x * 3;
            float l = 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
            int bin = (int)(l * DEPTH_CUT_BINS);
            bin = bin < 0 ? 0 : bin >= DEPTH_CUT_BINS ? DEPTH_CUT_BINS - 1 : bin;
            hist[i & 3][bin]++;
            samples++;
        }
    }
    float inv = samples > 0 ? 1.0f / (float)samples : 0.0f;
    for (int bin = 0; bin < DEPTH_CUT_BINS; bin++) {
        t->hist[bin] = (hist[0][bin] + hist[1][bin] + hist[2][bin] + hist[3][bin]) * inv;
    }
}

float depthThumbDiff(const DepthThumb* a, const DepthThumb* b) {
    float sum = 0.0f;
    for (int i = 0; i < DEPTH_CUT_GRID * DEPTH_CUT_GRID; i++) {
        sum += fabsf(a->grid[i] - b->grid[i]);
    }
    return sum / (DEPTH_CUT_GRID * DEPTH_CUT_GRID);
}

float depthThumbHistDiff(const DepthThumb* a, const DepthThumb* b) {
    float sum = 0.0f;
    for (int i = 0; i < DEPTH_CUT_BINS; i++) {
        sum += fabsf(a->hist[i] - b->hist[i]);
    }
    return 0.5f * sum;
}

float depthThumbCorr(const DepthThumb* a, const DepthThumb* b) {
    const int count = DEPTH_CUT_GRID * DEPTH_CUT_GRID;
    float ma = 0.0f, mb = 0.0f;
    for (int i = 0; i < count; i++) {
        ma += a->grid[i];
        mb += b->grid[i];
    }
    ma /= count;
    mb /= count;
    float ab = 0.0f, aa = 0.0f, bb = 0.0f;
    for (int i = 0; i < count; i++) {
        float x = a->grid[i] - ma;
        float y = b->grid[i] - mb;
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    if (aa < DEPTH_CUT_FLAT * count || bb < DEPTH_CUT_FLAT * count) {
        return 0.0f;
    }
    return ab / sqrtf(aa * bb);
}

// A cut is a settled picture, one jump, then a capture that stays with where
// the jump went. A flash jumps there and back and a fast pan is a run of
// jumps, so neither is confirmed, though the per texel average still starts
// again at every jump, which costs one map without it. A fade or an exposure
// change keeps the layout, which the correlation sees, and a pan across a
// skipped capture moves about as far on the steps either side of it and
// further again from the capture two back.
int depthCutStep(DepthCut* c, const DepthThumb* t, float* diff, float* hist) {
    if (!c->haveLast) {
        c->last = *t;
        c->haveLast = 1;
        c->haveOlder = 0;
        c->lastJumped = 0;
        c->pending = 0;
        c->lastDiff = 0.0f;
        *diff = 0.0f;
        *hist = 0.0f;
        return 0;
    }
    float d = depthThumbDiff(t, &c->last);
    float h = depthThumbHistDiff(t, &c->last);
    int jump = d > DEPTH_CUT_JUMP_DIFF || h > DEPTH_CUT_JUMP_HIST;
    int result = jump ? DEPTH_CUT_JUMP : 0;
    *diff = d;
    *hist = h;

    if (c->pending) {
        c->pending = 0;
        int farFromBefore = depthThumbDiff(t, &c->before) > DEPTH_CUT_JUMP_DIFF
                || depthThumbHistDiff(t, &c->before) > DEPTH_CUT_JUMP_HIST;
        int nearLast = d < DEPTH_CUT_SAME_DIFF && h < DEPTH_CUT_SAME_HIST;
        float corr = depthThumbCorr(&c->last, &c->before);
        float around = c->settledDiff > d ? c->settledDiff : d;
        if (farFromBefore && nearLast && corr < DEPTH_CUT_CORR_MAX
                && c->jumpDiff >= DEPTH_CUT_STAND_OUT * around
                && c->twoBackDiff <= DEPTH_CUT_TWO_BACK * c->jumpDiff) {
            result |= DEPTH_CUT_CONFIRMED;
            *diff = c->jumpDiff;
            *hist = c->jumpHist;
            c->corr = corr;
        }
    }
    // Only a jump out of a capture that did not jump waits for confirmation,
    // so this never arms over one already waiting
    if (jump && !c->lastJumped) {
        c->pending = 1;
        c->before = c->last;
        c->settledDiff = c->lastDiff;
        c->twoBackDiff = c->haveOlder ? depthThumbDiff(t, &c->older) : d;
        c->jumpDiff = d;
        c->jumpHist = h;
    }
    c->lastJumped = jump;
    c->lastDiff = d;
    c->older = c->last;
    c->haveOlder = 1;
    c->last = *t;
    return result;
}
