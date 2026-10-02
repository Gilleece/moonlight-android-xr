// The pinch and its hold, the ring finger lock gesture, and the drag the eyes
// start, checked a frame at a time
#include "check.h"
#include "xr_pinch.h"

#define MS 1000000L
// One frame at 90 Hz
#define FRAME_NS 11111111L

// Closes the tips from one gap to another over a time, a frame at a time, and
// says whether the joints read a pinch at the end
static int closeTips(PinchGate* g, int64_t* t, float from, float to, int64_t overNs,
                     int tracked) {
    int frames = (int)(overNs / FRAME_NS);
    if (frames < 1) {
        frames = 1;
    }
    float closed = 0.0f;
    int down = 0;
    for (int i = 1; i <= frames; i++) {
        *t += FRAME_NS;
        float gap = from + (to - from) * i / frames;
        down = pinchGateStep(g, 1, tracked, gap, *t, &closed);
    }
    return down;
}

static void testPinchGate(void) {
    PinchGate g;
    int64_t t = 5000 * MS;
    float closed;

    // A deliberate pinch, 60 mm to 8 mm in 120 ms, presses
    pinchGateReset(&g);
    CHECK(closeTips(&g, &t, 0.060f, 0.008f, 120 * MS, 1));
    // And stays down while the tips open to under 20 mm, through a drag
    CHECK(closeTips(&g, &t, 0.008f, 0.019f, 200 * MS, 1));
    // Then lets go
    CHECK(!closeTips(&g, &t, 0.019f, 0.021f, 20 * MS, 1));

    // Fingers drifting together over a second never press, even well inside
    // the on distance
    pinchGateReset(&g);
    CHECK(!closeTips(&g, &t, 0.030f, 0.010f, 1000 * MS, 1));
    CHECK(!closeTips(&g, &t, 0.010f, 0.005f, 500 * MS, 1));

    // Inside 20 mm is no longer close enough: the tips have to reach 14
    pinchGateReset(&g);
    CHECK(!closeTips(&g, &t, 0.060f, 0.016f, 100 * MS, 1));
    // Closing on from there inside the window still counts as it finishes
    CHECK(closeTips(&g, &t, 0.016f, 0.012f, 30 * MS, 1));

    // A hand that turns up already pinched has no closing to show
    pinchGateReset(&g);
    t += FRAME_NS;
    CHECK(!pinchGateStep(&g, 1, 1, 0.006f, t, &closed));

    // Estimated tips take the old 20 / 32 mm rule
    pinchGateReset(&g);
    t += FRAME_NS;
    CHECK(!pinchGateStep(&g, 1, 0, 0.021f, t, &closed));
    t += FRAME_NS;
    CHECK(pinchGateStep(&g, 1, 0, 0.019f, t, &closed));
    t += FRAME_NS;
    CHECK(pinchGateStep(&g, 1, 0, 0.031f, t, &closed));
    t += FRAME_NS;
    CHECK(!pinchGateStep(&g, 1, 0, 0.033f, t, &closed));

    // Tips lost: no pinch, and the history goes with them
    t += FRAME_NS;
    CHECK(!pinchGateStep(&g, 0, 0, 0.0f, t, &closed));
    CHECK(g.count == 0);
}

static void testPinchHold(void) {
    int64_t since = 0;
    int64_t t = 100 * MS;
    // 80 ms of a wanted pinch before it is a press
    CHECK(!pinchHoldStep(&since, 1, 0, t));
    CHECK(!pinchHoldStep(&since, 1, 0, t + 79 * MS));
    CHECK(pinchHoldStep(&since, 1, 0, t + 80 * MS));
    // Down stays down while wanted, and letting go is never held
    CHECK(pinchHoldStep(&since, 1, 1, t + 300 * MS));
    CHECK(!pinchHoldStep(&since, 0, 1, t + 301 * MS));
    // A brush of the fingers, 60 ms, never presses
    CHECK(!pinchHoldStep(&since, 1, 0, t + 400 * MS));
    CHECK(!pinchHoldStep(&since, 1, 0, t + 460 * MS));
    CHECK(!pinchHoldStep(&since, 0, 0, t + 470 * MS));
    CHECK(!pinchHoldStep(&since, 1, 0, t + 480 * MS));
    CHECK(!pinchHoldStep(&since, 1, 0, t + 540 * MS));

    // The runtime's value presses at 0.9 and lets go at 0.7
    CHECK(!pressHysteresis(0.89f, 0, PINCH_VALUE_ON, PINCH_VALUE_OFF));
    CHECK(pressHysteresis(0.91f, 0, PINCH_VALUE_ON, PINCH_VALUE_OFF));
    CHECK(pressHysteresis(0.71f, 1, PINCH_VALUE_ON, PINCH_VALUE_OFF));
    CHECK(!pressHysteresis(0.69f, 1, PINCH_VALUE_ON, PINCH_VALUE_OFF));
}

