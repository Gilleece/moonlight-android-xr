
// CPU filtering of the depth model output: the robust range the map is
// normalised against and the low pass that splits it into overall shape
// and local detail. Plain arrays in and out, so it builds anywhere.

#ifndef XR_DEPTHMAP_H
#define XR_DEPTHMAP_H

// Bins for the percentile search over the model output
#define DEPTH_HIST_BINS 512

void robustRange(const float* v, int count, float* outLo, float* outHi);
void lowPass(const float* src, float* dst, float* scratch, float* colSums, int n, int r);

#endif
