// Where a hand's pinch comes from and when it presses, the triple pinch that
// locks the hands, and the drag the eyes start, checked a frame at a time
#include "check.h"
#include "xr_pinch.h"

#define MS 1000000L
// One frame at 90 Hz
#define FRAME_NS 11111111L

static void testPinchSource(void) {
    // The runtime's value wherever a hand profile has it bound, then the aim
    // flag, then the joints, then nothing
    CHECK(pinchSource(1, 1, 1) == PINCH_SRC_VALUE);
    CHECK(pinchSource(1, 0, 0) == PINCH_SRC_VALUE);
    CHECK(pinchSource(0, 1, 1) == PINCH_SRC_AIM);
    CHECK(pinchSource(0, 0, 1) == PINCH_SRC_JOINTS);
    CHECK(pinchSource(0, 0, 0) == PINCH_SRC_NONE);
    CHECK(!pinchStep(PINCH_SRC_NONE, 0, 0, 1.0f, 1, 1, 0.0f));
}

// Steps a source a frame at a time while its input moves from one value to
// another over a time, and says on which frame the pinch first went down, or
// -1. Value and gap move together, so one helper serves both. ext picks the
// EXT profile's pair for the value.
static int firstDown(int source, int ext, float from, float to, int64_t overNs) {
    int frames = (int)(overNs / FRAME_NS);
    int down = 0;
    for (int i = 0; i <= frames; i++) {
        float x = from + (to - from) * i / frames;
        down = pinchStep(source, ext, down, x, 0, 1, x);
        if (down) {
            return i;
        }
    }
    return -1;
}

static void testPinchValue(void) {
    // The value presses at 0.65 and lets go at 0.35, the frame it crosses
    CHECK(!pinchStep(PINCH_SRC_VALUE, 0, 0, 0.64f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 0, 0, 0.66f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 0, 1, 0.36f, 0, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_VALUE, 0, 1, 0.34f, 0, 0, 0.0f));
    CHECK(PINCH_VALUE_ON == 0.65f && PINCH_VALUE_OFF == 0.35f);

    // A quick pinch, 0 to 1 in 100 ms (9 frames): down on the first frame
    // past 0.65, the sixth, with nothing held back
    CHECK(firstDown(PINCH_SRC_VALUE, 0, 0.0f, 1.0f, 100 * MS) == 6);
    // A slow one, 0 to 1 over 1.5 s, still presses, on the first frame past
    // 0.65 of its 135
    int slow = firstDown(PINCH_SRC_VALUE, 0, 0.0f, 1.0f, 1500 * MS);
    CHECK(slow == 88);
    // And one that only reaches 0.6 never does
    CHECK(firstDown(PINCH_SRC_VALUE, 0, 0.0f, 0.6f, 500 * MS) == -1);

    // The value is all a hand with it reads: the tips and the flag are ignored
    CHECK(!pinchStep(PINCH_SRC_VALUE, 0, 0, 0.1f, 1, 1, 0.001f));
}

static void testPinchAim(void) {
    // The flag is the runtime's judgement, taken as it is
    CHECK(pinchStep(PINCH_SRC_AIM, 0, 0, 0.0f, 1, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_AIM, 0, 1, 1.0f, 0, 1, 0.001f));
}

