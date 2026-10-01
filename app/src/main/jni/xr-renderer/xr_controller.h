// The controllers as they are drawn: whether the ray from one shows. Plain
// arithmetic over values handed in, no GL, no OpenXR calls and no context, so
// the host tests reach all of it.

#ifndef XR_CONTROLLER_H
#define XR_CONTROLLER_H

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

#endif
