
// CPU filtering of the depth model output: the robust range the map is
// normalised against, the low pass that splits it into overall shape and
// local detail, and the averages over time. Plain arrays in and out, so it
// builds anywhere.

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

// Both averages below step over real time rather than once per map, so they
// settle in the same time whatever rate the model manages. The time since the
// last map counts as at least 1/60 s, so a burst of maps cannot stall them,
// and at most a second, so one map after a pause cannot count for more.
#define DEPTH_TAU_DT_MIN_S (1.0f / 60.0f)
#define DEPTH_TAU_DT_MAX_S 1.0f

// Time constants in milliseconds. The per texel average takes the model's
// flicker off without a lag the eye follows. The range is much slower on
// purpose: it should follow the scene, not the frame. The maxima are only for
// the debug knobs.
#define DEPTH_TAU_DEFAULT_MS 30
#define DEPTH_TAU_MAX_MS 10000
#define DEPTH_RANGE_TAU_DEFAULT_MS 150
#define DEPTH_RANGE_TAU_MAX_MS 10000

// How far a texel moves towards the newest map: 1 - exp(-dt / tau) with dt
// held to the limits above, and 1, no averaging, when tau is 0
float depthTauAlpha(float dtSec, float tauSec);

// Each texel of avg a step of alpha towards v, or v itself on a seed. A value
// that is not finite leaves its texel alone, and a texel that is not finite
// takes the next value that is, so one NaN out of the model cannot stay.
void depthTauBlend(float* avg, const float* v, int count, float alpha, int seed);

// The range the map is normalised against, smoothed the same way. A relative
// depth model re-normalises every frame, so without this the whole scene's
// depth breathes with whatever happens to be nearest and furthest in shot,
// which reads as the picture swelling and shrinking on a pan.
typedef struct {
    float lo, hi;
    int valid;
} DepthRange;

// A step towards this map's lo and hi, or the two taken as they are when the
// range holds nothing yet or tau is 0
void depthRangeStep(DepthRange* r, float lo, float hi, float dtSec, float tauSec);

// One over the span, held to at least depthSpanFloor(lo): the two ends are
// smoothed apart, and a span that rounds to nothing would make every texel of
// the map NaN
float depthRangeScale(const DepthRange* r);

#endif