static void testPinchExtValue(void) {
    // A hand on the EXT profile presses at 0.9 and lets go at 0.7, the
    // Microsoft profile's hand at 0.65 and 0.35 as before
    CHECK(PINCH_EXT_VALUE_ON == 0.9f && PINCH_EXT_VALUE_OFF == 0.7f);
    float on = 0.0f, off = 0.0f;
    pinchValuePair(1, &on, &off);
    CHECK(on == PINCH_EXT_VALUE_ON && off == PINCH_EXT_VALUE_OFF);
    pinchValuePair(0, &on, &off);
    CHECK(on == PINCH_VALUE_ON && off == PINCH_VALUE_OFF);

    CHECK(!pinchStep(PINCH_SRC_VALUE, 1, 0, 0.89f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 1, 0, 0.91f, 0, 0, 0.0f));
    CHECK(pinchStep(PINCH_SRC_VALUE, 1, 1, 0.71f, 0, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_VALUE, 1, 1, 0.70f, 0, 0, 0.0f));

    // A pinch the way the Pico 4 Ultra reports one: 1.0 closed, and anywhere
    // from 0.47 to 0.7 open again. Each lets go on the EXT pair, where the
    // Microsoft pair would have held every one of them down.
    const float opened[] = { 0.47f, 0.55f, 0.62f, 0.7f };
    for (int i = 0; i < 4; i++) {
        int down = pinchStep(PINCH_SRC_VALUE, 1, 0, opened[i], 0, 0, 0.0f);
        CHECK(!down);
        down = pinchStep(PINCH_SRC_VALUE, 1, down, 1.0f, 0, 0, 0.0f);
        CHECK(down);
        down = pinchStep(PINCH_SRC_VALUE, 1, down, opened[i], 0, 0, 0.0f);
        CHECK(!down);
        CHECK(pinchStep(PINCH_SRC_VALUE, 0, 1, opened[i], 0, 0, 0.0f));
    }

    // A quick pinch, 0 to 1 in 100 ms, presses on its last frame, the first
    // past 0.9
    CHECK(firstDown(PINCH_SRC_VALUE, 1, 0.0f, 1.0f, 100 * MS) == 9);

    // The pair is the value's alone: the aim flag and the joints ignore it
    CHECK(pinchStep(PINCH_SRC_AIM, 1, 0, 0.0f, 1, 0, 0.0f));
    CHECK(!pinchStep(PINCH_SRC_AIM, 1, 1, 1.0f, 0, 1, 0.001f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 1, 0, 0.0f, 0, 1, 0.019f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 1, 1, 0.0f, 0, 1, 0.031f));
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 1, 1, 0.0f, 0, 1, 0.033f));
}

static void testPinchJoints(void) {
    // The tips press inside 20 mm and let go past 32
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 0, 0, 0.0f, 0, 1, 0.021f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 0, 0, 0.0f, 0, 1, 0.019f));
    CHECK(pinchStep(PINCH_SRC_JOINTS, 0, 1, 0.0f, 0, 1, 0.031f));
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 0, 1, 0.0f, 0, 1, 0.033f));
    CHECK(PINCH_ON_M == 0.020f && PINCH_OFF_M == 0.032f);
    // Tips not located are no pinch, held or not
    CHECK(!pinchStep(PINCH_SRC_JOINTS, 0, 1, 0.0f, 0, 0, 0.005f));

    // Fingers closing slowly, 60 to 5 mm over a second, press on the first
    // frame inside 20 mm; a hand that turns up already closed presses at once
    CHECK(firstDown(PINCH_SRC_JOINTS, 0, 0.060f, 0.005f, 1000 * MS) == 66);
    CHECK(pinchStep(PINCH_SRC_JOINTS, 0, 0, 0.0f, 0, 1, 0.006f));
}

// What a run of pinches does as the input pass applies it: each pinch is its
// press landing at a time and held for a time, stepped a frame at a time on
// from *now. Counts how often the lock turned and how many presses reached
// the host as clicks, the lock holding every press back while it is on and
// the gesture holding back its own third.
typedef struct {
    int turned;
    int clicks;
} PinchRun;

static PinchRun pinches(TriplePinch* tp, int64_t* now, int* locked, const int* atMs, int count,
                        int holdMs) {
    PinchRun run = { 0, 0 };
    int64_t from = *now;
    int64_t end = from + (int64_t)(atMs[count - 1] + holdMs + 300) * MS;
    int reached = 0;
    for (int64_t t = from; t <= end; t += FRAME_NS) {
        int down = 0;
        for (int i = 0; i < count; i++) {
            int64_t on = from + (int64_t)atMs[i] * MS;
            down |= t >= on && t < on + (int64_t)holdMs * MS;
        }
        if (triplePinchStep(tp, down, t)) {
            *locked = !*locked;
            run.turned++;
        }
        int reaches = down && !*locked && !triplePinchHeld(tp);
        run.clicks += reaches && !reached;
        reached = reaches;
        *now = t;
    }
    *now += 2000 * MS;
    return run;
}

