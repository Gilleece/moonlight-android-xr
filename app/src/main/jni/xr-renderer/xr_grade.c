// The picture grade's arithmetic. No GL and no context, so the host tests
// reach all of it. GRADE_GLSL in xr_shaders.c does the same sums per pixel.
#include <math.h>

#include "xr_grade.h"

static const int LANE_MIN[PICTURE_VALUES] = {
    PICTURE_BRIGHTNESS_MIN, PICTURE_CONTRAST_MIN, PICTURE_GAMMA_MIN, PICTURE_SATURATION_MIN
};
static const int LANE_MAX[PICTURE_VALUES] = {
    PICTURE_BRIGHTNESS_MAX, PICTURE_CONTRAST_MAX, PICTURE_GAMMA_MAX, PICTURE_SATURATION_MAX
};
static const int LANE_DEFAULT[PICTURE_VALUES] = {
    PICTURE_BRIGHTNESS_DEFAULT, PICTURE_CONTRAST_DEFAULT, PICTURE_GAMMA_DEFAULT,
    PICTURE_SATURATION_DEFAULT
};

static int rowOf(int row) {
    return row >= 0 && row < PICTURE_VALUES ? row : PICTURE_BRIGHTNESS;
}

int pictureMin(int row) {
    return LANE_MIN[rowOf(row)];
}

int pictureMax(int row) {
    return LANE_MAX[rowOf(row)];
}

int pictureDefault(int row) {
    return LANE_DEFAULT[rowOf(row)];
}

int pictureClamp(int row, int units) {
    int lo = pictureMin(row);
    int hi = pictureMax(row);
    return units < lo ? lo : (units > hi ? hi : units);
}

int pictureNeutral(const int units[PICTURE_VALUES]) {
    for (int row = 0; row < PICTURE_VALUES; row++) {
        if (pictureClamp(row, units[row]) != pictureDefault(row)) {
            return 0;
        }
    }
    return 1;
}

PictureGrade pictureGradeFor(const int units[PICTURE_VALUES]) {
    PictureGrade grade;
    grade.offset = pictureClamp(PICTURE_BRIGHTNESS, units[PICTURE_BRIGHTNESS]) / 100.0f;
    grade.contrast = pictureClamp(PICTURE_CONTRAST, units[PICTURE_CONTRAST]) / 100.0f;
    // Never zero: the lane starts at half
    grade.exponent = 100.0f / (float)pictureClamp(PICTURE_GAMMA, units[PICTURE_GAMMA]);
    grade.saturation = pictureClamp(PICTURE_SATURATION, units[PICTURE_SATURATION]) / 100.0f;
    return grade;
}

static float unit(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

void pictureGradeApply(const PictureGrade* grade, const float in[3], float out[3]) {
    float c[3];
    for (int i = 0; i < 3; i++) {
        float v = unit((unit(in[i]) - 0.5f) * grade->contrast + 0.5f + grade->offset);
        c[i] = powf(v, grade->exponent);
    }
    float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    for (int i = 0; i < 3; i++) {
        out[i] = unit(luma + (c[i] - luma) * grade->saturation);
    }
}
