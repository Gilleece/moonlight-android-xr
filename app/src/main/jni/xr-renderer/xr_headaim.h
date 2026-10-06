// Head aim: with the screen locked to the head, turning the head moves the
// host's mouse the way a game's look control does, and the controller's
// pointer moves it by how far the ray's point moves rather than to it, so the
// two add up. Plain arithmetic over values handed in, no OpenXR calls and no
// context, so the host tests reach all of it.

#ifndef XR_HEADAIM_H
#define XR_HEADAIM_H

#include <stdint.h>

#include "xr_math.h"
#include "xr_shared.h"

// Past this far up or down the head's yaw stops meaning anything: looking
// straight up, a small nod swings it right round. Turns read there send no
// yaw, only the pitch.
#define HEAD_AIM_PITCH_LIMIT_DEG 80.0f
// Faster than any head turns, in degrees a second. A frame that reads faster
// is the tracking jumping, and is dropped whole.
#define HEAD_AIM_JUMP_DEG_S 1500.0f
// Frames further apart than this, in seconds, are not measured across: the
// frame loop stalled or stopped, and a turn made meanwhile would land as one
// lurch
#define HEAD_AIM_GAP_SEC 0.5f

// What one frame did, for the log. Sent includes a move of under a pixel,
// which is carried to the next frame rather than lost.
#define HEAD_AIM_SENT      0
// Turning no faster than the dead zone, so nothing is sent
#define HEAD_AIM_STILL     1
// The first frame after a gap, with nothing to measure from yet
#define HEAD_AIM_SEEDED    2
// Not switched on, the screen not locked to the head, or a room up
#define HEAD_AIM_OFF       3
// A panel up, the session not focused or the splash still up
#define HEAD_AIM_PAUSED    4
// The head's orientation not tracked
#define HEAD_AIM_LOST      5
// The reference space was recentred under it
#define HEAD_AIM_RECENTRED 6
// Faster than a head turns
#define HEAD_AIM_JUMPED    7

// The switch: the setting a session starts from, turned over each time the
// bar button or the Display tab's row flips it
int headAimSwitchOn(int settingOn, int flipped);

// What the flip has to be for the switch to read wantOn
int headAimFlipFor(int settingOn, int wantOn);

// Pixels a degree and the dead zone in degrees a second, in whole units, held
// to the lanes in xr_shared.h
int headAimSensitivityClamp(int units);
int headAimDeadZoneClamp(int units);

// The head as last measured, and the pixels owed to the host that have not
// added up to a whole one yet
typedef struct {
    int seeded;
    float yaw;
    float pitch;
    int64_t timeNs;
    float carryX;
    float carryY;
} HeadAim;

void headAimReset(HeadAim* a);

// An angle in radians brought into (-pi, pi], so a turn through behind the
// head reads the short way round
float headAimWrap(float radians);

// Which way a head with this orientation faces, in radians: yaw about the
// space's up, positive to the left, in (-pi, pi], and pitch, positive looking
// up. Roll changes neither.
void headAimAngles(XrQuaternionf orientation, float* yaw, float* pitch);

// What stops a frame being measured at all, in this order: the mode not
// active, paused, the head not tracked, a recentre. HEAD_AIM_SENT where
// nothing does.
int headAimBlocked(int active, int paused, int tracked, int recentred);

// One frame. blocked is what headAimBlocked said; anything but HEAD_AIM_SENT
// drops the frame and forgets the head, so the next frame measures from
// itself rather than across the gap, and so does a frame HEAD_AIM_GAP_SEC or
// more after the last. yaw and pitch are headAimAngles' at the frame's
// display time, sensitivity is pixels per degree and deadZone degrees a
// second, which a turn's rate loses before it moves anything: a turn at the
// dead zone sends nothing and one past it eases in from there. The pixels to
// move the host's mouse by come back in outDx and outDy, right and down
// positive, and what happened as one of the HEAD_AIM_ values.
int headAimStep(HeadAim* a, int blocked, float yaw, float pitch, int64_t timeNs,
                float sensitivity, float deadZone, int* outDx, int* outDy);

// The controller's pointer while head aim is on: where its ray's point was
// last frame, on which hand, and the pixels owed, as for the head
typedef struct {
    int seeded;
    int hand;
    long frame;
    float u;
    float v;
    float carryX;
    float carryY;
} PointerNudge;

void pointerNudgeReset(PointerNudge* n);

// One frame with the ray on the picture: the hand pointing, its smoothed
// point across the picture, the input frame's number, and the stream's size
// in pixels. Moves by how far the point moved since the last frame. The first
// frame, a frame after one without the ray on the picture, and a change of
// hands only take the point, so coming back onto the picture somewhere else
// never throws the cursor across it.
void pointerNudge(PointerNudge* n, int hand, float u, float v, long frame, int width, int height,
                  int* outDx, int* outDy);

#endif
