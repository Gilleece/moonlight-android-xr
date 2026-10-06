// The controllers as they are drawn: whether the ray from one shows, whether
// the bundled model of one does, and where that model goes. Plain arithmetic
// over values handed in, no GL, no OpenXR calls and no context, so the host
// tests reach all of it.

#ifndef XR_CONTROLLER_H
#define XR_CONTROLLER_H

#include "xr_math.h"

// Whether the ray is switched on for the session: the setting the session
// started from, turned over each time the bar button or the Display tab's row
// flips it
int raySwitchOn(int settingOn, int flipped);

// What the flip has to be for the switch to read wantOn
int rayFlipFor(int settingOn, int wantOn);

// Whether the beam is drawn this frame. Switched off it is not, except while
// a panel is up (the settings panel, the picker, the keyboard, the exit prompt
// or the report sheet), so those can still be aimed at, and it goes again
// when they close. The cursor dot and the hit test never ask: with the beam
// gone a lightgun game still has its dot and every press still lands.
int rayDrawn(int settingOn, int flipped, int panelOpen);

// The cursor dot keeps the size it has on the default screen, PTR_DOT_SIZE_M
// across at PTR_DOT_REF_M away, as an angle at any distance, so it reads the
// same on a 14 m wall as on a panel near the viewer. Held between a floor,
// where a near picture would make it a speck, and a ceiling.
#define PTR_DOT_SIZE_M 0.022f
#define PTR_DOT_REF_M 3.0f
#define PTR_DOT_MIN_M 0.005f
#define PTR_DOT_MAX_M 0.15f

// The dot's width in metres where the ray lands distance metres from the head
float pointerDotSize(float distance);

// Whether one hand's controller model is drawn: the setting on, not in
// passthrough, where the real controller is in view, a controller profile on
// that hand rather than tracked hands or nothing, its grip pose live, and the
// grip placed with a tracked orientation. Position only needs to be valid, so
// a controller out of the cameras' view for a moment keeps its model where
// the runtime puts it. kind is a PROFILE_ value and flags the GATE_ bits, both
// from xr_gate.h.
int controllerModelShown(int settingOn, int passthrough, int kind, int poseActive,
                         unsigned flags);

// The model's placement: the grip pose as a column major matrix, the model
// being a right hand controller authored in grip space, mirrored across the
// grip's x for the left hand. Its 3x3 is a turn and at most a mirror, so it
// carries the normals as it is.
void controllerModelMatrix(XrPosef grip, int leftHand, float* out16);

// Where a point of the model lands, the same placement worked without the
// matrix
Vec3 controllerModelPoint(XrPosef grip, int leftHand, Vec3 local);

#endif
