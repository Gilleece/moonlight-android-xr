// Which handle a point is over, the stand in screen the panels hang against
// in a room, where the bar's row hangs under the picture, and the settings
// panel's tracks and presets. No GL and no context, so the host tests reach
// all of it.
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

// How far the middle of the bar is from the seat on the stand in, for a
// picture of that shape
static float standInBarDistance(float aspect) {
    float drop = STAND_IN_WIDTH_M * (aspect * 0.5f + BAR_DROP_FRAC);
    return sqrtf(STAND_IN_DISTANCE_M * STAND_IN_DISTANCE_M + drop * drop);
}

// The bar's row hangs a share of the frame's width under the edge, so its
// distance, and with it the width wanted, moves a little with the width. Each
// round moves it a twentieth as far as the last, so four leave nothing over.
float barFrameWidth(Vec3 bottomMid, Vec3 down, float aspect) {
    float perMetre = STAND_IN_WIDTH_M / standInBarDistance(aspect);
    float width = perMetre * sqrtf(vecDot(bottomMid, bottomMid));
    for (int i = 0; i < 4; i++) {
        float drop = BAR_DROP_FRAC * width;
        Vec3 bar = { bottomMid.x + down.x * drop, bottomMid.y + down.y * drop,
                     bottomMid.z + down.z * drop };
        width = perMetre * sqrtf(vecDot(bar, bar));
    }
    return width;
}

BarFrame roomBarFrame(XrPosef picture, float pictureHeight, float aspect) {
    Vec3 downLocal = { 0.0f, -1.0f, 0.0f };
    Vec3 down = quatRotate(picture.orientation, downLocal);
    float half = pictureHeight * 0.5f;
    Vec3 bottomMid = { picture.position.x + down.x * half, picture.position.y + down.y * half,
                       picture.position.z + down.z * half };

    BarFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.width = barFrameWidth(bottomMid, down, aspect);
    frame.height = frame.width * aspect;
    // Up from the picture's bottom edge by half its own height, in the
    // picture's plane, so the two bottom edges are one
    float up = frame.height * 0.5f;
    frame.pose.orientation = picture.orientation;
    frame.pose.position.x = bottomMid.x - down.x * up;
    frame.pose.position.y = bottomMid.y - down.y * up;
    frame.pose.position.z = bottomMid.z - down.z * up;
    return frame;
}

// Out from the middle in the order the buttons are added, the pill between
// the first pair: the picker and the cog, then the exit and keyboard
// buttons, then gamepad mode and the ray, then head aim and the 3D
void barSlotPlacement(int slot, float width, float height, Vec3* outLocal, float* outSide) {
    static const int OUT[BAR_SLOTS] = { 0, 0, 1, 1, 2, 3, 2, 3 };
    static const int LEFT[BAR_SLOTS] = { 1, 0, 0, 1, 1, 1, 0, 0 };
    if (slot < 0 || slot >= BAR_SLOTS) {
        slot = BAR_SLOT_ENV;
    }
    float side = width * (slot == BAR_SLOT_ENV ? ENV_BUTTON_FRAC : COG_BUTTON_FRAC);
    float gap = width * ENV_GAP_FRAC;
    float x = width * BAR_WIDTH_FRAC * 0.5f + gap + side * 0.5f + OUT[slot] * (side + gap);
    outLocal->x = LEFT[slot] ? -x : x;
    outLocal->y = -(height * 0.5f + width * BAR_DROP_FRAC);
    outLocal->z = 0.005f;
    *outSide = side;
}

int barSlotHit(int slot, float u, float v, float width, float height) {
    Vec3 local;
    float side;
    barSlotPlacement(slot, width, height, &local, &side);
    float cu = 0.5f + local.x / width;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / width;
    float halfV = side * HOVER_MARGIN * 0.5f / height;
    return fabsf(u - cu) < halfU && fabsf(v - cv) < halfV;
}

_Static_assert(BTN_CELLS <= BTN_ATLAS_COLS * BTN_ATLAS_ROWS, "a cell for every button face");

int buttonCellOrigin(int cell, int* outX, int* outY) {
    if (cell < 0 || cell >= BTN_CELLS) {
        *outX = 0;
        *outY = 0;
        return 0;
    }
    *outX = (cell % BTN_ATLAS_COLS) * BUTTON_TEX;
    *outY = (cell / BTN_ATLAS_COLS) * BUTTON_TEX;
    return 1;
}

int buttonCellPut(unsigned char* atlas, int cell, const unsigned char* face) {
    int x, y;
    if (atlas == NULL || face == NULL || !buttonCellOrigin(cell, &x, &y)) {
        return 0;
    }
    const size_t row = (size_t)BUTTON_TEX * 4;
    for (int r = 0; r < BUTTON_TEX; r++) {
        unsigned char* dst = atlas + ((size_t)(y + BUTTON_TEX - 1 - r) * BTN_ATLAS_W + x) * 4;
        memcpy(dst, face + row * r, row);
    }
    return 1;
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
    if (tab == COG_TAB_SCREEN) {
        return COG_SCREEN_ROW_V0 + row * COG_SCREEN_ROW_STEP;
    }
    return COG_ROW_V0 + row * COG_ROW_STEP;
}

float cogRowHalf(int tab) {
    return tab == COG_TAB_DISPLAY ? COG_DISPLAY_ROW_HALF
            : tab == COG_TAB_SCREEN ? COG_SCREEN_ROW_HALF : COG_ROW_HALF;
}

float cogCellHalf(int tab) {
    return tab == COG_TAB_DISPLAY ? COG_DISPLAY_CELL_HALF
            : tab == COG_TAB_SCREEN ? COG_SCREEN_CELL_HALF : COG_CELL_HALF;
}

// The screen tab's eight tracks sit closer than a full size thumb, so its
// thumbs are a little smaller and two at the same place on neighbouring rows
// stay apart. The display tab has only the one.
float cogThumbSize(int tab) {
    return tab == COG_TAB_SCREEN ? 0.075f : 0.085f;
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
        // either side of it, a twentieth of the curve, tenths of a metre wide,
        // then head aim's two in their own whole units
        static const int SCREEN_STEPS[COG_SCREEN_ROW_COUNT] = {
            78, 80, 32, 30, 20, 72,
            HEAD_AIM_SENSITIVITY_MAX - HEAD_AIM_SENSITIVITY_MIN,
            HEAD_AIM_DEADZONE_MAX - HEAD_AIM_DEADZONE_MIN
        };
        return row >= 0 && row < COG_SCREEN_ROW_COUNT ? SCREEN_STEPS[row] : 0;
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

int cogKofiButtonAt(float pu, float pv) {
    return pu >= COG_KOFI_L && pu <= COG_KOFI_R && pv >= COG_KOFI_T && pv <= COG_KOFI_B;
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

// The one button, bottom right. The code and the words are nothing.
int kofiSheetZone(float u, float v) {
    return u >= KOFI_CLOSE_L && u <= KOFI_CLOSE_R && v >= KOFI_BTN_T && v <= KOFI_BTN_B
            ? KOFI_ZONE_CLOSE : KOFI_ZONE_NONE;
}

// The two buttons side by side along the bottom. The words over them are
// nothing.
int handHintZone(float u, float v) {
    if (v < HINT_BTN_T || v > HINT_BTN_B) {
        return HINT_ZONE_NONE;
    }
    if (u >= HINT_OK_L && u <= HINT_OK_R) {
        return HINT_ZONE_OK;
    }
    if (u >= HINT_NEVER_L && u <= HINT_NEVER_R) {
        return HINT_ZONE_NEVER;
    }
    return HINT_ZONE_NONE;
}
