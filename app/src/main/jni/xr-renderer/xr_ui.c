// Where everything around the picture sits and what the ray is over: the
// handles, the buttons under the bar, the panels, and the rows and cells
// on the settings panel.
#include "xr_renderer.h"

// Whether the furniture hangs against the stand in screen rather than the
// picture, which it does whenever a room is up. The buttons along the bar, the
// padlock, the picker, the settings panel, the keyboard and the exit prompt are
// all placed and sized off it, and their hit tests are made on it too.
int furnitureOnStandIn(XrCtx* ctx) {
    return roomEffective(ctx) > 0;
}

// Where that screen is: the picture's own pose outside a room, and the stand
// in's inside one
XrPosef furniturePose(XrCtx* ctx) {
    return furnitureOnStandIn(ctx) ? standInPose() : ctx->screenPose;
}

float furnitureWidth(XrCtx* ctx) {
    return furnitureOnStandIn(ctx) ? STAND_IN_WIDTH_M : ctx->screenWidth;
}

// The same shape as the picture, so the furniture keeps the proportions it has
// outside a room at the same size
float furnitureHeight(XrCtx* ctx) {
    return furnitureWidth(ctx) * (float)ctx->videoHeight / (float)ctx->videoWidth;
}

// How big the corner brackets are, in metres, and 0 where there are none. A
// room hangs its own picture, so only a room that lets it be resized has
// corners, and they are sized off the stand in rather than the picture, so
// they look the same wherever the room hangs it and however small it is.
float cornerSide(XrCtx* ctx) {
    int style = roomEffective(ctx);
    if (style > 0) {
        if (!roomResizable(style)) {
            return 0.0f;
        }
        XrVector3f p = ctx->screenPose.position;
        return roomCornerSide(sqrtf(p.x * p.x + p.y * p.y + p.z * p.z));
    }
    return CORNER_FRAC * ctx->screenWidth;
}

// The curve in force. The panel takes over from the preference the moment it
// is touched, and hands it back when the reset button clears it.
float effectiveCurvature(XrCtx* ctx) {
    return ctx->panelCurve >= 0.0f ? ctx->panelCurve : ctx->prefCurvature;
}

// A room places and sizes its own picture, so every row on the screen tab is
// dead while one is on, and the first tab is the Room tab instead. Which rows
// the panel shows, which the input side has to agree with.
int cogFace(XrCtx* ctx) {
    return ctx->cogTab == COG_TAB_SCREEN && roomEffective(ctx) > 0 ? COG_FACE_ROOM
                                                                    : ctx->cogTab;
}

// Which sheet shows them. In a room every tab is drawn with the Room tab's
// name over the first slot, and the Room tab itself greys its size row where
// the room will not have the picture resized.
int cogArt(XrCtx* ctx) {
    int style = roomEffective(ctx);
    if (style <= 0) {
        return ctx->cogTab;
    }
    if (ctx->cogTab == COG_TAB_DISPLAY) {
        return COG_ART_ROOM_DISPLAY;
    }
    if (ctx->cogTab == COG_TAB_3D) {
        return COG_ART_ROOM_3D;
    }
    return roomResizable(style) ? COG_ART_ROOM : COG_ART_ROOM_FIXED;
}

// The room the Room tab's rows belong to, or 0 with none up. Only ever a
// style the per room values can be read at.
static int roomFaceStyle(XrCtx* ctx) {
    int style = roomEffective(ctx);
    return style >= ROOM_STYLE_FIRST && style <= ROOM_STYLE_LAST ? style : 0;
}

// How far the screen's face is tipped up or down, in radians. Positive is
// looking up at it.
float screenPitch(XrCtx* ctx) {
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
    return atan2f(fwd.y, sqrtf(fwd.x * fwd.x + fwd.z * fwd.z));
}

// The three angles the screen is described by, built back into an orientation:
// yaw about world up, then pitch, then roll about the screen's own forward
// axis. Roll goes innermost on purpose. A turn about the forward axis leaves
// that axis where it is, so the pitch and yaw read back untouched however far
// the picture is rolled, which is what lets a drag hold one while recomputing
// the other. Pitch is negated for the same reason the tilt slider negates it:
// turning by +theta about local x takes the face downward.
XrQuaternionf screenOrient(float yaw, float pitch, float roll) {
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 fwd = { 0.0f, 0.0f, 1.0f };
    XrQuaternionf q = quatMul(axisAngleQuat(up, yaw), axisAngleQuat(right, -pitch));
    return quatNorm(quatMul(q, axisAngleQuat(fwd, roll)));
}

// How far the screen is twisted about the axis it faces along, in radians.
// Positive raises its right edge. Measured by undoing the yaw and pitch rather
// than by reading how high the right edge sits, since those two only agree
// while the screen is level, and this one comes back out of screenOrient
// exactly at any pitch.
float screenRoll(XrCtx* ctx) {
    XrQuaternionf q = ctx->screenPose.orientation;
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(q, back);
    float yaw = atan2f(fwd.x, fwd.z);
    XrQuaternionf level = screenOrient(yaw, screenPitch(ctx), 0.0f);
    XrQuaternionf twist = quatMul(quatConj(level), q);
    // A quaternion and its negation are the same rotation, and with the screen
    // turned to face behind the viewer the rebuilt yaw lands on the other one.
    // Without this the answer comes back a full turn out.
    if (twist.w < 0.0f) {
        twist.z = -twist.z;
        twist.w = -twist.w;
    }
    return 2.0f * atan2f(twist.z, twist.w);
}