// Holds the four fingertips where they are told, in mm from the thumb tip, for
// a time, and says how many times the gesture fired and why it was not held on
// the last frame
static int holdRing(RingGate* g, int64_t* t, int64_t forNs, float index, float middle,
                    float ring, float little, int* why) {
    const float gaps[TIP_COUNT] = { index * 0.001f, middle * 0.001f, ring * 0.001f,
                                    little * 0.001f };
    int fired = 0;
    for (int64_t done = 0; done < forNs; done += FRAME_NS) {
        *t += FRAME_NS;
        fired += ringGateStep(g, 1, gaps, *t, why);
    }
    return fired;
}

static void testRingGesture(void) {
    RingGate g;
    int64_t t = 0;
    int why;

    // Held cleanly for 350 ms it fires once, and only once however long it
    // is held. 340 ms of frames reach 333 ms of hold, two more pass 350.
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 340 * MS, 70, 50, 10, 40, &why) == 0);
    CHECK(why == RING_OK);
    CHECK(ringHoldNs(&g, t) > 300 * MS && ringHoldNs(&g, t) < RING_HOLD_NS);
    CHECK(holdRing(&g, &t, 20 * MS, 70, 50, 10, 40, &why) == 1);
    CHECK(holdRing(&g, &t, 2000 * MS, 70, 50, 10, 40, &why) == 0);
    CHECK(why == RING_SPENT);
    CHECK(ringHoldNs(&g, t) == 0);
    // Parting the fingers lets it fire again
    holdRing(&g, &t, 50 * MS, 70, 50, 40, 40, &why);
    CHECK(why == RING_FAR);
    CHECK(holdRing(&g, &t, 370 * MS, 70, 50, 10, 40, &why) == 1);

    // A real hand pinching thumb to ring: the middle tip curls in 20 mm from
    // the thumb, beside the ring tip, and it still passes
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 400 * MS, 45, 20, 13, 30, &why) == 1);
    // As close as the ring tip without being nearer, the same
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 400 * MS, 45, 13, 13, 30, &why) == 1);

    // An index pinch is never one: the ring tip too far off
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 1000 * MS, 8, 35, 30, 45, &why) == 0);
    CHECK(why == RING_FAR);
    // Or curled in close, but the index tip is nearer
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 1000 * MS, 8, 25, 18, 30, &why) == 0);
    CHECK(why == RING_NOT_NEAREST);
    // Or the ring tip nearest, but the index tip all but as close
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 1000 * MS, 20, 30, 12, 35, &why) == 0);
    CHECK(why == RING_INDEX);
    // A millimetre inside the margin refuses, half a millimetre past it passes
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 1000 * MS, 26, 30, 15, 35, &why) == 0);
    CHECK(why == RING_INDEX);
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 400 * MS, 27.5f, 30, 15, 35, &why) == 1);

    // A middle or little tip nearer than the ring tip is not the gesture
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 1000 * MS, 50, 9, 12, 30, &why) == 0);
    CHECK(why == RING_NOT_NEAREST);
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 1000 * MS, 50, 30, 12, 8, &why) == 0);
    CHECK(why == RING_NOT_NEAREST);

    // A refusal partway through starts the hold again
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 300 * MS, 60, 30, 12, 30, &why) == 0);
    CHECK(holdRing(&g, &t, 30 * MS, 60, 10, 12, 30, &why) == 0);
    CHECK(why == RING_NOT_NEAREST);
    CHECK(holdRing(&g, &t, 300 * MS, 60, 30, 12, 30, &why) == 0);
    CHECK(holdRing(&g, &t, 100 * MS, 60, 30, 12, 30, &why) == 1);

    // Its own hysteresis: closes under 22 mm, stays closed up to 32
    ringGateReset(&g);
    CHECK(holdRing(&g, &t, 1000 * MS, 60, 40, 23, 40, &why) == 0);
    CHECK(why == RING_FAR);
    holdRing(&g, &t, 20 * MS, 60, 40, 20, 40, &why);
    CHECK(holdRing(&g, &t, 400 * MS, 60, 40, 31, 40, &why) == 1);
    holdRing(&g, &t, 20 * MS, 60, 40, 33, 40, &why);
    CHECK(why == RING_FAR && !g.closed);

    // Tips lost resets it
    ringGateReset(&g);
    holdRing(&g, &t, 300 * MS, 70, 50, 10, 40, &why);
    t += FRAME_NS;
    const float none[TIP_COUNT] = { -1.0f, -1.0f, -1.0f, -1.0f };
    CHECK(!ringGateStep(&g, 0, none, t, &why));
    CHECK(why == RING_UNTRACKED);
    CHECK(holdRing(&g, &t, 300 * MS, 70, 50, 10, 40, &why) == 0);

    // Every reason has words for the log
    for (int r = 0; r < RING_REASONS; r++) {
        CHECK(ringReasonName(r)[0] != '\0');
    }
}

