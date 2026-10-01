// Which handle a point is over, the stand in screen the furniture hangs
// against in a room, and the settings panel's tracks and presets. No GL and no
// context, so the host tests reach all of it.
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

// Rounded rather than cut, so a value that went through a float comes back
// to the unit it started as. Not held to the track: the debug property can
// ask for more than the track shows.
int separationUnits(float separation) {
    return (int)roundf(separation * 1000.0f);
}

float separationOf(int units) {
    return units * 0.001f;
}

int cogPresetAt(int units, const int presets[COG_PRESET_CELLS]) {
    static const int order[COG_PRESET_CELLS] = {
        COG_PRESET_BALANCED, COG_PRESET_COMFORT, COG_PRESET_STRONG
    };
    for (int i = 0; i < COG_PRESET_CELLS; i++) {
        if (presets[order[i]] == units) {
            return order[i];
        }
    }
    return -1;
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

float cogRowV(int tab, int row) {
    if (tab == COG_TAB_DISPLAY) {
        return COG_DISPLAY_ROW_V0 + row * COG_DISPLAY_ROW_STEP;
    }
    return COG_ROW_V0 + row * COG_ROW_STEP;
}

float cogRowHalf(int tab) {
    return tab == COG_TAB_DISPLAY ? COG_DISPLAY_ROW_HALF : COG_ROW_HALF;
}

float cogCellHalf(int tab) {
    return tab == COG_TAB_DISPLAY ? COG_DISPLAY_CELL_HALF : COG_CELL_HALF;
}

int cogTrackPart(float pu) {
    if (pu >= COG_TRACK_L - COG_CHEVRON_REACH && pu <= COG_TRACK_L + COG_CHEVRON_W) {
        return TRACK_PART_DOWN;
    }
    if (pu >= COG_TRACK_R - COG_CHEVRON_W && pu <= COG_TRACK_R + COG_CHEVRON_REACH) {
        return TRACK_PART_UP;
    }
    if (pu > COG_TRACK_L + COG_CHEVRON_W && pu < COG_TRACK_R - COG_CHEVRON_W) {
        return TRACK_PART_RUN;
    }
    return TRACK_PART_NONE;
}

float cogRunPlace(float pu) {
    float t = (pu - COG_RUN_L) / (COG_RUN_R - COG_RUN_L);
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

float cogRunU(float t) {
    return COG_RUN_L + t * (COG_RUN_R - COG_RUN_L);
}

// A thousandth of a step either way is still that step, since a place read
// back from a pose or a float is rarely exactly on one
int cogStepIndex(float t, int steps, int dir) {
    if (steps <= 0) {
        return 0;
    }
    float at = t * (float)steps;
    int step = dir > 0 ? (int)floorf(at + 0.001f) + 1 : (int)ceilf(at - 0.001f) - 1;
    return step < 0 ? 0 : (step > steps ? steps : step);
}

int cogTrackSteps(int tab, int row) {
    if (tab == COG_TAB_SCREEN) {
        // Tenths of a metre out, five centimetres up, two and a half degrees
        // of tilt and six of roll, which both step clear of the snap to level
        // either side of it, a twentieth of the curve, tenths of a metre wide
        static const int SCREEN_STEPS[COG_SLIDER_COUNT] = { 78, 80, 32, 30, 20, 72 };
        return row >= 0 && row < COG_SLIDER_COUNT ? SCREEN_STEPS[row] : 0;
    }
    if (tab == COG_TAB_DISPLAY) {
        // The glow level's five percent steps
        return row == COG_DISPLAY_SLIDER_ROW ? 20 : 0;
    }
    if (tab == COG_TAB_3D) {
        // The tenths of a percent the depth is kept in, whole percent of
        // convergence
        return row == COG_ROW3D_SEPARATION ? COG_SEP_STEPS
                : row == COG_ROW3D_CONVERGENCE ? 100 : 0;
    }
    if (tab == COG_TAB_ABOUT) {
        // No rows at all, only the button
        return 0;
    }
    if (tab == COG_TAB_PICTURE) {
        // Whole units, the ones each value is kept in and its readout shows
        static const int PICTURE_STEPS[PICTURE_VALUES] = {
            PICTURE_BRIGHTNESS_MAX - PICTURE_BRIGHTNESS_MIN,
            PICTURE_CONTRAST_MAX - PICTURE_CONTRAST_MIN,
            PICTURE_GAMMA_MAX - PICTURE_GAMMA_MIN,
            PICTURE_SATURATION_MAX - PICTURE_SATURATION_MIN
        };
        return row >= 0 && row < PICTURE_VALUES ? PICTURE_STEPS[row] : 0;
    }
    // The Room tab's lanes in five unit steps, and the size in whole percent
    if (row == COG_ROOM_ROW_BRIGHTNESS) {
        return (ROOM_BRIGHTNESS_MAX - ROOM_BRIGHTNESS_MIN) / 5;
    }
    if (row == COG_ROOM_ROW_LIGHT_LEVEL) {
        return (ROOM_LIGHT_MAX - ROOM_LIGHT_MIN) / 5;
    }
    if (row == COG_ROOM_ROW_SIZE) {
        return ROOM_SCREEN_MAX - ROOM_SCREEN_MIN;
    }
    return 0;
}

int cogReportButtonAt(float pu, float pv) {
    return pu >= COG_REPORT_L && pu <= COG_REPORT_R && pv >= COG_REPORT_T && pv <= COG_REPORT_B;
}

// The two fields across the sheet, one over the other, then the two buttons
// side by side under them. Everything else on it is words.
int reportZone(float u, float v) {
    if (v >= REPORT_BTN_T && v <= REPORT_BTN_B) {
        if (u >= REPORT_CANCEL_L && u <= REPORT_CANCEL_R) {
            return REPORT_ZONE_CANCEL;
        }
        if (u >= REPORT_SEND_L && u <= REPORT_SEND_R) {
            return REPORT_ZONE_SEND;
        }
        return REPORT_ZONE_NONE;
    }
    if (u < REPORT_FIELD_L || u > REPORT_FIELD_R) {
        return REPORT_ZONE_NONE;
    }
    if (v >= REPORT_NOTE_T && v <= REPORT_NOTE_B) {
        return REPORT_ZONE_NOTE;
    }
    if (v >= REPORT_EMAIL_T && v <= REPORT_EMAIL_B) {
        return REPORT_ZONE_EMAIL;
    }
    return REPORT_ZONE_NONE;
}
