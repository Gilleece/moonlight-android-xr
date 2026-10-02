// The pinch, the ring finger gesture and the drag the eyes start, as plain
// arithmetic, see xr_pinch.h
#include "xr_pinch.h"

#include <string.h>

void pinchGateReset(PinchGate* g) {
    memset(g, 0, sizeof(*g));
}

// How far the tips have closed within the window: the widest gap seen in it
// less this one
static float pinchClosing(const PinchGate* g, float gap, int64_t nowNs) {
    int64_t from = nowNs - PINCH_CLOSE_WINDOW_NS;
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

int pinchGateStep(PinchGate* g, int valid, int tracked, float gap, int64_t nowNs,
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

int pinchHoldStep(int64_t* wantSince, int want, int wasDown, int64_t nowNs) {
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

void ringGateReset(RingGate* g) {
    memset(g, 0, sizeof(*g));
}

int ringNearestTip(const float gaps[TIP_COUNT]) {
    int nearest = gaps[TIP_RING] >= 0.0f ? TIP_RING : -1;
    for (int t = 0; t < TIP_COUNT; t++) {
        if (gaps[t] >= 0.0f && (nearest < 0 || gaps[t] < gaps[nearest])) {
            nearest = t;
        }
    }
    return nearest;
}

int ringGateStep(RingGate* g, int tracked, const float gaps[TIP_COUNT], int64_t nowNs,
                 int* outWhy) {
    if (!tracked) {
        ringGateReset(g);
        *outWhy = RING_UNTRACKED;
        return 0;
    }
    float ring = gaps[TIP_RING];
    g->closed = ring < (g->closed ? RING_PINCH_OFF_M : RING_PINCH_ON_M);
    if (!g->closed) {
        g->since = 0;
        g->fired = 0;
        *outWhy = RING_FAR;
        return 0;
    }
    if (g->fired) {
        *outWhy = RING_SPENT;
        return 0;
    }
    int why = RING_OK;
    if (ringNearestTip(gaps) != TIP_RING) {
        why = RING_NOT_NEAREST;
    }
    else if (gaps[TIP_INDEX] - ring < RING_INDEX_MARGIN_M) {
        why = RING_INDEX;
    }
    *outWhy = why;
    if (why != RING_OK) {
        // The hold has to be clean from end to end
        g->since = 0;
        return 0;
    }
    if (g->since == 0) {
        g->since = nowNs;
    }
    if (nowNs - g->since >= RING_HOLD_NS) {
        g->fired = 1;
        return 1;
    }
    return 0;
}

int64_t ringHoldNs(const RingGate* g, int64_t nowNs) {
    return g->since != 0 && !g->fired ? nowNs - g->since : 0;
}

const char* ringReasonName(int why) {
    static const char* const NAMES[RING_REASONS] = {
        "none",
        "the tips are not tracked",
        "the ring tip is not close enough to the thumb",
        "another tip is nearer the thumb than the ring tip",
        "the index tip is not 12 mm further from the thumb than the ring tip",
        "fired, waiting for the fingers to part"
    };
    return why >= 0 && why < RING_REASONS ? NAMES[why] : "unknown";
}

int ringDiagDue(int64_t* lastNs, const float gaps[TIP_COUNT], int64_t nowNs) {
    int anyNear = 0;
    for (int t = 0; t < TIP_COUNT; t++) {
        if (gaps[t] >= 0.0f && gaps[t] < RING_DIAG_NEAR_M) {
            anyNear = 1;
        }
    }
    if (!anyNear || (*lastNs != 0 && nowNs - *lastNs < RING_DIAG_EVERY_NS)) {
        return 0;
    }
    *lastNs = nowNs;
    return 1;
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
