// The picture grade: brightness, contrast, gamma and saturation over the
// streamed picture. No GL and no context, so the host tests reach all of it.
// The warp shader and the frame colour sample do the same sums on the GPU
// from the four numbers PictureGrade carries.

#ifndef XR_GRADE_H
#define XR_GRADE_H

#include "xr_shared.h"

typedef struct {
    // Added after the contrast, in full scale
    float offset;
    // Gain about mid grey
    float contrast;
    // What each channel is raised to, the inverse of the gamma, so a gamma
    // over 1 lifts the mid tones
    float exponent;
    // 0 is grey, 1 the picture's own colour, 2 twice as far from grey
    float saturation;
} PictureGrade;

// Each row's lane and its default, in the preferences' whole units, in the
// PICTURE_ order. A row out of range reads as brightness.
int pictureMin(int row);
int pictureMax(int row);
int pictureDefault(int row);

// Held to the row's lane
int pictureClamp(int row, int units);

// 1 when every row is at its default, which is the picture as streamed
int pictureNeutral(const int units[PICTURE_VALUES]);

// The grade four values in whole units ask for, each held to its lane first
PictureGrade pictureGradeFor(const int units[PICTURE_VALUES]);

// One colour through the grade, held to 0..1 per channel on the way in: the
// contrast about mid grey and the brightness after it, held to 0..1, then the
// gamma, then the saturation toward Rec. 709 luma, held to 0..1 again
void pictureGradeApply(const PictureGrade* grade, const float in[3], float out[3]);

#endif
