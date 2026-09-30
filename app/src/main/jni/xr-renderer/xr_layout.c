// Which handle a point is over, and the stand in screen the furniture hangs
// against in a room. No GL and no context, so the host tests reach all of it.
#include <string.h>

#include "xr_layout.h"
#include "xr_shared.h"

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

// Snapped to whole units, so the thumb shows exactly what gets written when
// the drag ends, and never off either end
int laneUnits(float t, int min, int max) {
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return min + (int)roundf(t * (float)(max - min));
}

float lanePlace(int units, int min, int max) {
    if (max <= min) {
        return 0.0f;
    }
    float t = (float)(units - min) / (float)(max - min);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// The readout is where the thumb sits rather than the value itself: a
// brightness running past what the room was baked at reading over a hundred
// percent is more confusing than useful
int lanePercent(int units, int min, int max) {
    return (int)roundf(lanePlace(units, min, max) * 100.0f);
}

int roomScreenClamp(int percent, int resizable) {
    if (!resizable || percent > ROOM_SCREEN_MAX) {
        return ROOM_SCREEN_MAX;
    }
    return percent < ROOM_SCREEN_MIN ? ROOM_SCREEN_MIN : percent;
}

// Whole percent, so a drag moves in the steps the size row does and what the
// picture shows is what gets written when the hand lets go. A reach at or
// behind the centre is held a little in front of it, the way the free resize
// holds its scale.
int roomResizePercent(int startPercent, float startReach, float reach) {
    if (startReach < 0.05f) {
        startReach = 0.05f;
    }
    if (reach < 0.05f) {
        reach = 0.05f;
    }
    float wanted = (float)startPercent * reach / startReach;
    if (wanted < (float)ROOM_SCREEN_MIN) {
        return ROOM_SCREEN_MIN;
    }
    if (wanted > (float)ROOM_SCREEN_MAX) {
        return ROOM_SCREEN_MAX;
    }
    return (int)roundf(wanted);
}

float roomCornerSide(float distance) {
    return CORNER_FRAC * STAND_IN_WIDTH_M * distance / STAND_IN_DISTANCE_M;
}
