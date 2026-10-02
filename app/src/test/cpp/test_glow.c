// The glow's colour arithmetic: the steady luma, what counts as lit, the roll
// off weights and the ring, and the edge pass put together from them the way
// GLOW_EDGE_FRAGMENT_SRC does it, run over made up frames
#include <stdlib.h>

#include "check.h"
#include "xr_glow.h"
#include "xr_math.h"

#define N AMBI_SAMPLE_TEX

// A sample texture, rows from the bottom, the way the GPU holds it
typedef struct {
    float c[N][N][3];
} Tex;

static Tex texA;
static Tex texB;
static Tex outA;
static Tex outB;

static void fill(Tex* t, float r, float g, float b) {
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            t->c[y][x][0] = r;
            t->c[y][x][1] = g;
            t->c[y][x][2] = b;
        }
    }
}

static void set(Tex* t, int x, int y, float r, float g, float b) {
    t->c[y][x][0] = r;
    t->c[y][x][1] = g;
    t->c[y][x][2] = b;
}

static float maxChannel(const float c[3]) {
    return fmaxf(c[0], fmaxf(c[1], c[2]));
}

// The shader's edgeColour: the ring texel lifted, or where it is dark the
// first lit one further in
static void edgeColour(const Tex* t, int i, float out[3]) {
    int x, y;
    glowRingTexel(i, &x, &y);
    int ix = x == 0 ? 1 : (x == N - 1 ? -1 : 0);
    int iy = y == 0 ? 1 : (y == N - 1 ? -1 : 0);
    glowNormalise(t->c[y][x], out);
    for (int s = 1; s <= glowInwardSteps(); s++) {
        float deeper[3];
        glowNormalise(t->c[y + iy * s][x + ix * s], deeper);
        float l = glowLit(out);
        for (int k = 0; k < 3; k++) {
            out[k] = deeper[k] + (out[k] - deeper[k]) * l;
        }
    }
}

// The shader's main, over every texel
static void edgePass(const Tex* in, Tex* out) {
    float reach = glowReachTexels();
    int r = (int)reach;
    if (r > GLOW_RING / 2 - 1) {
        r = GLOW_RING / 2 - 1;
    }
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int j = glowRingIndex(x, y);
            if (j < 0) {
                glowNormalise(in->c[y][x], out->c[y][x]);
                continue;
            }
            float own[3] = { 0.0f, 0.0f, 0.0f };
            float sum[3] = { 0.0f, 0.0f, 0.0f };
            float weight = 0.0f;
            float carried = 0.0f;
            for (int k = -r; k <= r; k++) {
                float c[3];
                edgeColour(in, (j + k + GLOW_RING) % GLOW_RING, c);
                float w = glowRolloffWeight(k, reach) * glowLit(c);
                for (int ch = 0; ch < 3; ch++) {
                    sum[ch] += c[ch] * w;
                }
                weight += w;
                carried = fmaxf(carried, w);
                if (k == 0) {
                    own[0] = c[0];
                    own[1] = c[1];
                    own[2] = c[2];
                }
            }
            float dark = 1.0f - glowLit(own);
            for (int ch = 0; ch < 3; ch++) {
                float nearby = sum[ch] / fmaxf(weight, 1e-4f);
                float v = own[ch] + nearby * carried * dark;
                out->c[y][x][ch] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
        }
    }
}

static void testSteadyLuma(void) {
    // A bright frame and the same frame at a fifth of the light glow alike
    float bright[3] = { 0.80f, 0.40f, 0.10f };
    float dark[3] = { 0.16f, 0.08f, 0.02f };
    float a[3], b[3];
    glowNormalise(bright, a);
    glowNormalise(dark, b);
    CHECK_NEAR(glowLuma(a), GLOW_LUMA_TARGET, 1e-4);
    CHECK_NEAR(glowLuma(b), GLOW_LUMA_TARGET, 1e-4);
    for (int i = 0; i < 3; i++) {
        CHECK_NEAR(a[i], b[i], 1e-4);
    }
    // In the frame's own hue: the channels keep their proportions
    CHECK_NEAR(a[0] / a[1], 2.0, 1e-4);
    CHECK_NEAR(a[1] / a[2], 4.0, 1e-4);

    // A grey too bright comes down to the same level
    float white[3] = { 1.0f, 1.0f, 1.0f };
    glowNormalise(white, a);
    CHECK_NEAR(a[0], GLOW_LUMA_TARGET, 1e-4);
    CHECK_NEAR(a[2], GLOW_LUMA_TARGET, 1e-4);

    // A saturated colour that cannot reach the target is held to the gamut
    // without its hue moving
    float blue[3] = { 0.0f, 0.0f, 0.6f };
    glowNormalise(blue, a);
    CHECK(a[0] == 0.0f && a[1] == 0.0f);
    CHECK_NEAR(a[2], 1.0, 1e-6);
}

