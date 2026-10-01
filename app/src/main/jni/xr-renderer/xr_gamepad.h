// Gamepad mode: the two controllers together as one Xbox pad on the host.
// What each control reads becomes Moonlight's button bits, triggers and
// sticks, a control still held from something else is kept off the pad until
// it is let go, and the left menu button held with the left grip switches the
// controllers between the pad and the pointer. Plain arithmetic over values
// handed in, no OpenXR calls and no context, so the host tests reach all of it.
//
// The right controller's menu button is the system's on both headsets and
// never reaches an app, so the switch is the left menu button with the left
// grip rather than the two menu buttons a flat app gets.

#ifndef XR_GAMEPAD_H
#define XR_GAMEPAD_H

#include <stdint.h>

#include "xr_shared.h"

// A stick at full tilt and a trigger all the way in, in the packet's units.
// The stick's is the 0x7FFE ControllerHandler scales a real pad's sticks by.
#define PAD_STICK_FULL 32766
#define PAD_TRIGGER_FULL 255
// A trigger this far in or less reads as let go: the 13 percent
// ControllerHandler gives a real pad that does not report its own
#define PAD_TRIGGER_DEADZONE 0.13f
// A grip is a bumper, down past the first and up under the second, the same
// pair the trigger clicks with as a pointer
#define PAD_GRIP_ON 0.65f
#define PAD_GRIP_OFF 0.35f
// The stick's round dead zone, a fraction of full tilt, when nothing better is
// handed down: ControllerHandler's default of 7 percent
#define PAD_STICK_DEADZONE_DEFAULT 0.07f
// How long the left menu button and the left grip are held together before
// the controllers switch between the pad and the pointer
#define PAD_TOGGLE_HOLD_NS 500000000LL

// The analogue controls among those held back, next to the button bits, which
// all sit under these
#define PAD_HELD_LT 0x10000
#define PAD_HELD_RT 0x20000
#define PAD_HELD_LS 0x40000
#define PAD_HELD_RS 0x80000

// One controller as read this frame, all zero for a hand with no controller
typedef struct {
    // 0 to 1
    float trigger;
    float grip;
    // -1 to 1, right and up positive, as OpenXR has them
    float stickX;
    float stickY;
    int stickClick;
    // The lower and upper face buttons: A and B on the right, X and Y on the
    // left
    int lower;
    int upper;
    // The menu button, which only the left controller has for an app
    int menu;
} PadHand;

// The pad as Moonlight sends it
typedef struct {
    // PAD_ bits from xr_shared.h
    int buttons;
    // 0 to 255
    int leftTrigger;
    int rightTrigger;
    // -32766 to 32766, right and up positive
    int leftX;
    int leftY;
    int rightX;
    int rightY;
} PadState;

// A grip as a bumper this frame, given whether it was down last frame
int padGripDown(float value, int was);

// A stick into the packet's units through ControllerHandler's dead zone: round,
// and inside it the stick is centred, while outside it the reading goes on as
// it is rather than rescaled, which leaves the host's own dead zone to act on
// it. Each axis is held to -1..1 first.
void padStick(float x, float y, float deadzone, int* outX, int* outY);

// A trigger's travel into 0..255, nothing at or under its dead zone
int padTrigger(float value);

// The stick dead zone from the whole percent the settings keep, at least the
// 1 percent ControllerHandler allows
float padDeadzoneFromPercent(int percent);

// The pad off both controllers. gripDown is each grip as a bumper this frame,
// left first. The left menu button is Start only while startOk says so, which
// the toggle decides.
void padMap(const PadHand* left, const PadHand* right, const int gripDown[2], int startOk,
            float stickDeadzone, PadState* out);

// Everything let go
void padRest(PadState* s);
int padAtRest(const PadState* s);
int padSame(const PadState* a, const PadState* b);

// What is held in a state: its button bits and the PAD_HELD_ bits of the
// triggers and sticks not at rest
int padHeldIn(const PadState* s);

// Keeps what held names off the pad until each is let go: anything named that
// has come to rest is dropped from held, and anything still held is put at
// rest in s. Taken when the pad comes back from resting, so a press that was
// for a panel or for the switch never lands in the game.
void padHoldBack(int* held, PadState* s);

// The switch between the pad and the pointer, the left menu button and the
// left grip held together, and whether the menu button is Start meanwhile
typedef struct {
    // Both held, and since when
    int holding;
    int64_t sinceNs;
    // Already switched during this hold, so nothing more until one is let go
    int fired;
    // The menu button has been held with the grip since it last went down, so
    // it is the switch's and not Start until it is let go
    int menuTaken;
} PadToggle;

void padToggleReset(PadToggle* t);

// One frame of the left menu button and the left grip. Returns 1 on the frame
// the two have been held together for PAD_TOGGLE_HOLD_NS, once a hold. startOk
// says whether the menu button may read as Start this frame: not once the grip
// has been held with it, until it is let go. A press made with the grip
// already down never reaches the host as Start; one made first is Start until
// the grip joins it.
int padToggleStep(PadToggle* t, int menu, int grip, int64_t nowNs, int* startOk);

#endif
