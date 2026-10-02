// When a hand's pinch is a press, the triple pinch that locks the hands, and
// how a drag the eyes started is carried by the hand that pinched. Plain arithmetic over numbers handed in, no
// OpenXR calls and no context, so the host tests reach all of it.

#ifndef XR_PINCH_H
#define XR_PINCH_H

#include <stdint.h>

#include "xr_math.h"

// The press a hand's pinch makes. Off the joints the tips have to come within
// 14 mm and to have closed by 10 mm in the last 200 ms, so fingers drifting
// together as a hand relaxes never press, and the pinch lets go at 20 mm.
// Tips the runtime only estimates, valid but not tracked, take the older and
// looser 20 mm on, 32 mm off, since there is nothing trustworthy to judge the
// closing by. Either way it has to be held 80 ms before it is a press. The
// runtime's own pinch value presses at 0.9 and lets go at 0.7: it sits at 1 on
// a pinch and falls only to 0.5 to 0.7 on the release.
#define PINCH_ON_M 0.014f
#define PINCH_OFF_M 0.020f
#define PINCH_CLOSE_M 0.010f
#define PINCH_CLOSE_WINDOW_NS 200000000L
#define PINCH_LOOSE_ON_M 0.020f
#define PINCH_LOOSE_OFF_M 0.032f
#define PINCH_HOLD_NS 80000000L
#define PINCH_VALUE_ON 0.9f
#define PINCH_VALUE_OFF 0.7f

// Gaps kept per hand for the closing rule. The 200 ms window at up to 150 Hz.
#define PINCH_RING 32

typedef struct {
    float gap[PINCH_RING];
    int64_t at[PINCH_RING];
    int next;
    int count;
    // The pinch as the joints have it, before the hold
    int down;
} PinchGate;

void pinchGateReset(PinchGate* g);

// One frame of the joints: whether the thumb and index tips are valid, and
// tracked as well, and the gap between them. Says whether the joints read a
// pinch. outClosed says how far the tips had closed when an untracked frame
// is refused or a tracked one pressed, for the log.
int pinchGateStep(PinchGate* g, int valid, int tracked, float gap, int64_t nowNs,
                  float* outClosed);

// A pinch held long enough to be a press. wantSince is the hand's own clock,
// want whether the pinch is asked for this frame and wasDown whether the press
// was down last frame. Letting go is never held back.
int pinchHoldStep(int64_t* wantSince, int want, int wasDown, int64_t nowNs);

// An analog value with a gap between pressing and letting go, so a value
// sitting near one threshold does not chatter
int pressHysteresis(float value, int wasDown, float on, float off);

// The hand lock gesture: three deliberate pinches, the press each makes going
// down and coming up again, the third landing within 0.9 s of the first, on
// either hand. Read off the press whether the hands are locked or not, since
// it is the way back. The first two reach whatever they were aimed at as
// clicks, untouched and on time; the third is the lock's, held back from
// where it lands until it lets go.
#define TRIPLE_PINCH_WINDOW_NS 900000000L

typedef struct {
    // When the last two presses landed, older first, and how many of those
    // two count towards the next lock
    int64_t pressNs[2];
    int presses;
    // The press as it was last frame
    int wasDown;
    // The press that turned the lock, held back until it lets go
    int holding;
} TriplePinch;

void triplePinchReset(TriplePinch* t);

// One frame of a hand's press, down or not. Says 1 on the frame the third
// quick press lands, which turns the lock.
int triplePinchStep(TriplePinch* t, int down, int64_t nowNs);

// Whether the press this frame is the lock's: the one that turned it, from
// where it landed until it lets go
int triplePinchHeld(const TriplePinch* t);

// A drag the eyes started is carried by the hand that pinched. It comes up to
// speed over half a second from the pinch, so a pinch that wanders as it
// closes does not throw what it picked up, then runs a quarter fast to repay
// what the ramp held back, and it holds still while the head turns faster than
// 12 degrees a second: a tracker walks its estimate of a still hand as the
// headset turns, and the drag's gearing multiplies that. The hand's travel is
// still followed while it holds, so the drag picks up from where the hand is.
#define GAZE_DRAG_RAMP_NS 500000000L
#define GAZE_DRAG_RAMP_GAIN 1.25f
#define HEAD_TURN_HOLD_DEG_S 12.0f
// How far the hand travels before a slider the eyes picked moves at all, since
// the gearing turns the shake of a pinch into a slide
#define GAZE_DRAG_DEAD_M 0.015f

typedef struct {
    Vec3 prev;
    Vec3 carried;
    int64_t startNs;
} DragRamp;

// How much of the hand's motion the ramp lets through, elapsed after the
// pinch: none at it, all of it once the ramp is through, eased between
float dragRampGain(int64_t elapsedNs);
void dragRampStart(DragRamp* r, int64_t nowNs);
// What the hand has carried the drag by so far, d being its travel since the
// pinch. outHeld says whether the head held this frame's step back.
Vec3 dragRampStep(DragRamp* r, Vec3 d, float headTurnDegS, int64_t nowNs, int* outHeld);

// A hand at half a metre cannot reach a target three metres out, so the drag
// is geared by how much further the target is than the hand, from half to 64
float dragScale(Vec3 head, Vec3 target, Vec3 hand);
// Nothing until the travel passes the dead zone, measured from there on
Vec3 dragDeadZone(Vec3 d, float dead);

// How fast the head is turning, in degrees a second, from two orientations dt
// seconds apart
float turnRateDegS(XrQuaternionf now, XrQuaternionf was, float dt);

#endif