static void testBlackStaysDark(void) {
    float black[3] = { 0.0f, 0.0f, 0.0f };
    float out[3];
    glowNormalise(black, out);
    CHECK(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);
    // Under the floor nothing moves, so the noise in a black frame stays put
    float noise[3] = { 0.02f, 0.025f, 0.015f };
    glowNormalise(noise, out);
    for (int i = 0; i < 3; i++) {
        CHECK_NEAR(out[i], noise[i], 1e-6);
    }
}

static void testKneeIsSmooth(void) {
    // Over a grey ramp the lifted luma never falls back and never jumps, so a
    // texel crossing the knee as the sample settles brightens over frames
    float last = 0.0f;
    float biggest = 0.0f;
    int falls = 0;
    for (int i = 0; i <= 1000; i++) {
        float v = i / 1000.0f;
        float grey[3] = { v, v, v };
        float out[3];
        glowNormalise(grey, out);
        float l = glowLuma(out);
        if (l < last - 1e-6f) {
            falls++;
        }
        if (i > 0 && l - last > biggest) {
            biggest = l - last;
        }
        last = l;
    }
    CHECK(falls == 0);
    // About 15 times the slope of the input at its steepest, against a step
    // that would be 0.45 in one go
    CHECK(biggest < 0.02f);
    // The ends of the knee land where they say
    float atFloor[3] = { GLOW_LUMA_FLOOR, GLOW_LUMA_FLOOR, GLOW_LUMA_FLOOR };
    float atFull[3] = { GLOW_LUMA_FULL, GLOW_LUMA_FULL, GLOW_LUMA_FULL };
    float out[3];
    glowNormalise(atFloor, out);
    CHECK_NEAR(glowLuma(out), GLOW_LUMA_FLOOR, 1e-5);
    glowNormalise(atFull, out);
    CHECK_NEAR(glowLuma(out), GLOW_LUMA_TARGET, 1e-5);
}

static void testLit(void) {
    float black[3] = { 0.0f, 0.0f, 0.0f };
    float red[3] = { 0.5f, 0.0f, 0.0f };
    float dim[3] = { 0.05f, 0.05f, 0.05f };
    CHECK(glowLit(black) == 0.0f);
    CHECK(glowLit(red) == 1.0f);
    CHECK(glowLit(dim) == 0.0f);
    // Anything the normalisation lifted all the way counts as fully lit, over
    // a sweep of colours at or above the top of the knee
    int unlit = 0;
    for (int r = 0; r <= 10; r++) {
        for (int g = 0; g <= 10; g++) {
            for (int b = 0; b <= 10; b++) {
                float c[3] = { r / 10.0f, g / 10.0f, b / 10.0f };
                if (glowLuma(c) < GLOW_LUMA_FULL) {
                    continue;
                }
                float out[3];
                glowNormalise(c, out);
                if (glowLit(out) < 1.0f) {
                    unlit++;
                }
            }
        }
    }
    CHECK(unlit == 0);
}

