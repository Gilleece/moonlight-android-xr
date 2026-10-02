// When a hand's pinch is a press, the thumb to ring finger gesture that locks
// the hands, and how a drag the eyes started is carried by the hand that
// pinched. Plain arithmetic over numbers handed in, no
// OpenXR calls and no context, so the host tests reach all of it.

#ifndef XR_PINCH_H
#define XR_PINCH_H

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
    long at[PINCH_RING];
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
int pinchGateStep(PinchGate* g, int valid, int tracked, float gap, long nowNs,
                  float* outClosed);

// A pinch held long enough to be a press. wantSince is the hand's own clock,
// want whether the pinch is asked for this frame and wasDown whether the press
// was down last frame. Letting go is never held back.
int pinchHoldStep(long* wantSince, int want, int wasDown, long nowNs);

// An analog value with a gap between pressing and letting go, so a value
// sitting near one threshold does not chatter
int pressHysteresis(float value, int wasDown, float on, float off);

// The hand lock gesture: the thumb to the ring finger, held a moment. The ring
// tip has to be the nearest of the four fingertips to the thumb and within
// 22 mm of it (it lets go at 32), and the index tip at least 12 mm further off
// than the ring tip, which is what keeps an index pinch from reading as one.
// The middle tip is left free: on a real hand it curls in beside the ring
// finger, well inside the 35 mm clearance this once asked of it. Nothing the
// runtime says about a pinch or a grip is consulted, since it reports both
// while the fingers curl for this very gesture.
#define RING_PINCH_ON_M 0.022f
#define RING_PINCH_OFF_M 0.032f
#define RING_INDEX_MARGIN_M 0.012f
#define RING_HOLD_NS 350000000L

// The four fingertips, in the order their gaps to the thumb tip travel in
#define TIP_INDEX  0
#define TIP_MIDDLE 1
#define TIP_RING   2
#define TIP_LITTLE 3
#define TIP_COUNT  4

// What kept the gesture from being held this frame, RING_OK while it is
#define RING_OK          0
#define RING_UNTRACKED   1
#define RING_FAR         2
#define RING_NOT_NEAREST 3
#define RING_INDEX       4
// Fired on this closing already, waiting for the fingers to part
#define RING_SPENT       5
#define RING_REASONS     6

typedef struct {
    // Since when the gesture has been held cleanly, 0 while it is not
    long since;
    // The thumb and ring tips together, with their own hysteresis
    int closed;
    // Fired on this closing already, so it waits for the fingers to part
    int fired;
} RingGate;

void ringGateReset(RingGate* g);

// One frame of it. tracked says the thumb and ring tips are seen and the other
// three at least placed, and gaps are each fingertip to the thumb tip in TIP_
// order. Says 1 on the frame the hold completes, and outWhy what kept it from
// being held this frame, a RING_ reason.
int ringGateStep(RingGate* g, int tracked, const float gaps[TIP_COUNT], long nowNs, int* outWhy);

// The fingertip nearest the thumb, the ring tip on a tie. A gap under zero is a
// tip the runtime could not place, and is passed over.
int ringNearestTip(const float gaps[TIP_COUNT]);

// How long the gesture has been held so far, 0 while it is not
long ringHoldNs(const RingGate* g, long nowNs);

// A RING_ reason in words, for the log
const char* ringReasonName(int why);

// The gesture's diagnostic line, for a hand whose fingertips come near the
// thumb: due when any tip is within 40 mm of it and the hand's last line was
// 250 ms ago or more, which it then marks as now
#define RING_DIAG_NEAR_M 0.040f
#define RING_DIAG_EVERY_NS 250000000L
int ringDiagDue(long* lastNs, const float gaps[TIP_COUNT], long nowNs);

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
    long startNs;
} DragRamp;

// How much of the hand's motion the ramp lets through, elapsed after the
// pinch: none at it, all of it once the ramp is through, eased between
float dragRampGain(long elapsedNs);
void dragRampStart(DragRamp* r, long nowNs);
// What the hand has carried the drag by so far, d being its travel since the
// pinch. outHeld says whether the head held this frame's step back.
Vec3 dragRampStep(DragRamp* r, Vec3 d, float headTurnDegS, long nowNs, int* outHeld);

// A hand at half a metre cannot reach a target three metres out, so the drag
// is geared by how much further the target is than the hand, from half to 64
float dragScale(Vec3 head, Vec3 target, Vec3 hand);
// Nothing until the travel passes the dead zone, measured from there on
Vec3 dragDeadZone(Vec3 d, float dead);

// How fast the head is turning, in degrees a second, from two orientations dt
// seconds apart
float turnRateDegS(XrQuaternionf now, XrQuaternionf was, float dt);

#endif
