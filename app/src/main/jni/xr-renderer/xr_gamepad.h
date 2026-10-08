// Gamepad mode: the two controllers together as one Xbox pad on the host.
// What each control reads becomes Moonlight's button bits, triggers and
// sticks, a control still held from something else is kept off the pad until
// it is let go, and a shortcut on the controllers switches them between the
// pad and the pointer. Plain arithmetic over values handed in, no OpenXR calls
// and no context, so the host tests reach all of it.
//
// The right controller's menu button is the system's on both headsets and
// never reaches an app, so the shortcut is the left menu button with the left
// grip rather than the two menu buttons a flat app gets, or by choice both
// stick clicks, or both triggers with both grips. Only the one chosen does
// anything.

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
    // A Steam Frame controller, laid out like an Xbox pad: A, B, X and Y and
    // Start (menu) on the right, the d-pad and Back (view) on the left, and a
    // bumper on each. On the left, lower and upper are the d-pad's down and
    // right, the two the pointer clicks with, and menu is the view button.
    int frame;
    int faceX;
    int faceY;
    int dpadUp;
    int dpadLeft;
    int bumper;
    int start;
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

// The menu and grip shortcut between the pad and the pointer, the left menu
// button and the left grip held together, and whether the menu button is
// Start meanwhile
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

// The other two shortcuts are chords of two or four controls, the parts, each
// a bit: a stick click, a trigger or a grip, left or right
#define PAD_PART_LS 0x01
#define PAD_PART_RS 0x02
#define PAD_PART_LT 0x04
#define PAD_PART_RT 0x08
#define PAD_PART_LG 0x10
#define PAD_PART_RG 0x20
#define PAD_PART_COUNT 6
// The rest of a chord has to be pressed this soon after its first part leaves
// rest, and all of it then held this long before it switches
#define PAD_CHORD_WINDOW_NS 150000000LL
#define PAD_CHORD_HOLD_NS 300000000LL

// What a chord step did, for the log
#define PAD_CHORD_NOTHING 0
#define PAD_CHORD_FORMING 1
#define PAD_CHORD_ABANDONED 2
#define PAD_CHORD_FIRED 3

// A chord is all its parts pressed within the window of the first leaving
// rest, from all of them at rest, and then held. Nothing of it may reach the
// host or the pointer meanwhile, so each part is held back, read as at rest,
// from the moment it leaves rest until the chord either switches or is given
// up: the window running out, or a part let go or eased off before the hold
// is over. Given up, what is still held goes through from that frame on, a
// little late, and a part pressed and let go inside it is replayed as a tap,
// one frame at the furthest it went. Once it switches, its parts stay held
// back until each is let go, and nothing more happens until all are at rest.
// A part already going through when the others join cannot start one.
typedef struct {
    // The chord's parts, none for the menu and grip shortcut or without a
    // controller in each hand
    int parts;
    // Last frame: off rest, and pressed far enough to count for the chord
    int stirred;
    int pressed;
    // A chord may be forming: since when, and whether every part has been
    // pressed and since when
    int forming;
    int64_t startNs;
    int allDown;
    int64_t allNs;
    // Held back while it forms, and how far each went
    int pending;
    float peak[PAD_PART_COUNT];
    // Spent on a switch and held back until let go, and the wait for every
    // part to be at rest before another can form
    int taken;
    int spent;
    // This frame: the parts read as at rest, and those replayed as a tap
    int held;
    int replay;
} PadChord;

// The parts of a shortcut, none for the menu and grip one
int padChordParts(int shortcut);

// Starts over with these parts. Whatever is down now has to be let go before
// a chord can form, so a reset never switches by itself.
void padChordReset(PadChord* c, int parts);

// One frame of both controllers as read. A part leaves rest past a trigger's
// dead zone, a grip's bumper press or a stick click, and is pressed for the
// chord at the bumper's press, held down to its release. Returns a
// PAD_CHORD_ value; FIRED is the frame to switch.
int padChordStep(PadChord* c, const PadHand* left, const PadHand* right, int64_t nowNs);

// A part's reading as the pad and the pointer take it this frame: 0 held back,
// its peak replayed, or the reading as it is
float padChordSeen(const PadChord* c, int part, float raw);

// Both controllers as the pad takes them: each part through padChordSeen, and
// a stick centred too while its click is held back, so pressing it in to make
// the chord does not leak as a nudge
void padChordApply(const PadChord* c, const PadHand* left, const PadHand* right,
                   PadHand* outLeft, PadHand* outRight);

// The host's rumble on the controllers: the low frequency motor on the left,
// the high on the right. The host only says when it changes, so each
// controller is given a pulse that outlasts a frame and armed again before it
// runs out for as long as the host wants it. A frame loop that stops leaves
// at most one pulse running.
#define PAD_RUMBLE_PULSE_NS 100000000LL
#define PAD_RUMBLE_REARM_NS 50000000LL

// What a frame asks of each controller's haptics
#define PAD_RUMBLE_KEEP 0
#define PAD_RUMBLE_APPLY 1
#define PAD_RUMBLE_STOP 2

typedef struct {
    // What the host asks for, left then right, 0 to 1, and whether it has
    // asked since the last frame
    float want[2];
    int fresh;
    // What each controller was last given and when, 0 once stopped
    float given[2];
    int64_t givenNs[2];
} PadRumble;

// A motor's level as the host sends it, 0 to 65535 (a Java short is taken as
// unsigned), as an amplitude from 0 to 1
float padRumbleAmplitude(int motor);

// Nothing asked for, nothing running
void padRumbleReset(PadRumble* r);

// The host's latest, the two motors as they came. The same values again still
// arm a pulse at once.
void padRumbleAsk(PadRumble* r, int lowMotor, int highMotor);

// One frame. live says, left then right, whether that controller is the pad's
// now; one that is not is stopped if it was running. Each action is a
// PAD_RUMBLE_ value and amp the amplitude to apply with.
void padRumbleStep(PadRumble* r, const int live[2], int64_t nowNs, int action[2], float amp[2]);

#endif