static void testRolloffWeights(void) {
    float reach = glowReachTexels();
    // A third of a 32 texel edge
    CHECK_NEAR(reach, 32.0 / 3.0, 1e-5);
    CHECK_NEAR(glowRolloffWeight(0, reach), 1.0, 0);
    // Three quarters of the light left halfway out
    CHECK_NEAR(glowRolloffWeight(1, 2.0f), 0.75, 1e-6);
    CHECK_NEAR(glowRolloffWeight(5, reach), 1.0 - (5.0 / reach) * (5.0 / reach), 1e-6);
    float last = 1.0f;
    for (int k = 1; k <= 12; k++) {
        float w = glowRolloffWeight(k, reach);
        CHECK(w == glowRolloffWeight(-k, reach));
        CHECK(w < last || w == 0.0f);
        CHECK(w >= 0.0f);
        last = w;
    }
    // The last tap the shader takes still carries something, the one past it
    // nothing
    CHECK(glowRolloffWeight((int)reach, reach) > 0.1f);
    CHECK(glowRolloffWeight((int)reach + 1, reach) == 0.0f);
    CHECK(glowRolloffWeight(0, 0.0f) == 1.0f);
    CHECK(glowRolloffWeight(1, 0.0f) == 0.0f);
    // Two texels in past the edge one, which end at 9.4 percent of the side
    CHECK(glowInwardSteps() == 2);
}

static void testRing(void) {
    int bad = 0;
    for (int i = 0; i < GLOW_RING; i++) {
        int x, y;
        glowRingTexel(i, &x, &y);
        if (glowRingIndex(x, y) != i) {
            bad++;
        }
        if (x != 0 && y != 0 && x != N - 1 && y != N - 1) {
            bad++;
        }
        // Each step round the ring is one texel to a neighbour
        int nx, ny;
        glowRingTexel(i + 1, &nx, &ny);
        if (abs(nx - x) + abs(ny - y) != 1) {
            bad++;
        }
    }
    CHECK(bad == 0);
    CHECK(GLOW_RING == 124);
    CHECK(glowRingIndex(5, 5) == -1);
    CHECK(glowRingIndex(-1, 0) == -1);
    CHECK(glowRingIndex(0, 0) == 0);
    CHECK(glowRingIndex(N - 1, N - 1) == 2 * (N - 1));
    int x, y;
    glowRingTexel(-1, &x, &y);
    CHECK(x == 0 && y == 1);
}

static void testLitFrameIsOnlyLifted(void) {
    fill(&texA, 0.3f, 0.2f, 0.6f);
    set(&texA, 0, 7, 0.9f, 0.1f, 0.1f);
    edgePass(&texA, &outA);
    int moved = 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float want[3];
            glowNormalise(texA.c[y][x], want);
            for (int ch = 0; ch < 3; ch++) {
                if (fabsf(outA.c[y][x][ch] - want[ch]) > 1e-5f) {
                    moved++;
                }
            }
        }
    }
    CHECK(moved == 0);
}

static void testThinBorderLooksInside(void) {
    // A one texel dark border round a lit picture, under what the bar
    // detection crops: the ring takes the colour from just inside it
    fill(&texA, 0.1f, 0.5f, 0.8f);
    for (int i = 0; i < GLOW_RING; i++) {
        int x, y;
        glowRingTexel(i, &x, &y);
        set(&texA, x, y, 0.0f, 0.0f, 0.0f);
    }
    edgePass(&texA, &outA);
    float inside[3];
    glowNormalise(texA.c[N / 2][N / 2], inside);
    float worst = 0.0f;
    for (int i = 0; i < GLOW_RING; i++) {
        int x, y;
        glowRingTexel(i, &x, &y);
        for (int ch = 0; ch < 3; ch++) {
            worst = fmaxf(worst, fabsf(outA.c[y][x][ch] - inside[ch]));
        }
    }
    CHECK(worst < 1e-4f);
}