// The picker floats just in front of the screen, centred on it
XrPosef pickerPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = furnitureWidth(ctx) * PICKER_WIDTH_FRAC;
    *outWidth = width;
    *outHeight = width * (float)PICKER_TEX_H / (float)PICKER_TEX_W;

    XrPosef pose = furniturePose(ctx);
    Vec3 local = { 0.0f, 0.0f, 0.06f };
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Button sits to the left of the move bar, at the same height. This and the
// placements after it are in the furniture's own flat frame, and height is
// that frame's.
void envButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float width = furnitureWidth(ctx);
    float side = width * ENV_BUTTON_FRAC;
    float barW = width * BAR_WIDTH_FRAC;
    float barH = width * BAR_HEIGHT_FRAC;
    outLocal->x = -(barW * 0.5f + width * ENV_GAP_FRAC + side * 0.5f);
    outLocal->y = -(height * 0.5f + width * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

// Whether a point on the furniture's frame is on a square button placed in
// it. Back into uv, where the button reaches a little further than it draws.
static int buttonHit(XrCtx* ctx, Vec3 local, float side, float u, float v, float height) {
    float width = furnitureWidth(ctx);
    float cu = 0.5f + local.x / width;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / width;
    float halfV = side * HOVER_MARGIN * 0.5f / height;
    return fabsf(u - cu) < halfU && fabsf(v - cv) < halfV;
}

int envButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    envButtonPlacement(ctx, height, &local, &side);
    return buttonHit(ctx, local, side, u, v, height);
}

// The cog is the same button on the other side of the bar
void cogButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float width = furnitureWidth(ctx);
    float side = width * COG_BUTTON_FRAC;
    float barW = width * BAR_WIDTH_FRAC;
    float barH = width * BAR_HEIGHT_FRAC;
    outLocal->x = barW * 0.5f + width * ENV_GAP_FRAC + side * 0.5f;
    outLocal->y = -(height * 0.5f + width * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

int cogButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    cogButtonPlacement(ctx, height, &local, &side);
    return buttonHit(ctx, local, side, u, v, height);
}

// The settings panel stands on top of the cog button that opens it, so it
// reads as belonging to that button and leaves the picture clear. The caller
// freezes what this returns for as long as the panel is open: the distance
// slider moves the screen, and a panel that followed it would drag the thumb
// out from under the ray halfway through a drag.
XrPosef cogPanelPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float frameWidth = furnitureWidth(ctx);
    float width = frameWidth * COG_WIDTH_FRAC;
    float height = width * (float)COG_TEX_H / (float)COG_TEX_W;
    *outWidth = width;
    *outHeight = height;

    // The button hangs below the screen, so the panel is placed off it rather
    // than off the screen. Same height the other placements are given.
    Vec3 button;
    float side;
    cogButtonPlacement(ctx, furnitureHeight(ctx), &button, &side);

    Vec3 local;
    local.x = button.x;
    local.y = button.y + side * 0.5f + frameWidth * ENV_GAP_FRAC + height * 0.5f;
    local.z = 0.05f;

    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// The keyboard button is the same button again, one place further out along
// the bar than the cog
void kbButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float width = furnitureWidth(ctx);
    float side = width * COG_BUTTON_FRAC;
    float barW = width * BAR_WIDTH_FRAC;
    float barH = width * BAR_HEIGHT_FRAC;
    float gap = width * ENV_GAP_FRAC;
    outLocal->x = barW * 0.5f + gap + side * 1.5f + gap;
    outLocal->y = -(height * 0.5f + width * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

int kbButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    kbButtonPlacement(ctx, height, &local, &side);
    return buttonHit(ctx, local, side, u, v, height);
}

// The keyboard hangs under the screen, centred on it, in the band the move bar
// lives in. Wider than the settings panel and squarer, so it wants the middle
// rather than a corner. Frozen while it is open, like the settings panel: the
// screen stays draggable behind it and the keys must not move under the ray.
XrPosef kbPanelPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float frameWidth = furnitureWidth(ctx);
    float width = frameWidth * KB_WIDTH_FRAC;
    float height = width * (float)KB_TEX_H / (float)KB_TEX_W;
    *outWidth = width;
    *outHeight = height;

    Vec3 local;
    local.x = 0.0f;
    // Top edge the same distance under the picture that the bar sits at
    local.y = -(furnitureHeight(ctx) * 0.5f + frameWidth * BAR_GAP_FRAC + height * 0.5f);
    local.z = 0.05f;

    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Which key a point on the panel is inside, or -1. The rectangles are the
// whole of what this side knows about the layout, so a row of them is all
// there is to search.
int kbKeyAt(XrCtx* ctx, float u, float v) {
    int found = -1;
    for (int i = 0; i < ctx->kbKeyCount; i++) {
        const float* r = &ctx->kbKeyRects[i * 4];
        if (u >= r[0] && u <= r[2] && v >= r[1] && v <= r[3]) {
            found = i;
        }
    }
    return found;
}

