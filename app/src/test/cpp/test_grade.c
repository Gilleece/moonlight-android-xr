// The picture grade: its lanes, when it is off, and what it does to a colour,
// including the picture as streamed coming through it untouched
#include "check.h"
#include "xr_grade.h"

static const int NEUTRAL[PICTURE_VALUES] = {
    PICTURE_BRIGHTNESS_DEFAULT, PICTURE_CONTRAST_DEFAULT, PICTURE_GAMMA_DEFAULT,
    PICTURE_SATURATION_DEFAULT
};

static void gradeUnits(int brightness, int contrast, int gamma, int saturation,
                       const float in[3], float out[3]) {
    int units[PICTURE_VALUES] = { brightness, contrast, gamma, saturation };
    PictureGrade grade = pictureGradeFor(units);
    pictureGradeApply(&grade, in, out);
}

static float luma(const float c[3]) {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

static void testLanes(void) {
    CHECK(pictureMin(PICTURE_BRIGHTNESS) == -50 && pictureMax(PICTURE_BRIGHTNESS) == 50);
    CHECK(pictureMin(PICTURE_CONTRAST) == 50 && pictureMax(PICTURE_CONTRAST) == 150);
    CHECK(pictureMin(PICTURE_GAMMA) == 50 && pictureMax(PICTURE_GAMMA) == 200);
    CHECK(pictureMin(PICTURE_SATURATION) == 0 && pictureMax(PICTURE_SATURATION) == 200);
    for (int row = 0; row < PICTURE_VALUES; row++) {
        CHECK(pictureDefault(row) == NEUTRAL[row]);
        CHECK(pictureDefault(row) > pictureMin(row) && pictureDefault(row) < pictureMax(row));
        CHECK(pictureClamp(row, -1000) == pictureMin(row));
        CHECK(pictureClamp(row, 1000) == pictureMax(row));
        CHECK(pictureClamp(row, pictureDefault(row)) == pictureDefault(row));
    }
    // The ticks are where the panel draws them
    CHECK(NEUTRAL[PICTURE_BRIGHTNESS] == 0);
    CHECK(NEUTRAL[PICTURE_CONTRAST] == 100);
    CHECK(NEUTRAL[PICTURE_GAMMA] == 100);
    CHECK(NEUTRAL[PICTURE_SATURATION] == 100);
    // A row out of range reads as brightness rather than off the end
    CHECK(pictureMin(-1) == PICTURE_BRIGHTNESS_MIN);
    CHECK(pictureMax(PICTURE_VALUES) == PICTURE_BRIGHTNESS_MAX);
}

static void testNeutral(void) {
    CHECK(pictureNeutral(NEUTRAL));
    for (int row = 0; row < PICTURE_VALUES; row++) {
        int units[PICTURE_VALUES] = { NEUTRAL[0], NEUTRAL[1], NEUTRAL[2], NEUTRAL[3] };
        units[row] += 1;
        CHECK(!pictureNeutral(units));
        units[row] = NEUTRAL[row] - 1;
        CHECK(!pictureNeutral(units));
        // Off the end of a lane is the end, never the default
        units[row] = 5000;
        CHECK(!pictureNeutral(units));
    }
    PictureGrade grade = pictureGradeFor(NEUTRAL);
    CHECK_NEAR(grade.offset, 0.0, 1e-7);
    CHECK_NEAR(grade.contrast, 1.0, 1e-7);
    CHECK_NEAR(grade.exponent, 1.0, 1e-7);
    CHECK_NEAR(grade.saturation, 1.0, 1e-7);
}

static void testUnitsToGrade(void) {
    int units[PICTURE_VALUES] = { 12, 120, 160, 80 };
    PictureGrade grade = pictureGradeFor(units);
    CHECK_NEAR(grade.offset, 0.12, 1e-6);
    CHECK_NEAR(grade.contrast, 1.2, 1e-6);
    CHECK_NEAR(grade.exponent, 1.0 / 1.6, 1e-6);
    CHECK_NEAR(grade.saturation, 0.8, 1e-6);
    // Out of lane values are held to it first
    int wild[PICTURE_VALUES] = { -90, 10, 0, 900 };
    grade = pictureGradeFor(wild);
    CHECK_NEAR(grade.offset, -0.5, 1e-6);
    CHECK_NEAR(grade.contrast, 0.5, 1e-6);
    CHECK_NEAR(grade.exponent, 2.0, 1e-6);
    CHECK_NEAR(grade.saturation, 2.0, 1e-6);
}

// At the ticks the grade is the identity, every grey level and a grid of
// colours, so leaving it on would still show the picture as streamed
static void testTicksAreTheIdentity(void) {
    PictureGrade grade = pictureGradeFor(NEUTRAL);
    for (int v = 0; v <= 255; v++) {
        float c = v / 255.0f;
        float in[3] = { c, c, c };
        float out[3];
        pictureGradeApply(&grade, in, out);
        for (int i = 0; i < 3; i++) {
            CHECK_NEAR(out[i], c, 1e-6);
        }
    }
    for (int r = 0; r <= 8; r++) {
        for (int g = 0; g <= 8; g++) {
            for (int b = 0; b <= 8; b++) {
                float in[3] = { r / 8.0f, g / 8.0f, b / 8.0f };
                float out[3];
                pictureGradeApply(&grade, in, out);
                for (int i = 0; i < 3; i++) {
                    CHECK_NEAR(out[i], in[i], 1e-6);
                }
            }
        }
    }
}

// Every corner of every lane over a grid of colours stays inside 0..1
static void testClamped(void) {
    const int corners[PICTURE_VALUES][2] = {
        { -50, 50 }, { 50, 150 }, { 50, 200 }, { 0, 200 }
    };
    for (int mask = 0; mask < 16; mask++) {
        int units[PICTURE_VALUES];
        for (int row = 0; row < PICTURE_VALUES; row++) {
            units[row] = corners[row][(mask >> row) & 1];
        }
        PictureGrade grade = pictureGradeFor(units);
        for (int r = 0; r <= 4; r++) {
            for (int g = 0; g <= 4; g++) {
                for (int b = 0; b <= 4; b++) {
                    float in[3] = { r / 4.0f, g / 4.0f, b / 4.0f };
                    float out[3];
                    pictureGradeApply(&grade, in, out);
                    for (int i = 0; i < 3; i++) {
                        CHECK(out[i] >= 0.0f && out[i] <= 1.0f);
                    }
                }
            }
        }
    }
    float white[3] = { 1.0f, 1.0f, 1.0f };
    float black[3] = { 0.0f, 0.0f, 0.0f };
    float out[3];
    gradeUnits(50, 150, 100, 100, white, out);
    CHECK_NEAR(out[0], 1.0, 1e-6);
    gradeUnits(-50, 150, 100, 100, black, out);
    CHECK_NEAR(out[0], 0.0, 1e-6);
    // A colour the video's conversion left under black or over white is
    // graded as the black or white the screen shows, so a lift brings it up
    // with the black around it rather than darker
    float under[3] = { -0.08f, 0.0f, 1.1f };
    gradeUnits(30, 100, 100, 100, under, out);
    CHECK_NEAR(out[0], 0.3, 1e-6);
    CHECK_NEAR(out[1], 0.3, 1e-6);
    CHECK_NEAR(out[2], 1.0, 1e-6);
    gradeUnits(-30, 100, 100, 100, under, out);
    CHECK_NEAR(out[2], 0.7, 1e-6);
    // Doubled saturation runs a strong colour off both ends, and is held there
    float red[3] = { 0.9f, 0.2f, 0.1f };
    gradeUnits(0, 100, 100, 200, red, out);
    CHECK_NEAR(out[0], 1.0, 1e-6);
    CHECK_NEAR(out[1], 0.0584, 1e-5);
    CHECK_NEAR(out[2], 0.0, 1e-6);
}

// One pixel by hand through the far ends of three lanes. Contrast and
// brightness: (0.2 0.4 0.6 - 0.5) * 1.5 + 0.5 + 0.5 is 0.55 0.85 1.15, held
// to 0.55 0.85 1. Gamma 2 takes the square root: 0.74162 0.92195 1. No
// saturation leaves the luma of that in all three, 0.88925.
static void testKnownPixel(void) {
    float in[3] = { 0.2f, 0.4f, 0.6f };
    float out[3];
    gradeUnits(50, 150, 200, 0, in, out);
    for (int i = 0; i < 3; i++) {
        CHECK_NEAR(out[i], 0.8892502, 1e-5);
    }
    // Brightness alone is a plain offset
    gradeUnits(30, 100, 100, 100, in, out);
    CHECK_NEAR(out[0], 0.5, 1e-6);
    CHECK_NEAR(out[1], 0.7, 1e-6);
    CHECK_NEAR(out[2], 0.9, 1e-6);
}

static void testEachRowDoesItsJob(void) {
    float grey[3] = { 0.5f, 0.5f, 0.5f };
    float dark[3] = { 0.1f, 0.15f, 0.2f };
    float black[3] = { 0.0f, 0.0f, 0.0f };
    float white[3] = { 1.0f, 1.0f, 1.0f };
    float out[3];
    // Contrast turns about mid grey, which stays put
    gradeUnits(0, 150, 100, 100, grey, out);
    CHECK_NEAR(out[0], 0.5, 1e-6);
    gradeUnits(0, 50, 100, 100, grey, out);
    CHECK_NEAR(out[0], 0.5, 1e-6);
    gradeUnits(0, 150, 100, 100, dark, out);
    CHECK(out[0] < dark[0]);
    // Gamma over 1 lifts the mid tones and keeps both ends where they are
    gradeUnits(0, 100, 160, 100, grey, out);
    CHECK_NEAR(out[0], 0.6484198, 1e-5);
    gradeUnits(0, 100, 160, 100, black, out);
    CHECK_NEAR(out[0], 0.0, 1e-6);
    gradeUnits(0, 100, 160, 100, white, out);
    CHECK_NEAR(out[0], 1.0, 1e-6);
    gradeUnits(0, 100, 60, 100, grey, out);
    CHECK(out[0] < 0.5f);
    // Brightness lifts every step of the lane a little more
    float last = -1.0f;
    for (int b = -50; b <= 50; b += 5) {
        gradeUnits(b, 100, 100, 100, dark, out);
        CHECK(luma(out) >= last);
        last = luma(out);
    }
    // No saturation is the colour's own luma in every channel, and the luma
    // is what saturation turns about, so it keeps it at any setting short of
    // a clamp
    float colour[3] = { 0.6f, 0.3f, 0.2f };
    gradeUnits(0, 100, 100, 0, colour, out);
    CHECK_NEAR(out[0], luma(colour), 1e-6);
    CHECK_NEAR(out[1], luma(colour), 1e-6);
    CHECK_NEAR(out[2], luma(colour), 1e-6);
    gradeUnits(0, 100, 100, 150, colour, out);
    CHECK_NEAR(luma(out), luma(colour), 1e-6);
    CHECK(out[0] - out[2] > colour[0] - colour[2]);
}

int main(void) {
    testLanes();
    testNeutral();
    testUnitsToGrade();
    testTicksAreTheIdentity();
    testClamped();
    testKnownPixel();
    testEachRowDoesItsJob();
    return checksDone("xr_grade");
}