static void testLetterboxRollsOff(void) {
    // Bars four texels deep top and bottom, past what the inward look reaches,
    // with an orange picture between: the top and bottom edges take the
    // picture's colour from the sides round the corners and fade to black
    // over a third of the edge, where before they cut straight to nothing
    fill(&texA, 0.0f, 0.0f, 0.0f);
    for (int y = 4; y < N - 4; y++) {
        for (int x = 0; x < N; x++) {
            set(&texA, x, y, 0.9f, 0.45f, 0.1f);
        }
    }
    edgePass(&texA, &outA);
    float lifted[3];
    glowNormalise(texA.c[N / 2][N / 2], lifted);
    // The sides are untouched
    CHECK_NEAR(outA.c[N / 2][0][0], lifted[0], 1e-5);
    CHECK_NEAR(outA.c[N / 2][N - 1][1], lifted[1], 1e-5);
    // Along the top, from the corner in, the light only ever falls, and in
    // the picture's hue
    float last = 2.0f;
    int rises = 0;
    for (int x = 0; x < N / 2; x++) {
        float m = maxChannel(outA.c[N - 1][x]);
        if (m > last + 1e-6f) {
            rises++;
        }
        last = m;
        if (m > 0.01f) {
            CHECK_NEAR(outA.c[N - 1][x][1] / outA.c[N - 1][x][0], lifted[1] / lifted[0], 1e-3);
        }
    }
    CHECK(rises == 0);
    // Lit at the corner, which is four texels round from the nearest lit
    // side texel, and black in the middle of the edge
    CHECK(maxChannel(outA.c[N - 1][0]) > 0.5f * maxChannel(lifted));
    CHECK(maxChannel(outA.c[N - 1][N / 2]) == 0.0f);
    CHECK(maxChannel(outA.c[0][N / 2]) == 0.0f);
    // Symmetric top and bottom, left and right
    CHECK_NEAR(maxChannel(outA.c[N - 1][3]), maxChannel(outA.c[0][3]), 1e-5);
    CHECK_NEAR(maxChannel(outA.c[N - 1][3]), maxChannel(outA.c[N - 1][N - 4]), 1e-5);
}

static void testLitRunRampsDown(void) {
    // The left half lit and the right half black: along the bottom edge the
    // light ramps down out of the lit run over the reach and is gone past it
    fill(&texA, 0.0f, 0.0f, 0.0f);
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N / 2; x++) {
            set(&texA, x, y, 0.2f, 0.6f, 0.3f);
        }
    }
    edgePass(&texA, &outA);
    float lifted[3];
    glowNormalise(texA.c[0][0], lifted);
    float reach = glowReachTexels();
    int off = 0;
    for (int d = 1; d < N / 2; d++) {
        float m = maxChannel(outA.c[0][N / 2 - 1 + d]);
        // As much as the nearest lit texel carries d texels out
        float want = d <= (int)reach ? glowRolloffWeight(d, reach) * maxChannel(lifted) : 0.0f;
        if (fabsf(m - want) > 1e-4f) {
            off++;
        }
    }
    CHECK(off == 0);
    CHECK(maxChannel(outA.c[0][N / 2 - 1]) == maxChannel(lifted));
}

static void testDarkFrameGlowsLikeBright(void) {
    // The same picture at full light and at under a third of it, with a
    // letterbox, come out of the edge pass the same
    fill(&texA, 0.0f, 0.0f, 0.0f);
    fill(&texB, 0.0f, 0.0f, 0.0f);
    for (int y = 5; y < N - 5; y++) {
        for (int x = 0; x < N; x++) {
            float r = 0.4f + 0.5f * x / (N - 1);
            float g = 0.7f - 0.4f * y / (N - 1);
            float b = 0.5f;
            set(&texA, x, y, r, g, b);
            set(&texB, x, y, r * 0.3f, g * 0.3f, b * 0.3f);
        }
    }
    edgePass(&texA, &outA);
    edgePass(&texB, &outB);
    float worst = 0.0f;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            for (int ch = 0; ch < 3; ch++) {
                worst = fmaxf(worst, fabsf(outA.c[y][x][ch] - outB.c[y][x][ch]));
            }
        }
    }
    CHECK(worst < 1e-4f);
}

// The glow round a curved picture. The viewer is at the origin looking down
// -z with the picture's centre straight ahead at distance d, which is where
// the sliders put it, and the radius the curvature slider gives: d at full
// curve, four times d at none.
#define PI_F 3.14159265f
#define DEG (180.0f / PI_F)

static float radiusFor(float d, float curve) {
    return d * (1.0f + 3.0f * (1.0f - curve));
}

// How far round to the right a point a radians round a cylinder of radius r
// is, seen from the viewer, when the cylinder's axis is axisZ along z
static float azimuthOnCylinder(float a, float r, float axisZ) {
    return atan2f(r * sinf(a), r * cosf(a) - axisZ) * DEG;
}

