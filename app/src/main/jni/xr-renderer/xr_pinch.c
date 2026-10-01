// The pinch and the drag the eyes start, as plain arithmetic, see xr_pinch.h
#include "xr_pinch.h"

#include <string.h>

void pinchGateReset(PinchGate* g) {
    memset(g, 0, sizeof(*g));
}

// How far the tips have closed within the window: the widest gap seen in it
// less this one
static float pinchClosing(const PinchGate* g, float gap, long nowNs) {
    long from = nowNs - PINCH_CLOSE_WINDOW_NS;
    float widest = gap;
    for (int k = 1; k <= g->count; k++) {
        int i = (g->next - k + PINCH_RING) % PINCH_RING;
        if (g->at[i] < from) {
            break;
        }
        if (g->gap[i] > widest) {
            widest = g->gap[i];
        }
    }
    return widest - gap;
}

int pinchGateStep(PinchGate* g, int valid, int tracked, float gap, long nowNs,
                  float* outClosed) {
    *outClosed = 0.0f;
    if (!valid) {
        pinchGateReset(g);
        return 0;
    }
    float closed = pinchClosing(g, gap, nowNs);
    g->gap[g->next] = gap;
    g->at[g->next] = nowNs;
    g->next = (g->next + 1) % PINCH_RING;
    if (g->count < PINCH_RING) {
        g->count++;
    }

    if (!tracked) {
        // Estimated tips: the plain distance with its old hysteresis
        g->down = gap < (g->down ? PINCH_LOOSE_OFF_M : PINCH_LOOSE_ON_M);
        return g->down;
    }
    // A pinch that is down stays down, through a drag, until the tips part.
    // One that is not needs them close and closing fast, checked every frame
    // they are inside the on distance, so a pinch that crosses it still
    // closing counts as it finishes.
    if (g->down) {
        g->down = gap < PINCH_OFF_M;
    }
    else if (gap < PINCH_ON_M) {
        g->down = closed >= PINCH_CLOSE_M;
        *outClosed = closed;
    }
    return g->down;
}

int pinchHoldStep(long* wantSince, int want, int wasDown, long nowNs) {
    if (!want) {
        *wantSince = 0;
        return 0;
    }
    if (wasDown) {
        return 1;
    }
    if (*wantSince == 0) {
        *wantSince = nowNs;
    }
    return nowNs - *wantSince >= PINCH_HOLD_NS;
}

int pressHysteresis(float value, int wasDown, float on, float off) {
    return value > (wasDown ? off : on);
}

float dragRampGain(long elapsedNs) {
    if (elapsedNs <= 0) {
        return 0.0f;
    }
    if (elapsedNs >= GAZE_DRAG_RAMP_NS) {
        return 1.0f;
    }
    float t = (float)elapsedNs / (float)GAZE_DRAG_RAMP_NS;
    return t * t * (3.0f - 2.0f * t);
}

void dragRampStart(DragRamp* r, long nowNs) {
    Vec3 still = { 0.0f, 0.0f, 0.0f };
    r->prev = still;
    r->carried = still;
    r->startNs = nowNs;
}

Vec3 dragRampStep(DragRamp* r, Vec3 d, float headTurnDegS, long nowNs, int* outHeld) {
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