static void testTriplePinch(void) {
    TriplePinch tp;
    triplePinchReset(&tp);
    int64_t now = 1000 * MS;
    int locked = 0;

    // Three pinches inside 0.8 s lock the hands. The first two reach the
    // host as clicks, on time; the third is the lock's.
    const int quick[3] = { 0, 300, 600 };
    PinchRun run = pinches(&tp, &now, &locked, quick, 3, 120);
    CHECK(run.turned == 1 && locked);
    CHECK(run.clicks == 2);

    // While locked the same three unlock them, and nothing reaches the host:
    // the lock holds the first two, the gesture the third
    run = pinches(&tp, &now, &locked, quick, 3, 120);
    CHECK(run.turned == 1 && !locked);
    CHECK(run.clicks == 0);

    // Three spread over 1.5 s are three clicks and nothing more
    const int slow[3] = { 0, 650, 1300 };
    run = pinches(&tp, &now, &locked, slow, 3, 120);
    CHECK(run.turned == 0 && !locked);
    CHECK(run.clicks == 3);

    // A double pinch is a double click
    const int twice[2] = { 0, 250 };
    run = pinches(&tp, &now, &locked, twice, 2, 100);
    CHECK(run.turned == 0 && !locked);
    CHECK(run.clicks == 2);

    // A slow pinch then three quick ones still locks, counted from the press
    // two before the last rather than from the first of the run
    const int late[4] = { 0, 1000, 1300, 1600 };
    run = pinches(&tp, &now, &locked, late, 4, 120);
    CHECK(run.turned == 1 && locked);
    CHECK(run.clicks == 3);

    // Six quick ones turn it twice, the fourth starting a run of its own
    const int six[6] = { 0, 250, 500, 750, 1000, 1250 };
    run = pinches(&tp, &now, &locked, six, 6, 100);
    CHECK(run.turned == 2 && locked);
    // Locked to begin with: the first three unlock, the next two click, the
    // sixth locks again
    CHECK(run.clicks == 2);
    locked = 0;
    triplePinchReset(&tp);

    // A press held down counts once however long it is held
    const int held[3] = { 0, 100, 200 };
    run = pinches(&tp, &now, &locked, held, 3, 400);
    CHECK(run.turned == 0 && run.clicks == 1);

    // The window, press to press: the third within 0.9 s of the first turns
    // it, a millisecond later does not
    triplePinchReset(&tp);
    int64_t t0 = now;
    CHECK(!triplePinchStep(&tp, 1, t0));
    CHECK(!triplePinchStep(&tp, 0, t0 + 50 * MS));
    CHECK(!triplePinchStep(&tp, 1, t0 + 400 * MS));
    CHECK(!triplePinchStep(&tp, 0, t0 + 450 * MS));
    CHECK(triplePinchStep(&tp, 1, t0 + TRIPLE_PINCH_WINDOW_NS));
    // Held back for as long as it is down, and not a moment after
    CHECK(triplePinchHeld(&tp));
    CHECK(!triplePinchStep(&tp, 1, t0 + 1000 * MS));
    CHECK(triplePinchHeld(&tp));
    CHECK(!triplePinchStep(&tp, 0, t0 + 1100 * MS));
    CHECK(!triplePinchHeld(&tp));

    triplePinchReset(&tp);
    t0 += 5000 * MS;
    CHECK(!triplePinchStep(&tp, 1, t0));
    CHECK(!triplePinchStep(&tp, 0, t0 + 50 * MS));
    CHECK(!triplePinchStep(&tp, 1, t0 + 400 * MS));
    CHECK(!triplePinchStep(&tp, 0, t0 + 450 * MS));
    CHECK(!triplePinchStep(&tp, 1, t0 + TRIPLE_PINCH_WINDOW_NS + MS));
    CHECK(!triplePinchHeld(&tp));
    CHECK(TRIPLE_PINCH_WINDOW_NS == 900 * MS);
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
    testPinchSource();
    testPinchValue();
    testPinchAim();
    testPinchExtValue();
    testPinchJoints();
    testTriplePinch();
    testDragRamp();
    return checksDone("xr_pinch");
}