static void testRingNearest(void) {
    const float clear[TIP_COUNT] = { 0.05f, 0.04f, 0.01f, 0.03f };
    CHECK(ringNearestTip(clear) == TIP_RING);
    const float middle[TIP_COUNT] = { 0.05f, 0.009f, 0.01f, 0.03f };
    CHECK(ringNearestTip(middle) == TIP_MIDDLE);
    // A tie goes to the ring tip
    const float tie[TIP_COUNT] = { 0.05f, 0.01f, 0.01f, 0.01f };
    CHECK(ringNearestTip(tie) == TIP_RING);
    // A tip the runtime could not place is passed over
    const float unplaced[TIP_COUNT] = { -1.0f, 0.03f, 0.02f, -1.0f };
    CHECK(ringNearestTip(unplaced) == TIP_RING);
    const float noRing[TIP_COUNT] = { 0.04f, 0.03f, -1.0f, -1.0f };
    CHECK(ringNearestTip(noRing) == TIP_MIDDLE);
    const float nothing[TIP_COUNT] = { -1.0f, -1.0f, -1.0f, -1.0f };
    CHECK(ringNearestTip(nothing) < 0);
}

static void testRingDiagnostic(void) {
    int64_t last = 0;
    int64_t t = 1000 * MS;
    // Nothing near the thumb, or only tips it cannot place, says nothing
    const float farTips[TIP_COUNT] = { 0.08f, 0.07f, 0.06f, 0.05f };
    CHECK(!ringDiagDue(&last, farTips, t));
    const float unplaced[TIP_COUNT] = { -1.0f, -1.0f, -1.0f, -1.0f };
    CHECK(!ringDiagDue(&last, unplaced, t));
    CHECK(last == 0);
    // A tip inside 40 mm says so at once, then every 250 ms
    const float nearTips[TIP_COUNT] = { 0.08f, 0.039f, 0.06f, 0.05f };
    CHECK(ringDiagDue(&last, nearTips, t));
    CHECK(last == t);
    CHECK(!ringDiagDue(&last, nearTips, t + 249 * MS));
    CHECK(ringDiagDue(&last, nearTips, t + 250 * MS));
    // 40 mm itself is not near
    const float edge[TIP_COUNT] = { 0.08f, 0.04f, 0.06f, 0.05f };
    CHECK(!ringDiagDue(&last, edge, t + 900 * MS));
}

