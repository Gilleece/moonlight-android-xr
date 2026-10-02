// The pinch, the triple pinch that locks the hands and the drag the eyes
// start, as plain arithmetic, see xr_pinch.h
#include "xr_pinch.h"

#include <string.h>

int pinchSource(int valueBound, int aimOffered, int jointsOffered) {
    if (valueBound) {
        return PINCH_SRC_VALUE;
    }
    if (aimOffered) {
        return PINCH_SRC_AIM;
    }
    return jointsOffered ? PINCH_SRC_JOINTS : PINCH_SRC_NONE;
}

int pinchStep(int source, int wasDown, float value, int aimPinching, int tipsValid, float gap) {
    switch (source) {
        case PINCH_SRC_VALUE:
            return pressHysteresis(value, wasDown, PINCH_VALUE_ON, PINCH_VALUE_OFF);
        case PINCH_SRC_AIM:
            // The runtime has already judged it
            return aimPinching != 0;
        case PINCH_SRC_JOINTS:
            return tipsValid && gap < (wasDown ? PINCH_OFF_M : PINCH_ON_M);
        default:
            return 0;
    }
}

int pressHysteresis(float value, int wasDown, float on, float off) {
    return value > (wasDown ? off : on);
}

void triplePinchReset(TriplePinch* t) {
    memset(t, 0, sizeof(*t));
}

// Only presses count, so each pinch has to come up before the next can land.
// The run is measured from the press two before this one rather than from
// wherever it started, so a slow first pinch followed by three quick ones
// still turns the lock. One that turns it starts the next run afresh.
int triplePinchStep(TriplePinch* t, int down, int64_t nowNs) {
    int pressed = down && !t->wasDown;
    t->wasDown = down;
    if (!down) {
        t->holding = 0;
    }
    if (!pressed) {
        return 0;
    }
    if (t->presses == 2 && nowNs - t->pressNs[0] <= TRIPLE_PINCH_WINDOW_NS) {
        t->presses = 0;
        t->holding = 1;
        return 1;
    }
    if (t->presses == 2) {
        t->pressNs[0] = t->pressNs[1];
        t->pressNs[1] = nowNs;
    }
    else {
        t->pressNs[t->presses++] = nowNs;
    }
    return 0;
}

int triplePinchHeld(const TriplePinch* t) {
    return t->holding;
}

float dragRampGain(int64_t elapsedNs) {
    if (elapsedNs <= 0) {
        return 0.0f;
    }
    if (elapsedNs >= GAZE_DRAG_RAMP_NS) {
        return 1.0f;
    }
    float t = (float)elapsedNs / (float)GAZE_DRAG_RAMP_NS;
    return t * t * (3.0f - 2.0f * t);
}

void dragRampStart(DragRamp* r, int64_t nowNs) {
    Vec3 still = { 0.0f, 0.0f, 0.0f };
    r->prev = still;
    r->carried = still;
    r->startNs = nowNs;
}

Vec3 dragRampStep(DragRamp* r, Vec3 d, float headTurnDegS, int64_t nowNs, int* outHeld) {
    *outHeld = headTurnDegS > HEAD_TURN_HOLD_DEG_S;
    Vec3 step = vecSub(d, r->prev);
    r->prev = d;
    if (*outHeld) {
        return r->carried;
    }
    // Added up a frame at a time rather than scaling the whole travel, so
    // easing it in never leaves the content a jump to catch up on
    float gain = dragRampGain(nowNs - r->startNs) * GAZE_DRAG_RAMP_GAIN;
    r->carried.x += step.x * gain;
    r->carried.y += step.y * gain;
    r->carried.z += step.z * gain;
    return r->carried;
}

float dragScale(Vec3 head, Vec3 target, Vec3 hand) {
    Vec3 toHand = vecSub(hand, head);
    float handDist = sqrtf(vecDot(toHand, toHand));
    // A hand right at the head has no direction worth gearing by
    if (handDist < 0.05f) {
        return 1.0f;
    }
    Vec3 toTarget = vecSub(target, head);
    float scale = sqrtf(vecDot(toTarget, toTarget)) / handDist;
    if (scale < 0.5f) {
        scale = 0.5f;
    }
    if (scale > 64.0f) {
        scale = 64.0f;
    }
    return scale;
}

Vec3 dragDeadZone(Vec3 d, float dead) {
    float len = sqrtf(vecDot(d, d));
    if (len <= dead) {
        Vec3 still = { 0.0f, 0.0f, 0.0f };
        return still;
    }
    float k = (len - dead) / len;
    Vec3 out = { d.x * k, d.y * k, d.z * k };
    return out;
}

float turnRateDegS(XrQuaternionf now, XrQuaternionf was, float dt) {
    if (dt <= 0.0f) {
        return 0.0f;
    }
    // The angle between two orientations is twice the arc cosine of their dot
    float dot = fabsf(now.x * was.x + now.y * was.y + now.z * was.z + now.w * was.w);
    if (dot > 1.0f) {
        dot = 1.0f;
    }
    return 2.0f * acosf(dot) * (180.0f / 3.14159265f) / dt;
}