// The exit button is the left hand mirror of the keyboard button: one place
// further out along the bar than the environment button, and past the left end
// of the bar's own zone
void exitButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float width = furnitureWidth(ctx);
    float side = width * COG_BUTTON_FRAC;
    float barW = width * BAR_WIDTH_FRAC;
    float barH = width * BAR_HEIGHT_FRAC;
    float gap = width * ENV_GAP_FRAC;
    outLocal->x = -(barW * 0.5f + gap + side * 1.5f + gap);
    outLocal->y = -(height * 0.5f + width * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

int exitButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    exitButtonPlacement(ctx, height, &local, &side);
    return buttonHit(ctx, local, side, u, v, height);
}

// The 3D switch is one place further out again on the right, past the
// keyboard, so a session without it loses only the last button and every
// other one stays where it always is
void stereoButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float width = furnitureWidth(ctx);
    float side = width * COG_BUTTON_FRAC;
    float barW = width * BAR_WIDTH_FRAC;
    float barH = width * BAR_HEIGHT_FRAC;
    float gap = width * ENV_GAP_FRAC;
    outLocal->x = barW * 0.5f + gap + side * 1.5f + gap + side + gap;
    outLocal->y = -(height * 0.5f + width * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

// Only where it is drawn, which is a session with stereo to switch once its
// art has arrived
int stereoButtonHit(XrCtx* ctx, float u, float v, float height) {
    if (!ctx->stereoButtonReady) {
        return 0;
    }
    Vec3 local;
    float side;
    stereoButtonPlacement(ctx, height, &local, &side);
    return buttonHit(ctx, local, side, u, v, height);
}

// The 3D on or off for the rest of the session, from the bar or the 3D tab.
// Takes effect on the next draw, which is asked for now so a picture standing
// still shows it too. Coming back on with a model running, the warp waits
// flat for a map of what is on screen now rather than picking up with the
// one left from whenever it went off.
void setStereoLive(XrCtx* ctx, int on, const char* from) {
    on = on ? 1 : 0;
    if (ctx->stereoMode == DEPTH_MODE_OFF || ctx->stereoLive == on) {
        return;
    }
    ctx->stereoLive = on;
    ctx->warpRedraw = 1;
    ctx->stereoWaiting = on && ctx->stereoMode == DEPTH_MODE_MODEL;
    ctx->stereoWaitIndex = atomic_load_explicit(&ctx->depthStagedIndex, memory_order_acquire);
    ctx->stereoWaitNs = nowNs();
    // The shift test measures again in the new state
    ctx->barTestFramesLogged = 0;
    LOGEV("3d %s from %s", on ? "on" : "off", from);
}

// The prompt stands on the button that opened it, the way the settings panel
// stands on the cog. Frozen for as long as it is up for the same reason: the
// screen can still be dragged behind it, and the two buttons must not move out
// from under the ray on the way to a press.
XrPosef exitPromptPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float frameWidth = furnitureWidth(ctx);
    float width = frameWidth * EXIT_WIDTH_FRAC;
    float height = width * (float)EXIT_TEX_H / (float)EXIT_TEX_W;
    *outWidth = width;
    *outHeight = height;

    Vec3 button;
    float side;
    exitButtonPlacement(ctx, furnitureHeight(ctx), &button, &side);

    Vec3 local;
    local.x = button.x;
    local.y = button.y + side * 0.5f + frameWidth * ENV_GAP_FRAC + height * 0.5f;
    local.z = 0.05f;

    XrPosef pose = furniturePose(ctx);
    Vec3 offset = quatRotate(pose.orientation, local);
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Which of the prompt's two buttons a point is on, in the sheet's own
// coordinates. Everything else on it is a question and a background.
int exitPromptZone(float u, float v) {
    if (v < EXIT_BTN_T || v > EXIT_BTN_B) {
        return EXIT_ZONE_NONE;
    }
    if (u >= EXIT_EXIT_L && u <= EXIT_EXIT_R) {
        return EXIT_ZONE_EXIT;
    }
    if (u >= EXIT_CANCEL_L && u <= EXIT_CANCEL_R) {
        return EXIT_ZONE_CANCEL;
    }
    return EXIT_ZONE_NONE;
}

// How many rows a face has, whatever kind they are
int cogTabRowCount(int face) {
    if (face == COG_TAB_SCREEN) {
        return COG_SLIDER_COUNT;
    }
    if (face == COG_TAB_3D) {
        return COG_ROW3D_COUNT;
    }
    if (face == COG_FACE_ROOM) {
        return COG_ROOM_ROW_COUNT;
    }
    // The option rows, then the glow level track under them
    return COG_DISPLAY_SLIDER_ROW + 1;
}

// Whether a row is a track to drag rather than a row of cells to press
int cogRowIsTrack(int face, int row) {
    if (face == COG_TAB_DISPLAY) {
        return row == COG_DISPLAY_SLIDER_ROW;
    }
    if (face == COG_FACE_ROOM) {
        return row != COG_ROOM_ROW_GLOW && row != COG_ROOM_ROW_LIGHT;
    }
    if (face == COG_TAB_3D) {
        return row == COG_ROW3D_SEPARATION || row == COG_ROW3D_CONVERGENCE;
    }
    return 1;
}

// How many cells a row of cells has, on whichever face it is
int cogRowCells(int face, int row) {
    if (face == COG_FACE_ROOM) {
        return COG_ROOM_SWITCH_CELLS;
    }
    if (face == COG_TAB_3D) {
        return row == COG_ROW3D_PRESET ? COG_PRESET_CELLS : COG_STEREO_CELLS;
    }
    return cogOptionCells(row);
}

// Whether a row can do anything here. A dead row is drawn greyed, carries no
// thumb and takes no press, so the art, the layers and the hit test all ask.
int cogRowLive(XrCtx* ctx, int face, int row) {
    // Curving needs a layer type this runtime may not have
    if (face == COG_TAB_SCREEN && row == COG_SLIDER_CURVE) {
        return ctx->cylinderSupported;
    }
    // With stereo off there is nothing for any 3D row to move or switch
    if (face == COG_TAB_3D) {
        return ctx->stereoMode != DEPTH_MODE_OFF;
    }
    // A room built around its picture keeps it at the whole of its anchor
    if (face == COG_FACE_ROOM && row == COG_ROOM_ROW_SIZE) {
        return roomResizable(roomFaceStyle(ctx));
    }
    return 1;
}

// Where a slider's thumb sits along its track, 0 at the left end and 1 at the
// right. Read back from the thing the slider controls rather than stored, so
// dragging the screen about cannot leave the panel disagreeing with it.
float cogSliderValue(XrCtx* ctx, int face, int slider) {
    XrVector3f p = ctx->screenPose.position;
    float t = 0.0f;

    if (face == COG_TAB_DISPLAY) {
        // Only one row on this tab has a thumb, so which one it is does not
        // need asking
        t = ctx->ambiIntensity;
    }
    else if (face == COG_FACE_ROOM) {
        // The room showing's own values, so the thumbs move when the picker
        // moves to another room
        int style = roomFaceStyle(ctx);
        if (style == 0) {
            return 0.0f;
        }
        if (slider == COG_ROOM_ROW_BRIGHTNESS) {
            t = lanePlace(ctx->roomBrightness[style], ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX);
        }
        else if (slider == COG_ROOM_ROW_LIGHT_LEVEL) {
            t = lanePlace(ctx->roomLightLevel[style], ROOM_LIGHT_MIN, ROOM_LIGHT_MAX);
        }
        else if (slider == COG_ROOM_ROW_SIZE) {
            // A quarter of the room's screen at the left end, all of it at the
            // right
            t = lanePlace(roomScreenPercent(ctx, style), ROOM_SCREEN_MIN, ROOM_SCREEN_MAX);
        }
    }
    else if (face == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            t = ctx->separationCurrent / COG_SEP_MAX;
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            t = ctx->convergence;
        }
    }
    else if (slider == COG_SLIDER_DISTANCE) {
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        t = (d - COG_DIST_MIN) / (COG_DIST_MAX - COG_DIST_MIN);
    }
    else if (slider == COG_SLIDER_HEIGHT) {
        t = (p.y - COG_HEIGHT_MIN) / (COG_HEIGHT_MAX - COG_HEIGHT_MIN);
    }
    else if (slider == COG_SLIDER_TILT) {
        // Level sits at the middle of the track
        t = (screenPitch(ctx) + COG_TILT_MAX) / (2.0f * COG_TILT_MAX);
    }
    else if (slider == COG_SLIDER_ROTATE) {
        // Reversed against the others: the right hand end turns the picture
        // clockwise, which lowers the right edge that screenRoll counts as
        // positive. Level sits at the middle either way.
        t = (COG_ROLL_MAX - screenRoll(ctx)) / (2.0f * COG_ROLL_MAX);
    }
    else if (slider == COG_SLIDER_CURVE) {
        t = effectiveCurvature(ctx);
    }
    else if (slider == COG_SLIDER_SIZE) {
        t = (ctx->screenWidth - SCREEN_MIN_WIDTH) / (SCREEN_MAX_WIDTH - SCREEN_MIN_WIDTH);
    }

    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Applies a point on the track to whatever the row controls. The run the
// thumb travels stops short of the track's ends, where the step buttons are.
void cogApplySlider(XrCtx* ctx, int face, int slider, float pu) {
    float t = cogRunPlace(pu);

    if (face == COG_FACE_ROOM) {
        // Whole units, the ones each preference is stored in, so the thumb
        // shows exactly what gets written when the drag ends. All three are
        // read afresh every frame, so the room changes under the thumb.
        int style = roomFaceStyle(ctx);
        if (style == 0) {
            return;
        }
        if (slider == COG_ROOM_ROW_BRIGHTNESS) {
            ctx->roomBrightness[style] = laneUnits(t, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX);
        }
        else if (slider == COG_ROOM_ROW_LIGHT_LEVEL) {
            ctx->roomLightLevel[style] = laneUnits(t, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX);
        }
        else if (slider == COG_ROOM_ROW_SIZE && roomResizable(style)) {
            // About the picture's centre, which the room keeps where it was
            ctx->roomScreen[style] = laneUnits(t, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX);
        }
        return;
    }

    if (face == COG_TAB_DISPLAY) {
        // Five percent steps, so the thumb shows exactly what the preference
        // will be written with when the drag ends
        int units = (int)roundf(t * 20.0f) * 5;
        ctx->ambiIntensity = units / 100.0f;
        return;
    }

    if (face == COG_TAB_3D) {
        // Both tracks move something only the 3D shows, so taking hold of
        // either brings it back if it was switched off
        setStereoLive(ctx, 1, "the 3D tab's tracks");
        if (slider == COG_ROW3D_SEPARATION) {
            // Snapped to the units the preference is stored in, so what the
            // thumb shows is exactly what gets written when the drag ends
            ctx->panelSeparation = separationOf(laneUnits(t, 0, COG_SEP_STEPS));
            ctx->separationCurrent = ctx->panelSeparation;
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            // Whole percent, same reason
            int units = (int)roundf(t * 100.0f);
            ctx->convergence = units / 100.0f;
        }
        return;
    }

    if (slider == COG_SLIDER_DISTANCE) {
        XrVector3f p = ctx->screenPose.position;
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        float wanted = COG_DIST_MIN + t * (COG_DIST_MAX - COG_DIST_MIN);
        if (d > 0.01f) {
            // Straight out along the line it already sits on, so only how far
            // away it is changes
            float scale = wanted / d;
            ctx->screenPose.position.x = p.x * scale;
            ctx->screenPose.position.y = p.y * scale;
            ctx->screenPose.position.z = p.z * scale;
            // Keeping the arc the same shape as it moves, same reason as the
            // resize path
            ctx->screenRadius *= scale;
        }
        else {
            // Sitting on top of the viewer, so there is no line to follow and
            // straight ahead is the only sensible answer
            ctx->screenPose.position.x = 0.0f;
            ctx->screenPose.position.y = 0.0f;
            ctx->screenPose.position.z = -wanted;
        }
    }
    else if (slider == COG_SLIDER_HEIGHT) {
        // Straight up and down, so raising the screen does not also bring it
        // nearer the way an arc about the viewer would
        ctx->screenPose.position.y = COG_HEIGHT_MIN + t * (COG_HEIGHT_MAX - COG_HEIGHT_MIN);
    }
    else if (slider == COG_SLIDER_TILT) {
        float target = -COG_TILT_MAX + t * (2.0f * COG_TILT_MAX);
        // Level is worth being able to land on exactly, same as the roll row
        if (fabsf(target) < COG_TILT_SNAP) {
            target = 0.0f;
        }
        // Rebuilt from the three angles rather than turned about the local x
        // axis, which stops being horizontal once the picture has been rolled
        // and would swing it round instead of tipping it. Identical result on
        // a level screen, and it lands on the target in one step.
        Vec3 back = { 0.0f, 0.0f, 1.0f };
        Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
        float yaw = atan2f(fwd.x, fwd.z);
        float roll = screenRoll(ctx);
        // Position untouched, so it tilts about its own centre rather than
        // swinging around the viewer
        ctx->screenPose.orientation = screenOrient(yaw, target, roll);
    }
    else if (slider == COG_SLIDER_ROTATE) {
        // Dragging right turns the picture clockwise as the viewer sees it,
        // the way a rotate right button does. The forward axis points out at
        // the viewer, so a right handed turn about it reads anticlockwise, and
        // the track runs from +max down to -max to match.
        float target = COG_ROLL_MAX - t * (2.0f * COG_ROLL_MAX);
        // Level is the whole point of the row and the track is far too coarse
        // to land on it by hand, so the middle of it clips to exactly zero
        if (fabsf(target) < COG_ROLL_SNAP) {
            target = 0.0f;
        }
        float delta = target - screenRoll(ctx);
        Vec3 fwd = { 0.0f, 0.0f, 1.0f };
        Vec3 axis = quatRotate(ctx->screenPose.orientation, fwd);
        XrQuaternionf turn = axisAngleQuat(axis, delta);
        // Turning about the axis it already faces along leaves the facing
        // alone, so this only rolls: the tilt and the yaw come back unchanged
        ctx->screenPose.orientation = quatNorm(quatMul(turn, ctx->screenPose.orientation));
    }
    else if (slider == COG_SLIDER_CURVE) {
        ctx->panelCurve = t;
        // The radius updatePlacement would have picked for this curve, so a
        // reseed later agrees with what is on screen now
        XrVector3f p = ctx->screenPose.position;
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        ctx->screenRadius = d * (1.0f + 3.0f * (1.0f - t));
    }
    else if (slider == COG_SLIDER_SIZE) {
        float wanted = SCREEN_MIN_WIDTH + t * (SCREEN_MAX_WIDTH - SCREEN_MIN_WIDTH);
        if (ctx->screenWidth > 0.01f) {
            // Keeping the arc the same shape rather than flattening as it
            // grows, same rule the corner resize follows
            ctx->screenRadius *= wanted / ctx->screenWidth;
        }
        // Centre and facing untouched, so it grows about the middle rather
        // than away from a corner
        ctx->screenWidth = wanted;
    }
}

// The option rows on the display tab are a row of cells rather than a track
int cogOptionCells(int option) {
    if (option == COG_OPTION_SHARPEN) {
        return COG_SHARPEN_CELLS;
    }
    if (option == COG_OPTION_SUPERSAMPLE) {
        return COG_SUPERSAMPLE_CELLS;
    }
    if (option == COG_OPTION_HEAD_LOCK) {
        return COG_HEAD_LOCK_CELLS;
    }
    if (option == COG_OPTION_POINTER_SLEEP) {
        return COG_POINTER_SLEEP_CELLS;
    }
    if (option == COG_OPTION_CLICK_SOUND) {
        return COG_CLICK_SOUND_CELLS;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        return COG_AMBI_CELLS;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        return COG_ROOM_LIGHT_CELLS;
    }
    return COG_STATS_CELLS;
}

// Which cell of a row is the one in force. Head lock is the one value this side
// does not keep: it arrives with the frame, and reading it back means the ring
// still tells the truth if something outside the panel changes it.
int cogOptionValue(XrCtx* ctx, int option, int headLocked) {
    if (option == COG_OPTION_SHARPEN) {
        return ctx->sharpenMode;
    }
    if (option == COG_OPTION_SUPERSAMPLE) {
        return ctx->supersampleMode;
    }
    if (option == COG_OPTION_HEAD_LOCK) {
        return headLocked ? 1 : 0;
    }
    if (option == COG_OPTION_POINTER_SLEEP) {
        return ctx->pointerSleepOn ? 1 : 0;
    }
    if (option == COG_OPTION_CLICK_SOUND) {
        return ctx->clickSoundOn ? 1 : 0;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        // The switch in force, which in a room is the room's own
        return roomGlowOn(ctx, roomEffective(ctx)) ? 1 : 0;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        return ctx->roomLightOn ? 1 : 0;
    }
    return ctx->overlayVisible ? 1 : 0;
}

// Takes effect here and now, and hands back the setting id so the frame can
// tell Java to store it too. Both of these are read fresh every frame by the
// code that acts on them, so there is nothing to restart.
int cogApplyOption(XrCtx* ctx, int option, int cell) {
    if (option == COG_OPTION_SHARPEN) {
        ctx->sharpenMode = cell;
        return SETTING_SHARPEN;
    }
    if (option == COG_OPTION_SUPERSAMPLE) {
        ctx->supersampleMode = cell;
        return SETTING_SUPERSAMPLE;
    }
    if (option == COG_OPTION_STATS) {
        ctx->overlayVisible = cell != 0;
        return SETTING_STATS;
    }
    if (option == COG_OPTION_HEAD_LOCK) {
        // Nothing to set here: Java writes the preference it already hands down
        // every frame, and the space is picked from that value on the next one.
        // Left live inside a room like the screen light row, since the picker
        // can drop the room at any moment. In one the wall wins and the screen
        // stays put whatever this says.
        LOGEV("head lock %s from the panel", cell != 0 ? "on" : "off");
        return SETTING_HEAD_LOCK;
    }
    if (option == COG_OPTION_POINTER_SLEEP) {
        // Set here as well as handed to Java, which hands it back down with
        // the next frame, so the ring moves on the press
        ctx->pointerSleepOn = cell != 0;
        LOGEV("pointer sleep %s from the panel", cell != 0 ? "on" : "off");
        return SETTING_POINTER_SLEEP;
    }
    if (option == COG_OPTION_CLICK_SOUND) {
        // Java plays it, and takes the switch from the setting this hands it
        ctx->clickSoundOn = cell != 0;
        LOGEV("click sound %s from the panel", cell != 0 ? "on" : "off");
        return SETTING_CLICK_SOUND;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        // In a room this is the same switch the Room tab's glow row is, so the
        // two can never disagree about what is on
        int style = roomFaceStyle(ctx);
        if (style != 0) {
            ctx->roomGlow[style] = cell != 0;
            LOGEV("room %d glow %s from the panel", style, cell != 0 ? "on" : "off");
            return SETTING_ROOM_GLOW;
        }
        ctx->ambilightOn = cell != 0;
        LOGEV("ambilight %s from the panel", ctx->ambilightOn ? "on" : "off");
        return SETTING_AMBILIGHT;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        ctx->roomLightOn = cell != 0;
        LOGEV("room light %s from the panel", ctx->roomLightOn ? "on" : "off");
        return SETTING_ROOM_LIGHT;
    }
    return -1;
}

// The Room tab's two rows of cells: the room's own glow, and the screen light,
// which is one switch for every room and the same one the display tab has
int cogRoomCellValue(XrCtx* ctx, int row) {
    if (row == COG_ROOM_ROW_GLOW) {
        return roomGlowOn(ctx, roomEffective(ctx)) ? 1 : 0;
    }
    return ctx->roomLightOn ? 1 : 0;
}

// Applied here and now, and handed to Java to store in the same frame. The
// glow is written under the room showing, which IN_SETTING_ROOM names.
void cogApplyRoomCell(XrCtx* ctx, int row, int cell, float* out) {
    int id = row == COG_ROOM_ROW_GLOW ? cogApplyOption(ctx, COG_OPTION_AMBILIGHT, cell)
                                      : cogApplyOption(ctx, COG_OPTION_ROOM_LIGHT, cell);
    out[IN_SETTING] = (float)id;
    out[IN_SETTING_VALUE] = (float)cell;
}

// Which cell of a row of cells on the Room or 3D tab is in force, or -1 where
// none is. A separation dragged to somewhere between the presets is none of
// them, so their row carries no ring.
int cogCellInForce(XrCtx* ctx, int face, int row) {
    if (face == COG_FACE_ROOM) {
        return cogRoomCellValue(ctx, row);
    }
    if (face == COG_TAB_3D && row == COG_ROW3D_PRESET) {
        return cogPresetAt(separationUnits(ctx->separationCurrent), ctx->presetUnits);
    }
    if (face == COG_TAB_3D && row == COG_ROW3D_SWITCH) {
        return ctx->stereoLive ? 1 : 0;
    }
    return -1;
}

// The switch is the bar button's, for this session only, so nothing goes to
// Java to store. A preset writes its separation the way letting go of the
// track there would, so it goes to the preference by the same road, and it
// brings the 3D back if it was off, since a strength is something to see.
static void cogApply3dCell(XrCtx* ctx, int row, int cell, float* out) {
    static const char* const PRESET_NAMES[COG_PRESET_CELLS] = {
        "Comfort", "Balanced", "Strong"
    };
    if (row == COG_ROW3D_SWITCH) {
        setStereoLive(ctx, cell != 0, "the 3D tab");
        return;
    }
    if (row != COG_ROW3D_PRESET || cell < 0 || cell >= COG_PRESET_CELLS) {
        return;
    }
    int units = ctx->presetUnits[cell];
    ctx->panelSeparation = separationOf(units);
    ctx->separationCurrent = ctx->panelSeparation;
    out[IN_SETTING] = (float)SETTING_SEPARATION;
    out[IN_SETTING_VALUE] = (float)units;
    LOGEV("3d preset %s from the panel, separation %d", PRESET_NAMES[cell], units);
    setStereoLive(ctx, 1, "a preset");
}

// A press on a cell, whichever tab it is on, applied here and now and handed
// to Java to store in the same frame
void cogApplyCell(XrCtx* ctx, int face, int row, int cell, float* out) {
    if (face == COG_FACE_ROOM) {
        cogApplyRoomCell(ctx, row, cell, out);
        return;
    }
    if (face == COG_TAB_3D) {
        cogApply3dCell(ctx, row, cell, out);
        return;
    }
    int id = cogApplyOption(ctx, row, cell);
    if (id >= 0) {
        out[IN_SETTING] = (float)id;
        out[IN_SETTING_VALUE] = (float)cell;
    }
}

// Letting go of a slider, either on purpose or because focus went away mid
// drag. Persisting where it ended up rather than every frame on the way there
// is the same policy a grab uses, so this is where the writing happens.
void cogDragEnded(XrCtx* ctx, float* out) {
    int slider = ctx->cogDragSlider;
    int face = ctx->cogDragFace;
    ctx->cogDragSlider = -1;
    ctx->cogDragHand = -1;
    ctx->cogDragFace = -1;
    ctx->cogDragByGaze = 0;

    if (slider < 0) {
        return;
    }
    if (face == COG_TAB_SCREEN) {
        // The placement is saved from the pose the frame hands back
        ctx->poseDirty = 1;
    }
    else if (face == COG_FACE_ROOM) {
        // The room's own units, written under the room showing. A room gone
        // from under the drag leaves nothing to write it to.
        int style = roomFaceStyle(ctx);
        if (style == 0) {
            return;
        }
        int setting = -1;
        int value = 0;
        const char* what = "";
        if (slider == COG_ROOM_ROW_BRIGHTNESS) {
            setting = SETTING_ROOM_BRIGHTNESS;
            value = ctx->roomBrightness[style];
            what = "brightness";
        }
        else if (slider == COG_ROOM_ROW_LIGHT_LEVEL) {
            setting = SETTING_ROOM_LIGHT_LEVEL;
            value = ctx->roomLightLevel[style];
            what = "light level";
        }
        else if (slider == COG_ROOM_ROW_SIZE) {
            setting = SETTING_ROOM_SCREEN;
            value = roomScreenPercent(ctx, style);
            what = "screen percent";
        }
        if (setting >= 0) {
            out[IN_SETTING] = (float)setting;
            out[IN_SETTING_VALUE] = (float)value;
            LOGEV("room %d %s %d from the panel", style, what, value);
        }
    }
    else if (face == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            out[IN_SETTING] = (float)SETTING_SEPARATION;
            // Tenths of a percent of frame width, the preference's units
            out[IN_SETTING_VALUE] = (float)separationUnits(ctx->separationCurrent);
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            out[IN_SETTING] = (float)SETTING_CONVERGENCE;
            out[IN_SETTING_VALUE] = roundf(ctx->convergence * 100.0f);
        }
    }
    else if (face == COG_TAB_DISPLAY) {
        out[IN_SETTING] = (float)SETTING_AMBI_LEVEL;
        // Whole percent, the preference's units
        out[IN_SETTING_VALUE] = roundf(ctx->ambiIntensity * 100.0f);
    }
}

