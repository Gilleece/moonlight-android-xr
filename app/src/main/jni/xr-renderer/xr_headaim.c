// Head aim, see xr_headaim.h
#include "xr_headaim.h"

#define HEAD_AIM_PI 3.14159265358979f
#define HEAD_AIM_DEG (180.0f / HEAD_AIM_PI)

int headAimSwitchOn(int settingOn, int flipped) {
    return (settingOn != 0) != (flipped != 0);
}

int headAimFlipFor(int settingOn, int wantOn) {
    return (settingOn != 0) != (wantOn != 0);
}

static int clampUnits(int units, int min, int max) {
    return units < min ? min : (units > max ? max : units);
}

int headAimSensitivityClamp(int units) {
    return clampUnits(units, HEAD_AIM_SENSITIVITY_MIN, HEAD_AIM_SENSITIVITY_MAX);
}

int headAimDeadZoneClamp(int units) {
    return clampUnits(units, HEAD_AIM_DEADZONE_MIN, HEAD_AIM_DEADZONE_MAX);
}

void headAimReset(HeadAim* a) {
    a->seeded = 0;
    a->yaw = 0.0f;
    a->pitch = 0.0f;
    a->timeNs = 0;
    a->carryX = 0.0f;
    a->carryY = 0.0f;
}

float headAimWrap(float radians) {
    const float turn = 2.0f * HEAD_AIM_PI;
    float wrapped = fmodf(radians, turn);
    if (wrapped > HEAD_AIM_PI) {
        wrapped -= turn;
    }
    else if (wrapped <= -HEAD_AIM_PI) {
        wrapped += turn;
    }
    return wrapped;
}

void headAimAngles(XrQuaternionf orientation, float* yaw, float* pitch) {
    Vec3 ahead = { 0.0f, 0.0f, -1.0f };
    Vec3 f = quatRotate(orientation, ahead);
    *yaw = atan2f(-f.x, -f.z);
    *pitch = atan2f(f.y, sqrtf(f.x * f.x + f.z * f.z));
}

int headAimBlocked(int active, int paused, int tracked, int recentred) {
    if (!active) {
        return HEAD_AIM_OFF;
    }
    if (paused) {
        return HEAD_AIM_PAUSED;
    }
    if (!tracked) {
        return HEAD_AIM_LOST;
    }
    if (recentred) {
        return HEAD_AIM_RECENTRED;
    }
    return HEAD_AIM_SENT;
}

// Whole pixels out of what is owed, to the nearest, leaving the rest owed. To
// the nearest rather than toward zero, so a turn that adds up to a whole
// number of pixels sends exactly that many.
static int payOut(float* carry) {
    float whole = roundf(*carry);
    *carry -= whole;
    return (int)whole;
}

int headAimStep(HeadAim* a, int blocked, float yaw, float pitch, int64_t timeNs,
                float sensitivity, float deadZone, int* outDx, int* outDy) {
    *outDx = 0;
    *outDy = 0;
    if (blocked != HEAD_AIM_SENT) {
        headAimReset(a);
        return blocked;
    }
    float dt = (float)(timeNs - a->timeNs) * 1e-9f;
    if (!a->seeded || dt >= HEAD_AIM_GAP_SEC) {
        a->seeded = 1;
        a->yaw = yaw;
        a->pitch = pitch;
        a->timeNs = timeNs;
        a->carryX = 0.0f;
        a->carryY = 0.0f;
        return HEAD_AIM_SEEDED;
    }
    // The same instant again has nothing to measure, and the head is still
    // measured from where it was
    if (dt <= 0.0f) {
        return HEAD_AIM_STILL;
    }

    float yawDeg = headAimWrap(yaw - a->yaw) * HEAD_AIM_DEG;
    float pitchDeg = (pitch - a->pitch) * HEAD_AIM_DEG;
    float limit = HEAD_AIM_PITCH_LIMIT_DEG / HEAD_AIM_DEG;
    if (fabsf(pitch) > limit || fabsf(a->pitch) > limit) {
        yawDeg = 0.0f;
    }
    // The next frame measures from this one whatever happens to this one, so
    // a head drifting under the dead zone never builds up into a move
    a->yaw = yaw;
    a->pitch = pitch;
    a->timeNs = timeNs;

    float speed = sqrtf(yawDeg * yawDeg + pitchDeg * pitchDeg) / dt;
    if (speed > HEAD_AIM_JUMP_DEG_S) {
        a->carryX = 0.0f;
        a->carryY = 0.0f;
        return HEAD_AIM_JUMPED;
    }
    if (speed < deadZone) {
        return HEAD_AIM_STILL;
    }
    // Turning right lowers the yaw and should move the mouse right, and
    // looking up raises the pitch and should move it up the screen
    a->carryX -= yawDeg * sensitivity;
    a->carryY -= pitchDeg * sensitivity;
    *outDx = payOut(&a->carryX);
    *outDy = payOut(&a->carryY);
    return HEAD_AIM_SENT;
}

void pointerNudgeReset(PointerNudge* n) {
    n->seeded = 0;
    n->hand = -1;
    n->frame = 0;
    n->u = 0.0f;
    n->v = 0.0f;
    n->carryX = 0.0f;
    n->carryY = 0.0f;
}

void pointerNudge(PointerNudge* n, int hand, float u, float v, long frame, int width, int height,
                  int* outDx, int* outDy) {
    *outDx = 0;
    *outDy = 0;
    int fresh = !n->seeded || n->hand != hand || frame != n->frame + 1;
    if (fresh) {
        n->carryX = 0.0f;
        n->carryY = 0.0f;
    }
    else {
        n->carryX += (u - n->u) * (float)width;
        n->carryY += (v - n->v) * (float)height;
        *outDx = payOut(&n->carryX);
        *outDy = payOut(&n->carryY);
    }
    n->seeded = 1;
    n->hand = hand;
    n->frame = frame;
    n->u = u;
    n->v = v;
}
