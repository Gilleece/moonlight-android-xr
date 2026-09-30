
// CPU filtering of the depth model output: the robust range the map is
// normalised against and the low pass that splits it into overall shape
// and local detail. Plain arrays in and out, so it builds anywhere.

#ifndef XR_DEPTHMAP_H
#define XR_DEPTHMAP_H

#include <math.h>

// Bins for the percentile search over the model output
#define DEPTH_HIST_BINS 512

// Only finite values count toward the range, so a NaN or an infinity out of
// the model cannot become it
void robustRange(const float* v, int count, float* outLo, float* outHi);

// The narrowest a range starting at lo may be: 1e-6 near zero, and more at
// larger values, so hi - lo never rounds to nothing and one over it stays
// finite
float depthSpanFloor(float lo);

// One over hi - lo, the span held to at least depthSpanFloor(lo)
float depthSpanScale(float lo, float hi);

// A model output value as the map reads it: anything not finite reads as lo,
// the far end of the range, rather than going into the smoothing and staying
static inline float depthRead(float v, float lo) {
    return isfinite(v) ? v : lo;
}

// A w by h map, rx and ry the box radius across and down
void lowPass(const float* src, float* dst, float* scratch, float* colSums, int w, int h,
             int rx, int ry);

// Whether a depth map of w by h fits: 64 up to DEPTH_TEX_W_MAX by
// DEPTH_TEX_H_MAX, both sides multiples of 8
int depthSizeOk(int w, int h);

#endif