// A press on one of a track's step buttons: one step that way, applied at once
// the way a drag is and written at once the way letting go of one is. Held
// down it does not repeat.
void cogStepTrack(XrCtx* ctx, int face, int row, int dir, float* out) {
    int steps = cogTrackSteps(face, row);
    if (steps <= 0) {
        return;
    }
    float before = cogSliderValue(ctx, face, row);
    int step = cogStepIndex(before, steps, dir);
    cogApplySlider(ctx, face, row, cogRunU((float)step / (float)steps));
    float after = cogSliderValue(ctx, face, row);
    LOGEV("track step %s: face %d row %d, step %.2f to %.2f of %d", dir > 0 ? "up" : "down",
          face, row, before * steps, after * steps, steps);
    ctx->cogDragSlider = row;
    ctx->cogDragFace = face;
    ctx->cogDragHand = -1;
    cogDragEnded(ctx, out);
}

// Which cell of a row the ray is on, or -1 off the ends
int cogCellAt(float pu, int cells) {
    if (pu < COG_TRACK_L || pu > COG_TRACK_R) {
        return -1;
    }
    int cell = (int)((pu - COG_TRACK_L) / (COG_TRACK_R - COG_TRACK_L) * cells);
    if (cell < 0) cell = 0;
    if (cell >= cells) cell = cells - 1;
    return cell;
}