// How high a point h above the middle is, seen from the viewer, at a radians
// round the same cylinder
static float elevationOnCylinder(float a, float r, float axisZ, float h) {
    float x = r * sinf(a);
    float z = axisZ - r * cosf(a);
    return atan2f(h, sqrtf(x * x + z * z)) * DEG;
}

// Halfway out through the fade, as a fraction of the glow image from its
// middle: the screen ends at 0.5 / GLOW_SCALE and the rim is at 0.5
#define HALF_FADE (0.5f / GLOW_SCALE + 0.5f * (0.5f - 0.5f / GLOW_SCALE))

static void testFlatGlowHidesBehindACloseCurve(void) {
    // Issue 9's case: a 3 m picture 1 m away at full curve. The flat glow sits
    // in a plane at the picture's centre, the cylinder's sides come round
    // past where the quad ends, so nothing of it shows at the sides.
    float d = 1.0f, w = 3.0f;
    float r = radiusFor(d, 1.0f);
    float edge = azimuthOnCylinder(0.5f * w / r, r, r - d);
    float flatRim = atan2f(0.5f * GLOW_SCALE * w, d - GLOW_PROUD_M) * DEG;
    CHECK_NEAR(edge, 85.9, 0.1);
    CHECK(flatRim < edge);

    // The cylinder glow reaches well past it, halfway through its fade
    GlowCylinder g;
    CHECK(glowCylinderFor(w, w * 9.0f / 16.0f, r, &g));
    float half = HALF_FADE * g.centralAngle * GLOW_TEX / g.rectWidth;
    CHECK(azimuthOnCylinder(half, g.radius, r - d) > edge + 20.0f);

    // Halfway in, at a gentler curve, the flat glow's fade is still covered
    r = radiusFor(d, 0.5f);
    edge = azimuthOnCylinder(0.5f * w / r, r, r - d);
    float flatHalf = atan2f(HALF_FADE * GLOW_SCALE * w, d - GLOW_PROUD_M) * DEG;
    CHECK(flatHalf < edge);
}