static void testDragRamp(void) {
    CHECK_NEAR(dragRampGain(0), 0.0, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS / 2), 0.5, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS), 1.0, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS * 4), 1.0, 1e-6);
    CHECK(dragRampGain(GAZE_DRAG_RAMP_NS / 4) < 0.25f);

    // A hand moving steadily: little gets through in the first frames and
    // the full gain once the ramp is over
    DragRamp r;
    int64_t t = 0;
    dragRampStart(&r, t);
    int held = 0;
    Vec3 d = { 0.0f, 0.0f, 0.0f };
    Vec3 out = d;
    for (int i = 0; i < 10; i++) {
        t += FRAME_NS;
        d.x += 0.001f;
        out = dragRampStep(&r, d, 0.0f, t, &held);
    }
    CHECK(!held);
    CHECK(out.x < 0.002f);
    for (int i = 0; i < 90; i++) {
        t += FRAME_NS;
        d.x += 0.001f;
        out = dragRampStep(&r, d, 0.0f, t, &held);
    }
    // Past the ramp each millimetre of hand is 1.25 of drag
    Vec3 before = out;
    t += FRAME_NS;
    d.x += 0.001f;
    out = dragRampStep(&r, d, 0.0f, t, &held);
    CHECK_NEAR(out.x - before.x, 0.00125, 1e-6);

    // Turning the head holds the drag, follows the hand, and picks up from
    // where the hand is with no jump
    before = out;
    for (int i = 0; i < 20; i++) {
        t += FRAME_NS;
        d.x += 0.002f;
        out = dragRampStep(&r, d, 30.0f, t, &held);
        CHECK(held);
    }
    CHECK_NEAR(out.x, before.x, 1e-6);
    t += FRAME_NS;
    d.x += 0.001f;
    out = dragRampStep(&r, d, 5.0f, t, &held);
    CHECK(!held);
    CHECK_NEAR(out.x - before.x, 0.00125, 1e-6);
    // Exactly at the threshold still moves
    t += FRAME_NS;
    out = dragRampStep(&r, d, HEAD_TURN_HOLD_DEG_S, t, &held);
    CHECK(!held);

    // Gearing: a target six times further than the hand moves six times as far
    Vec3 head = { 0.0f, 0.0f, 0.0f };
    Vec3 target = { 0.0f, 0.0f, -3.0f };
    Vec3 hand = { 0.0f, -0.3f, -0.4f };
    CHECK_NEAR(dragScale(head, target, hand), 6.0, 1e-4);
    Vec3 near = { 0.0f, 0.0f, -0.1f };
    Vec3 far = { 0.0f, 0.0f, -100.0f };
    CHECK_NEAR(dragScale(head, near, hand), 0.5, 1e-6);
    CHECK_NEAR(dragScale(head, far, hand), 64.0, 1e-6);
    Vec3 atHead = { 0.0f, 0.01f, 0.0f };
    CHECK_NEAR(dragScale(head, target, atHead), 1.0, 1e-6);

    // The dead zone
    Vec3 small = { 0.010f, 0.0f, 0.0f };
    Vec3 big = { 0.025f, 0.0f, 0.0f };
    CHECK_NEAR(dragDeadZone(small, GAZE_DRAG_DEAD_M).x, 0.0, 1e-6);
    CHECK_NEAR(dragDeadZone(big, GAZE_DRAG_DEAD_M).x, 0.010, 1e-6);

    // Head turn rate: 1 degree over a frame at 90 Hz is 90 degrees a second
    XrQuaternionf was = { 0.0f, 0.0f, 0.0f, 1.0f };
    float half = 0.5f * 3.14159265f / 180.0f;
    XrQuaternionf now = { 0.0f, sinf(half), 0.0f, cosf(half) };
    CHECK_NEAR(turnRateDegS(now, was, 1.0f / 90.0f), 90.0, 0.5);
    CHECK_NEAR(turnRateDegS(now, was, 0.0f), 0.0, 1e-6);
    // The double cover: q and -q are the same orientation
    XrQuaternionf neg = { 0.0f, -sinf(half), 0.0f, -cosf(half) };
    CHECK_NEAR(turnRateDegS(neg, was, 1.0f / 90.0f), 90.0, 0.5);
}

int main(void) {
    testPinchGate();
    testPinchHold();
    testRingGesture();
    testRingNearest();
    testRingDiagnostic();
    testDragRamp();
    return checksDone("xr_pinch");
}