// What the percents beside the Room tab's tracks should say, in IN_READOUT
// order, or -1 first while the tab is not up. The size says nothing in a room
// that keeps its picture whole, where its row is greyed.
void cogReadouts(XrCtx* ctx, int* values) {
    int style = roomFaceStyle(ctx);
    // Still up while the panel fades out, so the strip goes with it
    int showing = ctx->cogOpen || ctx->panelFades[FADE_COG].level > 0.0f;
    if (!showing || cogFace(ctx) != COG_FACE_ROOM || style == 0) {
        values[0] = -1;
        values[1] = -1;
        values[2] = -1;
        return;
    }
    values[0] = lanePercent(ctx->roomBrightness[style], ROOM_BRIGHTNESS_MIN,
                            ROOM_BRIGHTNESS_MAX);
    values[1] = lanePercent(ctx->roomLightLevel[style], ROOM_LIGHT_MIN, ROOM_LIGHT_MAX);
    values[2] = roomResizable(style) ? roomScreenPercent(ctx, style) : -1;
}

// Held on the depth track, which is all the panel can show or write
static int onSeparationTrack(int units) {
    return units < 0 ? 0 : (units > COG_SEP_STEPS ? COG_SEP_STEPS : units);
}

// The running model's own pair and the separations its presets write, all in
// the preferences' units and the presets in cell order, handed down once
// before the first frame. The 3D tab's reset goes back to the pair.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetDepthDefaults(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jint separation,
                                                                   jint convergence,
                                                                   jintArray presets) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    int units = onSeparationTrack(separation);
    int percent = convergence < 0 ? 0 : (convergence > 100 ? 100 : convergence);
    ctx->defaultSeparation = separationOf(units);
    ctx->defaultConvergence = percent / 100.0f;
    if (presets != NULL && (*env)->GetArrayLength(env, presets) >= COG_PRESET_CELLS) {
        int values[COG_PRESET_CELLS];
        (*env)->GetIntArrayRegion(env, presets, 0, COG_PRESET_CELLS, values);
        for (int i = 0; i < COG_PRESET_CELLS; i++) {
            ctx->presetUnits[i] = onSeparationTrack(values[i]);
        }
    }
    LOGEV("3d defaults: separation %d, convergence %d, presets %d %d %d", units, percent,
          ctx->presetUnits[COG_PRESET_COMFORT], ctx->presetUnits[COG_PRESET_BALANCED],
          ctx->presetUnits[COG_PRESET_STRONG]);
}

// Padlock sits clear of the left edge, halfway up, in the furniture's flat
// local frame. Where the picture is curved the draw puts this on the surface,
// and the arc length that comes out of it is the same x, so the hit test below
// still reads straight off these numbers.
void lockButtonPlacement(XrCtx* ctx, Vec3* outLocal, float* outSide) {
    float width = furnitureWidth(ctx);
    float side = width * LOCK_BUTTON_FRAC;
    outLocal->x = -(width * (0.5f + LOCK_GAP_FRAC) + side * 0.5f);
    outLocal->y = 0.0f;
    outLocal->z = 0.005f;
    *outSide = side;
}

int lockButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    lockButtonPlacement(ctx, &local, &side);
    return buttonHit(ctx, local, side, u, v, height);
}

// Whether a press ticks, which only the display tab's ring reads on this
// side. Handed down before the first frame.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetClickSound(JNIEnv* env, jobject thiz,
                                                                jlong handle, jboolean on) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx != NULL) {
        ctx->clickSoundOn = on;
    }
}
