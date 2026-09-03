// Where everything around the picture sits and what the ray is over: the
// handles, the buttons under the bar, the panels, and the rows and cells
// on the settings panel.
#include "xr_renderer.h"

// Which affordance the ray is over. Corners are numbered 0 top left, 1 top
// right, 2 bottom left, 3 bottom right, and are skipped where they are not
// drawn so the ray falls through to what is behind them.
int hoverTest(float u, float v, float width, float height, int cornersLive,
                     int* corner) {
    if (cornersLive) {
        // Centred where the bracket is drawn, which is a half bracket outside
        // the corner in both axes, with the same reach each way as before
        float sideM = CORNER_FRAC * width;
        float reachM = sideM * CORNER_HOVER * 0.5f;
        float cu = reachM / width;
        float cv = reachM / height;
        float outU = sideM * 0.5f / width;
        float outV = sideM * 0.5f / height;

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

// The curve in force. The panel takes over from the preference the moment it
// is touched, and hands it back when the reset button clears it.
float effectiveCurvature(XrCtx* ctx) {
    return ctx->panelCurve >= 0.0f ? ctx->panelCurve : ctx->prefCurvature;
}

// A room places and sizes its own picture, so every row on the screen tab is
// dead while one is on. The panel shows a note in their place, and the input
// side has to agree with what is drawn.
int cogScreenLocked(XrCtx* ctx) {
    return ctx->cogTab == COG_TAB_SCREEN && roomEffective(ctx) > 0;
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
    float width = ctx->screenWidth * PICKER_WIDTH_FRAC;
    *outWidth = width;
    *outHeight = width * (float)PICKER_TEX_H / (float)PICKER_TEX_W;

    Vec3 local = { 0.0f, 0.0f, 0.06f };
    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose = ctx->screenPose;
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Button sits to the left of the move bar, at the same height
void envButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float side = ctx->screenWidth * ENV_BUTTON_FRAC;
    float barW = ctx->screenWidth * BAR_WIDTH_FRAC;
    float barH = ctx->screenWidth * BAR_HEIGHT_FRAC;
    outLocal->x = -(barW * 0.5f + ctx->screenWidth * ENV_GAP_FRAC + side * 0.5f);
    outLocal->y = -(height * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

// Whether a point on the picture is on a square button placed in the screen's
// flat local frame. Back into uv, where the button reaches a little further
// than it draws.
static int buttonHit(XrCtx* ctx, Vec3 local, float side, float u, float v, float height) {
    float cu = 0.5f + local.x / ctx->screenWidth;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / ctx->screenWidth;
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
    float side = ctx->screenWidth * COG_BUTTON_FRAC;
    float barW = ctx->screenWidth * BAR_WIDTH_FRAC;
    float barH = ctx->screenWidth * BAR_HEIGHT_FRAC;
    outLocal->x = barW * 0.5f + ctx->screenWidth * ENV_GAP_FRAC + side * 0.5f;
    outLocal->y = -(height * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + barH * 0.5f);
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
    float width = ctx->screenWidth * COG_WIDTH_FRAC;
    float height = width * (float)COG_TEX_H / (float)COG_TEX_W;
    *outWidth = width;
    *outHeight = height;

    // The button hangs below the screen, so the panel is placed off it rather
    // than off the screen. Same height the other placements are given.
    float screenHeight = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    Vec3 button;
    float side;
    cogButtonPlacement(ctx, screenHeight, &button, &side);

    Vec3 local;
    local.x = button.x;
    local.y = button.y + side * 0.5f + ctx->screenWidth * ENV_GAP_FRAC + height * 0.5f;
    local.z = 0.05f;

    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose = ctx->screenPose;
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// The keyboard button is the same button again, one place further out along
// the bar than the cog
void kbButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float side = ctx->screenWidth * COG_BUTTON_FRAC;
    float barW = ctx->screenWidth * BAR_WIDTH_FRAC;
    float barH = ctx->screenWidth * BAR_HEIGHT_FRAC;
    float gap = ctx->screenWidth * ENV_GAP_FRAC;
    outLocal->x = barW * 0.5f + gap + side * 1.5f + gap;
    outLocal->y = -(height * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + barH * 0.5f);
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
    float width = ctx->screenWidth * KB_WIDTH_FRAC;
    float height = width * (float)KB_TEX_H / (float)KB_TEX_W;
    *outWidth = width;
    *outHeight = height;

    float screenHeight = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    Vec3 local;
    local.x = 0.0f;
    // Top edge the same distance under the picture that the bar sits at
    local.y = -(screenHeight * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + height * 0.5f);
    local.z = 0.05f;

    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose = ctx->screenPose;
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
    float side = ctx->screenWidth * COG_BUTTON_FRAC;
    float barW = ctx->screenWidth * BAR_WIDTH_FRAC;
    float barH = ctx->screenWidth * BAR_HEIGHT_FRAC;
    float gap = ctx->screenWidth * ENV_GAP_FRAC;
    outLocal->x = -(barW * 0.5f + gap + side * 1.5f + gap);
    outLocal->y = -(height * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

int exitButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    exitButtonPlacement(ctx, height, &local, &side);
    return buttonHit(ctx, local, side, u, v, height);
}

// The prompt stands on the button that opened it, the way the settings panel
// stands on the cog. Frozen for as long as it is up for the same reason: the
// screen can still be dragged behind it, and the two buttons must not move out
// from under the ray on the way to a press.
XrPosef exitPromptPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = ctx->screenWidth * EXIT_WIDTH_FRAC;
    float height = width * (float)EXIT_TEX_H / (float)EXIT_TEX_W;
    *outWidth = width;
    *outHeight = height;

    float screenHeight = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    Vec3 button;
    float side;
    exitButtonPlacement(ctx, screenHeight, &button, &side);

    Vec3 local;
    local.x = button.x;
    local.y = button.y + side * 0.5f + ctx->screenWidth * ENV_GAP_FRAC + height * 0.5f;
    local.z = 0.05f;

    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose = ctx->screenPose;
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

// How many rows a tab has, whatever kind they are
int cogTabRowCount(int tab) {
    if (tab == COG_TAB_SCREEN) {
        return COG_SLIDER_COUNT;
    }
    if (tab == COG_TAB_3D) {
        return COG_ROW3D_COUNT;
    }
    // The option rows, then the glow level track under them
    return COG_DISPLAY_SLIDER_ROW + 1;
}

// Where a slider's thumb sits along its track, 0 at the left end and 1 at the
// right. Read back from the thing the slider controls rather than stored, so
// dragging the screen about cannot leave the panel disagreeing with it.
float cogSliderValue(XrCtx* ctx, int tab, int slider) {
    XrVector3f p = ctx->screenPose.position;
    float t = 0.0f;

    if (tab == COG_TAB_DISPLAY) {
        // Only one row on this tab has a thumb, so which one it is does not
        // need asking
        t = ctx->ambiIntensity;
    }
    else if (tab == COG_TAB_3D) {
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

// Applies a point on the track to whatever the row controls
void cogApplySlider(XrCtx* ctx, int tab, int slider, float pu) {
    float t = (pu - COG_TRACK_L) / (COG_TRACK_R - COG_TRACK_L);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    if (tab == COG_TAB_DISPLAY) {
        // Five percent steps, so the thumb shows exactly what the preference
        // will be written with when the drag ends
        int units = (int)roundf(t * 20.0f) * 5;
        ctx->ambiIntensity = units / 100.0f;
        return;
    }

    if (tab == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            // Snapped to the units the preference is stored in, so what the
            // thumb shows is exactly what gets written when the drag ends
            int units = (int)roundf(t * COG_SEP_STEPS);
            ctx->panelSeparation = units * 0.001f;
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
    if (option == COG_OPTION_HEAD_LOCK) {
        return COG_HEAD_LOCK_CELLS;
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
    if (option == COG_OPTION_HEAD_LOCK) {
        return headLocked ? 1 : 0;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        return ctx->ambilightOn ? 1 : 0;
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
    if (option == COG_OPTION_AMBILIGHT) {
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

// Letting go of a slider, either on purpose or because focus went away mid
// drag. Persisting where it ended up rather than every frame on the way there
// is the same policy a grab uses, so this is where the writing happens.
void cogDragEnded(XrCtx* ctx, float* out) {
    int slider = ctx->cogDragSlider;
    int tab = ctx->cogTab;
    ctx->cogDragSlider = -1;
    ctx->cogDragHand = -1;

    if (slider < 0) {
        return;
    }
    if (tab == COG_TAB_SCREEN) {
        // The placement is saved from the pose the frame hands back
        ctx->poseDirty = 1;
    }
    else if (tab == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            out[IN_SETTING] = (float)SETTING_SEPARATION;
            // Tenths of a percent of frame width, the preference's units
            out[IN_SETTING_VALUE] = roundf(ctx->separationCurrent * 1000.0f);
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            out[IN_SETTING] = (float)SETTING_CONVERGENCE;
            out[IN_SETTING_VALUE] = roundf(ctx->convergence * 100.0f);
        }
    }
    else if (tab == COG_TAB_DISPLAY) {
        out[IN_SETTING] = (float)SETTING_AMBI_LEVEL;
        // Whole percent, the preference's units
        out[IN_SETTING_VALUE] = roundf(ctx->ambiIntensity * 100.0f);
    }
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

// Padlock sits clear of the left edge, halfway up, in the screen's flat local
// frame. Where the picture is curved the draw puts this on the surface, and
// the arc length that comes out of it is the same x, so the hit test below
// still reads straight off these numbers.
void lockButtonPlacement(XrCtx* ctx, Vec3* outLocal, float* outSide) {
    float side = ctx->screenWidth * LOCK_BUTTON_FRAC;
    outLocal->x = -(ctx->screenWidth * (0.5f + LOCK_GAP_FRAC) + side * 0.5f);
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
