// The ambilight glow's colour arithmetic and its shape around a curved
// picture. No GL and no context, so the host tests reach all of it.
// GLOW_EDGE_FRAGMENT_SRC does the same colour sums on the GPU.
#include <math.h>

#include "xr_glow.h"

static float smooth(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

float glowLuma(const float rgb[3]) {
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

void glowNormalise(const float in[3], float out[3]) {
    float l = glowLuma(in);
    float s = smooth(GLOW_LUMA_FLOOR, GLOW_LUMA_FULL, l);
    // The luma it lands on, then the gain that takes it there. Every channel
    // moves by the same gain, so the hue is the picture's.
    float lifted = l + (GLOW_LUMA_TARGET - l) * s;
    float gain = lifted / fmaxf(l, 1e-5f);
    for (int i = 0; i < 3; i++) {
        float c = in[i] * gain;
        out[i] = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    }
}

float glowLit(const float rgb[3]) {
    float m = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
    return smooth(GLOW_LIT_LO, GLOW_LIT_HI, m);
}

float glowReachTexels(void) {
    return fmaxf(GLOW_ROLLOFF_REACH * AMBI_SAMPLE_TEX, 1.0f);
}

int glowInwardSteps(void) {
    return (int)(GLOW_INWARD_DEPTH * AMBI_SAMPLE_TEX - 0.5f);
}

float glowRolloffWeight(int k, float reach) {
    if (reach <= 0.0f) {
        return k == 0 ? 1.0f : 0.0f;
    }
    float t = k / reach;
    float w = 1.0f - t * t;
    return w > 0.0f ? w : 0.0f;
}

void glowRingTexel(int i, int* x, int* y) {
    const int n = AMBI_SAMPLE_TEX;
    i = ((i % GLOW_RING) + GLOW_RING) % GLOW_RING;
    if (i < n - 1) {
        *x = i;
        *y = 0;
    }
    else if (i < 2 * (n - 1)) {
        *x = n - 1;
        *y = i - (n - 1);
    }
    else if (i < 3 * (n - 1)) {
        *x = 3 * (n - 1) - i;
        *y = n - 1;
    }
    else {
        *x = 0;
        *y = GLOW_RING - i;
    }
}

int glowRingIndex(int x, int y) {
    const int n = AMBI_SAMPLE_TEX;
    if (x < 0 || y < 0 || x >= n || y >= n) {
        return -1;
    }
    if (y == 0) {
        return x;
    }
    if (x == n - 1) {
        return (n - 1) + y;
    }
    if (y == n - 1) {
        return 3 * (n - 1) - x;
    }
    if (x == 0) {
        return GLOW_RING - y;
    }
    return -1;
}

int glowCylinderFor(float screenWidth, float screenHeight, float screenRadius,
                    GlowCylinder* out) {
    if (screenRadius <= 1e-3f || screenWidth <= 0.0f || screenHeight <= 0.0f) {
        return 0;
    }
    float wanted = GLOW_SCALE * screenWidth / screenRadius;
    int columns = GLOW_TEX;
    if (wanted > GLOW_MAX_ANGLE) {
        // Even, so the columns left out are the same either side
        columns = (int)(GLOW_TEX * GLOW_MAX_ANGLE / wanted) & ~1;
        if (columns < 2) {
            columns = 2;
        }
    }
    out->rectWidth = columns;
    out->rectX = (GLOW_TEX - columns) / 2;
    // From the whole columns, so a cut glow is exactly as wide as what it shows
    out->centralAngle = wanted * columns / GLOW_TEX;
    out->radius = screenRadius > 2.0f * GLOW_PROUD_M ? screenRadius - GLOW_PROUD_M : screenRadius;
    out->aspectRatio = out->radius * out->centralAngle / (GLOW_SCALE * screenHeight);
    return 1;
}
