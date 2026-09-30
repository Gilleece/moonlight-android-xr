// Which handle a point is over, and the stand in screen the furniture hangs
// against in a room. No GL and no context, so the host tests reach all of it.
#include <string.h>

#include "xr_layout.h"

int hoverTest(float u, float v, float width, float height, float cornerSide, int* corner) {
    if (cornerSide > 0.0f) {
        // Centred where the bracket is drawn, which is a half bracket outside
        // the corner in both axes, with the same reach each way as before
        float reachM = cornerSide * CORNER_HOVER * 0.5f;
        float cu = reachM / width;
        float cv = reachM / height;
        float outU = cornerSide * 0.5f / width;
        float outV = cornerSide * 0.5f / height;

        int left = fabsf(u + outU) < cu;
        int right = fabsf(u - (1.0f + outU)) < cu;
        int top = fabsf(v + outV) < cv;
        int bottom = fabsf(v - (1.0f + outV)) < cv;
        if ((left || right) && (top || bottom)) {
            *corner = (top ? 0 : 2) + (right ? 1 : 0);
            return HOVER_CORNER;
        }
    }

    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
        return HOVER_SCREEN;
    }

    // The move bar sits under the bottom edge, so v runs past 1 here
    float barU = BAR_WIDTH_FRAC * BAR_HOVER * 0.5f;
    float reach = (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 3.0f) * width / height;
    if (v > 1.0f && v < 1.0f + reach && fabsf(u - 0.5f) < barU) {
        return HOVER_BAR;
    }

    // Beyond the picture the ray still draws out to a margin, so it does not
    // blink out on the way to the handles underneath
    if (u > -HALO_FRAC && u < 1.0f + HALO_FRAC && v > -HALO_FRAC && v < 1.0f + HALO_FRAC) {
        return HOVER_HALO;
    }

    return HOVER_NONE;
}

// Straight ahead at eye level, facing back at the seat the way the picture
// faces the viewer, which is the pose the screen starts at outside a room
XrPosef standInPose(void) {
    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.w = 1.0f;
    pose.position.z = -STAND_IN_DISTANCE_M;
    return pose;
}
