// Who may point, the controllers' own clocks and the bridge from missing eyes
// to the hands, checked a frame at a time
#include "check.h"
#include "xr_gate.h"

#include <string.h>

#define MS 1000000L
// One frame at 90 Hz
#define FRAME_NS 11111111L
#define FRAME_S 0.0111111f

static void testWhoCounts(void) {
    CHECK(profileKind(0, 0) == PROFILE_NONE);
    CHECK(profileKind(0, 1) == PROFILE_NONE);
    CHECK(profileKind(1, 1) == PROFILE_HANDS);
    CHECK(profileKind(1, 0) == PROFILE_CONTROLLER);

    // Valid alone is not enough for the eyes, nor orientation alone for a
    // controller
    unsigned valid = GATE_POSITION_VALID | GATE_ORIENTATION_VALID;
    CHECK(!gazeUsable(valid));
    CHECK(gazeUsable(valid | GATE_ORIENTATION_TRACKED));
    CHECK(!gazeUsable(GATE_ORIENTATION_VALID | GATE_ORIENTATION_TRACKED));
    CHECK(!aimFullyTracked(valid | GATE_ORIENTATION_TRACKED));
    CHECK(!aimFullyTracked(valid | GATE_POSITION_TRACKED));
    CHECK(aimFullyTracked(valid | GATE_ORIENTATION_TRACKED | GATE_POSITION_TRACKED));

    // A hand whose tracking dropped reads as no profile, and that is never a
    // controller in use however its clock stands
    CHECK(!controllerInUse(PROFILE_NONE, 1, 1));
    CHECK(!controllerInUse(PROFILE_HANDS, 1, 1));
    CHECK(!controllerInUse(PROFILE_CONTROLLER, 0, 1));
    CHECK(!controllerInUse(PROFILE_CONTROLLER, 1, 0));
    CHECK(controllerInUse(PROFILE_CONTROLLER, 1, 1));
}

// Runs a clock for a number of frames with the same inputs, and says the last
// event that was not CLOCK_SAME
static int runClock(ControllerClock* c, int frames, int moved, int pressed, int holding,
                    int trigger, int pressWakes, int sleepOn, int* swallow) {
    int last = CLOCK_SAME;
    for (int i = 0; i < frames; i++) {
        int e = controllerClockStep(c, FRAME_S, moved, pressed, holding, trigger, pressWakes,
                                    sleepOn, 0.5f, 5.0f, swallow);
        if (e != CLOCK_SAME) {
            last = e;
        }
    }
    return last;
}

static void testControllerClock(void) {
    ControllerClock c;
    int swallow = 0;

    // Without eyes, half a second of deliberate movement wakes it and five
    // still seconds put it down
    controllerClockReset(&c);
    CHECK(runClock(&c, 30, 1, 0, 0, 0, 0, 1, &swallow) == CLOCK_SAME && !c.awake);
    CHECK(runClock(&c, 20, 1, 0, 0, 0, 0, 1, &swallow) == CLOCK_PICKED_UP && c.awake);
    CHECK(runClock(&c, 440, 0, 0, 0, 0, 0, 1, &swallow) == CLOCK_SAME && c.awake);
    CHECK(runClock(&c, 20, 0, 0, 0, 0, 0, 1, &swallow) == CLOCK_PUT_DOWN && !c.awake);

    // While the eyes point a nudge never wakes it, however long it goes on
    controllerClockReset(&c);
    CHECK(runClock(&c, 300, 1, 0, 0, 0, 1, 1, &swallow) == CLOCK_SAME && !c.awake);
    // A press does, at once, and the trigger that did it is held back until it
    // is let go
    CHECK(runClock(&c, 1, 0, 1, 1, 1, 1, 1, &swallow) == CLOCK_PRESSED && c.awake);
    CHECK(swallow);
    runClock(&c, 10, 0, 1, 1, 1, 1, 1, &swallow);
    CHECK(swallow);
    runClock(&c, 1, 0, 0, 0, 0, 1, 1, &swallow);
    CHECK(!swallow);
    // The next press is a press
    runClock(&c, 1, 0, 1, 1, 1, 1, 1, &swallow);
    CHECK(!swallow);
    // A grip or a button that wakes it leaves the trigger alone
    controllerClockReset(&c);
    CHECK(runClock(&c, 1, 0, 1, 0, 0, 1, 1, &swallow) == CLOCK_PRESSED && !swallow);

    // A held trigger or thumbstick keeps a still controller awake well past
    // the sleep
    controllerClockReset(&c);
    runClock(&c, 50, 1, 0, 0, 0, 0, 1, &swallow);
    CHECK(c.awake);
    CHECK(runClock(&c, 900, 0, 0, 1, 1, 0, 1, &swallow) == CLOCK_SAME && c.awake);
    CHECK(runClock(&c, 460, 0, 0, 0, 0, 0, 1, &swallow) == CLOCK_PUT_DOWN);

    // With the sleep switched off it never retires
    controllerClockReset(&c);
    runClock(&c, 50, 1, 0, 0, 0, 0, 0, &swallow);
    CHECK(c.awake);
    CHECK(runClock(&c, 9000, 0, 0, 0, 0, 0, 0, &swallow) == CLOCK_SAME && c.awake);
}

static void testGazeBridge(void) {
    GazeBridge b;
    memset(&b, 0, sizeof(b));
    float sec = 0.0f;
    long t = 1000 * MS;

    // Eyes there: nothing happens
    gazeBridgeTrack(&b, 1, 1, t);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_SAME);

    // Gone for just under ten seconds: still the eyes
    t += FRAME_NS;
    gazeBridgeTrack(&b, 1, 0, t);
    long lost = t;
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, lost + 9900 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    // A controller in use, an unfocused session or a press held each keep it
    // back
    CHECK(gazeBridgeUpdate(&b, 1, 1, 1, 0, lost + 10100 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 0, 0, lost + 10100 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 1, lost + 10100 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    // Then the hands
    t = lost + 10100 * MS;
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_ON);
    CHECK(b.bridged);
    CHECK_NEAR(sec, 10.1, 0.01);

    // The eyes coming back during a pinch wait for it to end
    t += FRAME_NS;
    gazeBridgeTrack(&b, 1, 1, t);
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 1, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_SAME);
    CHECK(b.bridged);
    t += 2000 * MS;
    CHECK(gazeBridgeUpdate(&b, 1, 0, 1, 0, t, GAZE_BRIDGE_SEC, &sec) == BRIDGE_OFF);
    CHECK(!b.bridged);
    CHECK_NEAR(sec, 2.011, 0.01);

    // Not asking is not missing: a controller holding the pointer for a minute
    // never counts against the eyes
    gazeBridgeTrack(&b, 0, 0, t);
    CHECK(gazeBridgeUpdate(&b, 1, 1, 1, 0, t + 60000 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_SAME);
    CHECK(b.missingSince == 0);

    // Switching gaze off while bridged drops it
    gazeBridgeTrack(&b, 1, 0, t);
    gazeBridgeUpdate(&b, 1, 0, 1, 0, t + 11000 * MS, GAZE_BRIDGE_SEC, &sec);
    CHECK(b.bridged);
    CHECK(gazeBridgeUpdate(&b, 0, 0, 1, 0, t + 12000 * MS, GAZE_BRIDGE_SEC, &sec)
          == BRIDGE_OFF);
    CHECK(!b.bridged && b.missingSince == 0);
}

int main(void) {
    testWhoCounts();
    testControllerClock();
    testGazeBridge();
    return checksDone("xr_gate");
}