static void testCylinderFollowsTheScreen(void) {
    // Every distance the panel's track reaches, the widths a resize allows
    // and curves from barely to fully, 16:9
    const float distances[] = { 0.2f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
    const float widths[] = { 0.8f, 1.5f, 3.0f, 5.0f, 8.0f };
    const float curves[] = { 0.02f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
    int whole = 0, cut = 0, bad = 0, sides = 0, ends = 0;
    for (int i = 0; i < (int)(sizeof(distances) / sizeof(distances[0])); i++) {
        for (int j = 0; j < (int)(sizeof(widths) / sizeof(widths[0])); j++) {
            for (int k = 0; k < (int)(sizeof(curves) / sizeof(curves[0])); k++) {
                float d = distances[i], w = widths[j];
                float h = w * 9.0f / 16.0f;
                float r = radiusFor(d, curves[k]);
                float screenAngle = w / r;
                GlowCylinder g;
                if (!glowCylinderFor(w, h, r, &g)) {
                    bad++;
                    continue;
                }
                // Always inside the picture's radius by the same few cm, and
                // as tall as the flat glow
                if (fabsf(g.radius - (r - GLOW_PROUD_M)) > 1e-5f) bad++;
                float height = g.radius * g.centralAngle / g.aspectRatio;
                if (fabsf(height - GLOW_SCALE * h) > 1e-4f * h) bad++;
                // The picture covers the middle 1 / GLOW_SCALE of the image's
                // full width, whichever columns are shown
                float perColumn = g.centralAngle / g.rectWidth;
                if (fabsf(perColumn * GLOW_TEX / GLOW_SCALE - screenAngle) > 1e-4f * screenAngle) {
                    bad++;
                }
                if (g.rectWidth % 2 != 0 || g.rectX * 2 + g.rectWidth != GLOW_TEX) bad++;
                if (g.centralAngle > GLOW_MAX_ANGLE || g.centralAngle >= 2.0f * PI_F) bad++;
                if (g.rectWidth == GLOW_TEX) {
                    whole++;
                    if (fabsf(g.centralAngle - GLOW_SCALE * screenAngle) > 1e-5f) bad++;
                }
                else {
                    cut++;
                    // Only cut where the whole would close the circle
                    if (GLOW_SCALE * screenAngle <= GLOW_MAX_ANGLE) bad++;
                }
                if (screenAngle >= GLOW_MAX_ANGLE) {
                    continue;
                }
                // Past the picture's side edge as the viewer sees it, halfway
                // through the fade or, where it is cut, at the cut
                float axisZ = r - d;
                float edge = azimuthOnCylinder(0.5f * screenAngle, r, axisZ);
                float fullAngle = g.centralAngle * GLOW_TEX / g.rectWidth;
                float out = fminf(HALF_FADE * fullAngle, 0.5f * g.centralAngle);
                if (azimuthOnCylinder(out, g.radius, axisZ) <= edge) {
                    sides++;
                }
                // And above the picture's top edge at that edge's column,
                // halfway through the vertical fade
                float top = elevationOnCylinder(0.5f * screenAngle, r, axisZ, 0.5f * h);
                float glowTop = elevationOnCylinder(0.5f * screenAngle, g.radius, axisZ,
                                                    HALF_FADE * GLOW_SCALE * h);
                if (glowTop <= top) {
                    ends++;
                }
            }
        }
    }
    CHECK(bad == 0);
    CHECK(sides == 0);
    CHECK(ends == 0);
    // Both kinds turn up in that sweep
    CHECK(whole > 100);
    CHECK(cut > 10);
}

static void testCylinderEdgeCases(void) {
    GlowCylinder g;
    g.rectWidth = -7;
    CHECK(!glowCylinderFor(3.0f, 1.7f, 0.0f, &g));
    CHECK(!glowCylinderFor(0.0f, 1.7f, 3.0f, &g));
    CHECK(!glowCylinderFor(3.0f, 0.0f, 3.0f, &g));
    CHECK(g.rectWidth == -7);
    // A picture already wrapped all the way round leaves a sliver, never
    // nothing and never more than a cylinder can take
    CHECK(glowCylinderFor(8.0f, 4.5f, 0.2f, &g));
    CHECK(g.rectWidth >= 2);
    CHECK(g.centralAngle <= GLOW_MAX_ANGLE);
    // A radius too small to come in by the offset keeps its own
    CHECK(glowCylinderFor(0.08f, 0.045f, 0.08f, &g));
    CHECK_NEAR(g.radius, 0.08, 1e-6);
    // The nearest screen at full curve, 3 m wide, wraps 15 rad and is drawn
    // held under a full turn and smaller: the glow goes round that picture
    // and still stays short of a turn, its middle columns only
    float held = 0.2f * CYLINDER_MAX_ANGLE;
    CHECK(glowCylinderFor(held, held * 9.0f / 16.0f, 0.2f, &g));
    CHECK(g.centralAngle <= GLOW_MAX_ANGLE && g.centralAngle < 2.0f * PI_F);
    CHECK(g.rectWidth < GLOW_TEX && g.rectWidth >= 2);
    // So does the size it was asked for
    CHECK(glowCylinderFor(3.0f, 3.0f * 9.0f / 16.0f, 0.2f, &g));
    CHECK(g.centralAngle <= GLOW_MAX_ANGLE && g.centralAngle < 2.0f * PI_F);
    CHECK(GLOW_MAX_ANGLE < CYLINDER_MAX_ANGLE);
    // The default screen at full curve, 3 m off: 1 rad of picture, 1.7 of glow
    CHECK(glowCylinderFor(3.0f, 3.0f * 9.0f / 16.0f, 3.0f, &g));
    CHECK_NEAR(g.centralAngle, 1.7, 1e-5);
    CHECK_NEAR(g.radius, 2.95, 1e-5);
    CHECK(g.rectX == 0 && g.rectWidth == GLOW_TEX);
}

int main(void) {
    testSteadyLuma();
    testBlackStaysDark();
    testKneeIsSmooth();
    testLit();
    testRolloffWeights();
    testRing();
    testLitFrameIsOnlyLifted();
    testThinBorderLooksInside();
    testLetterboxRollsOff();
    testLitRunRampsDown();
    testDarkFrameGlowsLikeBright();
    testFlatGlowHidesBehindACloseCurve();
    testCylinderFollowsTheScreen();
    testCylinderEdgeCases();
    return checksDone("xr_glow");
}
