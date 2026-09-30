
// CPU filtering of the depth model output: the robust range the map is
// normalised against, the low pass that splits it into overall shape and
// local detail, the averages over time, and the scene cut detector on the
// model input. Plain arrays in and out, so it builds anywhere.

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
// which reads as the picture swelling and shrinking on a pan. A confirmed cut
// starts it again rather than waiting it out.
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

// Scene cuts, found on the model input before the model runs. Each capture is
// boiled down to a grid of box averaged luma and a coarse luma histogram, and
// compared with the captures before it.
#define DEPTH_CUT_GRID 16
#define DEPTH_CUT_BINS 16

typedef struct {
    float grid[DEPTH_CUT_GRID * DEPTH_CUT_GRID];
    float hist[DEPTH_CUT_BINS];
} DepthThumb;

// From a w by h RGB float image, 0..1. An image smaller than the grid on
// either side gives an empty thumb.
void depthThumbMake(DepthThumb* t, const float* rgb, int w, int h);
// Mean absolute difference of the two grids, 0..1
float depthThumbDiff(const DepthThumb* a, const DepthThumb* b);
// Half the L1 distance of the two histograms, 0 the same, 1 no overlap
float depthThumbHistDiff(const DepthThumb* a, const DepthThumb* b);
// Correlation of the two grids, -1..1, and 0 when either is flat, since a
// flat grid has no layout to keep
float depthThumbCorr(const DepthThumb* a, const DepthThumb* b);

// Set from a Quest 3 log of an 81 s film reel with 31 cuts, each gate in the
// middle of the gap between the cuts and the jumps that were not cuts.
//
// A jump is a grid step over 0.06 or a histogram distance over 0.5. Every cut
// stepped the grid by 0.094 or more, and a jump that is not a cut only costs
// one map without the average, so the gates below do the rejecting.
#define DEPTH_CUT_JUMP_DIFF 0.06f
#define DEPTH_CUT_JUMP_HIST 0.50f
// The capture after a cut stays this close to it. A flash jumps back by as
// much as it jumped, which is further.
#define DEPTH_CUT_SAME_DIFF 0.06f
#define DEPTH_CUT_SAME_HIST 0.20f
// The jumps that were not cuts (an exposure pulse, fireworks, a fast zoom)
// kept the layout, the grid correlating 0.725 or more with the one before
// where no cut passed 0.505. They stood out from the steps either side 2.3
// times at most where cuts did 5 times or more. And they carried on the
// motion before them, landing at least 1.23 times as far from the capture two
// back as from the last one, where a cut stays about as far from both.
#define DEPTH_CUT_CORR_MAX 0.60f
#define DEPTH_CUT_STAND_OUT 3.0f
#define DEPTH_CUT_TWO_BACK 1.12f
// A grid whose cells vary by less than this, as a variance, is flat
#define DEPTH_CUT_FLAT 1e-5f

typedef struct {
    DepthThumb last;
    // The capture before the last one
    DepthThumb older;
    // The capture ahead of a jump that waits on the next one to confirm it
    DepthThumb before;
    int haveLast;
    int haveOlder;
    int lastJumped;
    int pending;
    // The last step's grid difference, the step into the capture ahead of
    // the waiting jump, the jump against the capture two back, and the jump
    float lastDiff;
    float settledDiff;
    float twoBackDiff;
    float jumpDiff;
    float jumpHist;
    // The last confirmed cut's correlation, for the log
    float corr;
} DepthCut;

// A capture far from the last one: the per texel average starts again
#define DEPTH_CUT_JUMP 1
// The capture after a jump stayed with the new picture, and the jump changed
// the layout, stood out from the steps around it and did not carry on the
// motion before it: the range starts again as well
#define DEPTH_CUT_CONFIRMED 2

// Takes the next capture's thumb and says what it was, as the flags above.
// On a confirmed cut diff and hist are the jump's, otherwise this step's.
int depthCutStep(DepthCut* c, const DepthThumb* t, float* diff, float* hist);

// Forgets every capture, so the next one only seeds the detector
static inline void depthCutClear(DepthCut* c) {
    c->haveLast = 0;
    c->haveOlder = 0;
    c->lastJumped = 0;
    c->pending = 0;
}

#endif
