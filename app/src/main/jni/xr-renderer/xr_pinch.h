// Where a hand's pinch is read from, the triple pinch that locks the hands, and
// how a drag the eyes started is carried by the hand that pinched. Plain
// arithmetic over numbers handed in, no OpenXR calls and no context, so the
// host tests reach all of it.

#ifndef XR_PINCH_H
#define XR_PINCH_H

#include <stdint.h>

#include "xr_math.h"

// Where a hand's pinch comes from, best first. The runtime's own pinch value,
// where a hand profile with one bound is current on that hand. Failing that,
// the pinching flag XR_FB_hand_tracking_aim gives beside the joints. Failing
// both, thumb tip to index tip measured off the joints.
#define PINCH_SRC_NONE 0
#define PINCH_SRC_VALUE 1
#define PINCH_SRC_AIM 2
#define PINCH_SRC_JOINTS 3

// The runtime's value presses at 0.65 and lets go at 0.35, the trigger's own
// pair, and the joints at 20 mm and 32 mm. A pinch is a press the frame it
// crosses, with no hold and no rule on how fast the fingers closed.
#define PINCH_VALUE_ON 0.65f
#define PINCH_VALUE_OFF 0.35f
#define PINCH_ON_M 0.020f
#define PINCH_OFF_M 0.032f

// Which source a hand reads: whether a hand profile with its pinch bound is
// current on it, whether the runtime gives the aim flags, and the joints
int pinchSource(int valueBound, int aimOffered, int jointsOffered);

// One frame of a hand's pinch off its source: the value, the aim flag or the
// gap between the tips, with tipsValid saying whether the tips were located.
// wasDown is the pinch last frame, as this said it.
int pinchStep(int source, int wasDown, float value, int aimPinching, int tipsValid, float gap);

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
