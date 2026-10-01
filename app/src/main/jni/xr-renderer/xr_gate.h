// Who may point: what the runtime has on each hand, when a controller wakes
// and sleeps on its own clock, and when eyes that have gone missing hand the
// pointing to the hands. Plain arithmetic over numbers handed in, no OpenXR
// calls and no context, so the host tests reach all of it.

#ifndef XR_GATE_H
#define XR_GATE_H

#include "xr_math.h"

// What the runtime has bound to a hand's path. Unknown is the value before the
// first read, so the first answer of a session is logged like any other.
#define PROFILE_UNKNOWN    0
#define PROFILE_NONE       1
#define PROFILE_HANDS      2
#define PROFILE_CONTROLLER 3

// The space location bits these rules read, the values OpenXR gives them
#define GATE_ORIENTATION_VALID   0x1
#define GATE_POSITION_VALID      0x2
#define GATE_ORIENTATION_TRACKED 0x4
#define GATE_POSITION_TRACKED    0x8

// Which kind of thing is on a hand: nothing, a hand profile, or anything else
// the runtime names, which is a controller. An empty profile is not a
// controller: a runtime reports one when it drops a hand.
int profileKind(int bound, int handProfile);

// A gaze pose that can be pointed with: valid, and its orientation tracked as
// well. An eye tracker can go on reporting a valid pose after it has lost
// the eyes, and that frozen ray hovers nothing and clicks nowhere.
int gazeUsable(unsigned flags);

// A controller's aim tracked in position and orientation both. One lying in a
// lap keeps a live orientation off its gyro and never settles enough to
// retire, so orientation alone does not count.
int aimFullyTracked(unsigned flags);

// Whether a controller is the thing in the user's hand: a controller profile
// on that hand, its aim fully tracked with the action live, and awake on its
// own rest clock
int controllerInUse(int kind, int tracked, int awake);

// A controller's own rest clock, the pointer's wake and sleep rule kept per
// controller. The shared clock is held on by the eyes and by the other hand,
// so on its own it never retires a controller that was put down.
typedef struct {
    float movingFor;
    float stillFor;
    int awake;
    // The trigger that woke it, which is held back from everything until it
    // is let go: its ray was hidden, so there was nothing to aim that press at
    int wakeTrigger;
} ControllerClock;

#define CLOCK_SAME      0
#define CLOCK_PICKED_UP 1
#define CLOCK_PRESSED   2
#define CLOCK_PUT_DOWN  3

void controllerClockReset(ControllerClock* c);

// One frame of it. moved is deliberate movement this frame, pressed any
// button on that controller, holding a trigger or a thumbstick held, which
// keeps a still controller awake through a drag or a long scroll. Where
// pressWakes (the eyes are pointing) only a press wakes it, never a nudge.
// sleepOn 0 never lets it retire. triggerDown is its trigger this frame, and
// swallow comes back 1 while that is the press that woke it. Says what
// changed.
int controllerClockStep(ControllerClock* c, float dt, int moved, int pressed, int holding,
                        int triggerDown, int pressWakes, int sleepOn, float wakeSec,
                        float sleepSec, int* swallow);

// How long the eyes can be gone, while they are the pointer, before the hands
// point in their place until they come back
#define GAZE_BRIDGE_SEC 10.0f

// The hands pointing in place of eyes that have gone missing
typedef struct {
    // Since when the eyes were asked for and gave nothing, 0 while they are
    // there or not asked for
    long missingSince;
    // The last frame that asked got a usable gaze
    int back;
    int bridged;
    long bridgedSince;
} GazeBridge;

#define BRIDGE_SAME 0
#define BRIDGE_ON   1
#define BRIDGE_OFF  2

// Settled once at the top of a frame, off the last frame's gaze. gazeOn is
// the setting and the runtime together, controllerAwake a controller in use,
// focused the session, and pressHeld any press or drag still down: the bridge
// only switches between presses, so a press always ends with the source that
// started it. outSec says how long the eyes were gone, or the bridge was on.
int gazeBridgeUpdate(GazeBridge* b, int gazeOn, int controllerAwake, int focused,
                     int pressHeld, long nowNs, float bridgeSec, float* outSec);

// What this frame's locate said: whether the gaze was asked for at all, and
// whether it came back usable. Not asked is not missing, so a controller
// holding the pointer is never counted against the eyes.
void gazeBridgeTrack(GazeBridge* b, int asked, int usable, long nowNs);

#endif
