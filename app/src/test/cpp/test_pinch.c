// The pinch and its hold, and the drag the eyes start, checked a frame at a
// time
#include "check.h"
#include "xr_pinch.h"

#define MS 1000000L
// One frame at 90 Hz
#define FRAME_NS 11111111L

// Closes the tips from one gap to another over a time, a frame at a time, and
// says whether the joints read a pinch at the end
static int closeTips(PinchGate* g, long* t, float from, float to, long overNs, int tracked) {
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
    long t = 5000 * MS;
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
    long since = 0;
    long t = 100 * MS;
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

static void testDragRamp(void) {
    CHECK_NEAR(dragRampGain(0), 0.0, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS / 2), 0.5, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS), 1.0, 1e-6);
    CHECK_NEAR(dragRampGain(GAZE_DRAG_RAMP_NS * 4), 1.0, 1e-6);
    CHECK(dragRampGain(GAZE_DRAG_RAMP_NS / 4) < 0.25f);

    // A hand moving steadily: little gets through in the first frames and
    // the full gain once the ramp is over
    DragRamp r;
    long t = 0;
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
    testDragRamp();
    return checksDone("xr_pinch");
}
