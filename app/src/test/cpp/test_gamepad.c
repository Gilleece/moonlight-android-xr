// Gamepad mode: the controllers' readings into Moonlight's pad, the dead
// zones, the grips as bumpers, the controls held back until let go, and the
// left menu button with the left grip switching modes after half a second
#include <string.h>

#include "check.h"
#include "xr_gamepad.h"

// 72 Hz, the display rate the slowest headset here runs at
#define FRAME_NS 13888889LL

static PadHand idle(void) {
    PadHand h;
    memset(&h, 0, sizeof(h));
    return h;
}

// The pad off two hands with both grips up and Start allowed, at the default
// dead zone
static PadState mapped(const PadHand* left, const PadHand* right) {
    int grips[2] = { 0, 0 };
    PadState s;
    padMap(left, right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    return s;
}

static void testTheBits(void) {
    // The packet's own flags, which ControllerPacket names the same
    CHECK(PAD_A == 0x1000 && PAD_B == 0x2000 && PAD_X == 0x4000 && PAD_Y == 0x8000);
    CHECK(PAD_LB == 0x0100 && PAD_RB == 0x0200);
    CHECK(PAD_LS_CLICK == 0x0040 && PAD_RS_CLICK == 0x0080);
    CHECK(PAD_START == 0x0010);
    // Nine separate bits, and the analogue ones held back sit clear of them
    CHECK(PAD_BUTTONS == (PAD_A | PAD_B | PAD_X | PAD_Y | PAD_LB | PAD_RB | PAD_LS_CLICK
                          | PAD_RS_CLICK | PAD_START));
    CHECK(PAD_BUTTONS == 0xf3d0);
    int analogue = PAD_HELD_LT | PAD_HELD_RT | PAD_HELD_LS | PAD_HELD_RS;
    CHECK((analogue & PAD_BUTTONS) == 0);
    CHECK((analogue & 0xffff) == 0);
}

static void testTheButtons(void) {
    PadHand left = idle();
    PadHand right = idle();
    CHECK(mapped(&left, &right).buttons == 0);

    // The right controller's A and B are the pad's, the left's X and Y theirs
    right.lower = 1;
    CHECK(mapped(&left, &right).buttons == PAD_A);
    right.lower = 0;
    right.upper = 1;
    CHECK(mapped(&left, &right).buttons == PAD_B);
    right.upper = 0;
    left.lower = 1;
    CHECK(mapped(&left, &right).buttons == PAD_X);
    left.lower = 0;
    left.upper = 1;
    CHECK(mapped(&left, &right).buttons == PAD_Y);
    left.upper = 0;

    // Stick clicks, each on its own side
    left.stickClick = 1;
    CHECK(mapped(&left, &right).buttons == PAD_LS_CLICK);
    left.stickClick = 0;
    right.stickClick = 1;
    CHECK(mapped(&left, &right).buttons == PAD_RS_CLICK);
    right.stickClick = 0;

    // The menu button is Start, unless the toggle has it
    left.menu = 1;
    CHECK(mapped(&left, &right).buttons == PAD_START);
    int grips[2] = { 0, 0 };
    PadState s;
    padMap(&left, &right, grips, 0, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == 0);
    // A right hand menu reading means nothing: it is the system's
    left.menu = 0;
    right.menu = 1;
    CHECK(mapped(&left, &right).buttons == 0);
    right.menu = 0;

    // The grips are the bumpers, as worked out by padGripDown
    grips[0] = 1;
    padMap(&left, &right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == PAD_LB);
    grips[0] = 0;
    grips[1] = 1;
    padMap(&left, &right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == PAD_RB);
    // Their travel alone does nothing to the bits
    left.grip = 1.0f;
    right.grip = 1.0f;
    grips[1] = 0;
    padMap(&left, &right, grips, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == 0);

    // Everything at once
    PadHand all = idle();
    all.lower = all.upper = all.stickClick = all.menu = 1;
    int both[2] = { 1, 1 };
    padMap(&all, &all, both, 1, PAD_STICK_DEADZONE_DEFAULT, &s);
    CHECK(s.buttons == PAD_BUTTONS);
}

static void testTheGrips(void) {
    // Down at the trigger's press, up under its release, and in between it
    // stays as it was
    CHECK(!padGripDown(0.0f, 0));
    CHECK(!padGripDown(0.64f, 0));
    CHECK(padGripDown(0.65f, 0));
    CHECK(padGripDown(1.0f, 0));
    CHECK(padGripDown(0.5f, 1));
    CHECK(padGripDown(0.36f, 1));
    CHECK(!padGripDown(0.35f, 1));
    CHECK(!padGripDown(0.0f, 1));
}

static void testTheTriggers(void) {
    // The 13 percent ControllerHandler gives a pad without its own
    CHECK(padTrigger(0.0f) == 0);
    CHECK(padTrigger(0.13f) == 0);
    CHECK(padTrigger(0.131f) == 33);
    // Past it the travel is not rescaled, as a real pad's is not
    CHECK(padTrigger(0.5f) == 127);
    CHECK(padTrigger(1.0f) == 255);
    CHECK(padTrigger(1.4f) == 255);
    CHECK(padTrigger(-0.2f) == 0);
    CHECK(padTrigger(NAN) == 0);

    // Analogue, each on its own side
    PadHand left = idle();
    PadHand right = idle();
    left.trigger = 0.25f;
    right.trigger = 0.9f;
    PadState s = mapped(&left, &right);
    CHECK(s.leftTrigger == 63);
    CHECK(s.rightTrigger == 229);
    CHECK(s.buttons == 0);
}

static void testTheSticks(void) {
    int x, y;
    // Full tilt each way, up staying up: OpenXR has up positive already, so
    // nothing is flipped the way Android's down positive axis is for a pad
    padStick(1.0f, 0.0f, 0.07f, &x, &y);
    CHECK(x == PAD_STICK_FULL && y == 0);
    padStick(-1.0f, 0.0f, 0.07f, &x, &y);
    CHECK(x == -PAD_STICK_FULL && y == 0);
    padStick(0.0f, 1.0f, 0.07f, &x, &y);
    CHECK(x == 0 && y == PAD_STICK_FULL);
    padStick(0.0f, -1.0f, 0.07f, &x, &y);
    CHECK(x == 0 && y == -PAD_STICK_FULL);
    CHECK(PAD_STICK_FULL == 0x7ffe);

    // The dead zone is round: inside it, or on its edge, the stick is centred
    padStick(0.05f, 0.0f, 0.07f, &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.0f, -0.069f, 0.07f, &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.5f, 0.0f, 0.5f, &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.049f, 0.049f, 0.07f, &x, &y);
    CHECK(x == 0 && y == 0);
    // Each axis under it alone, but the two together past it
    padStick(0.05f, 0.05f, 0.07f, &x, &y);
    CHECK(x == 1638 && y == 1638);
    // Past it the reading goes on as it is, not rescaled
    padStick(0.5f, -0.25f, 0.07f, &x, &y);
    CHECK(x == 16383 && y == -8191);
    // A wider dead zone from the settings swallows more
    padStick(0.5f, 0.0f, padDeadzoneFromPercent(50), &x, &y);
    CHECK(x == 0 && y == 0);
    padStick(0.51f, 0.0f, padDeadzoneFromPercent(50), &x, &y);
    CHECK(x == 16710 && y == 0);

    // Held to full tilt, each axis on its own
    padStick(1.3f, -1.2f, 0.07f, &x, &y);
    CHECK(x == PAD_STICK_FULL && y == -PAD_STICK_FULL);
    padStick(1.0f, 1.0f, 0.07f, &x, &y);
    CHECK(x == PAD_STICK_FULL && y == PAD_STICK_FULL);

    // Each controller's stick is its own side of the pad
    PadHand left = idle();
    PadHand right = idle();
    left.stickX = -1.0f;
    right.stickY = 0.5f;
    PadState s = mapped(&left, &right);
    CHECK(s.leftX == -PAD_STICK_FULL && s.leftY == 0);
    CHECK(s.rightX == 0 && s.rightY == 16383);
    CHECK(s.buttons == 0);
}

static void testTheDeadZoneSetting(void) {
    // Whole percent, held to at least 1, as ControllerHandler holds it
    CHECK_NEAR(padDeadzoneFromPercent(7), 0.07, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(0), 0.01, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(-5), 0.01, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(20), 0.2, 1e-6);
    CHECK_NEAR(padDeadzoneFromPercent(250), 1.0, 1e-6);
    CHECK_NEAR(PAD_STICK_DEADZONE_DEFAULT, 0.07, 1e-6);
}

static void testRestAndSame(void) {
    PadState a, b;
    padRest(&a);
    CHECK(padAtRest(&a));
    CHECK(padHeldIn(&a) == 0);
    b = a;
    CHECK(padSame(&a, &b));
    b.rightY = 1;
    CHECK(!padSame(&a, &b));
    CHECK(!padAtRest(&b));
    CHECK(padHeldIn(&b) == PAD_HELD_RS);
    b.rightY = 0;
    b.leftTrigger = 40;
    CHECK(padHeldIn(&b) == PAD_HELD_LT);
    b.buttons = PAD_A | PAD_LB;
    b.leftX = -9;
    b.rightTrigger = 1;
    CHECK(padHeldIn(&b) == (PAD_A | PAD_LB | PAD_HELD_LT | PAD_HELD_RT | PAD_HELD_LS));
}

static void testTheHoldBack(void) {
    // The trigger that pressed a panel's button and the A held as it closed
    PadState s;
    padRest(&s);
    s.rightTrigger = 255;
    s.buttons = PAD_A;
    int held = padHeldIn(&s);
    CHECK(held == (PAD_A | PAD_HELD_RT));

    // Still held: kept off the pad, while a new press on another button and
    // the other stick go straight through
    s.buttons = PAD_A | PAD_B;
    s.leftX = 20000;
    padHoldBack(&held, &s);
    CHECK(s.rightTrigger == 0);
    CHECK(s.buttons == PAD_B);
    CHECK(s.leftX == 20000);
    CHECK(held == (PAD_A | PAD_HELD_RT));

    // The trigger eased off a little but still in: still held back
    padRest(&s);
    s.rightTrigger = 120;
    s.buttons = PAD_A;
    padHoldBack(&held, &s);
    CHECK(s.rightTrigger == 0 && s.buttons == 0);

    // Let go of: free again, so the next press is the game's
    padRest(&s);
    s.buttons = PAD_A;
    padHoldBack(&held, &s);
    CHECK(held == PAD_A);
    CHECK(s.buttons == 0);
    padRest(&s);
    s.rightTrigger = 200;
    padHoldBack(&held, &s);
    CHECK(s.rightTrigger == 200);
    padRest(&s);
    padHoldBack(&held, &s);
    CHECK(held == 0);
    s.buttons = PAD_A;
    padHoldBack(&held, &s);
    CHECK(s.buttons == PAD_A);

    // A stick held over is centred until it comes back inside its dead zone
    padRest(&s);
    s.rightX = -30000;
    s.rightY = 4000;
    held = padHeldIn(&s);
    s.rightX = -10000;
    padHoldBack(&held, &s);
    CHECK(s.rightX == 0 && s.rightY == 0);
    padRest(&s);
    padHoldBack(&held, &s);
    CHECK(held == 0);
    s.rightX = -10000;
    padHoldBack(&held, &s);
    CHECK(s.rightX == -10000);

    // Nothing held, nothing touched
    held = 0;
    s.buttons = PAD_BUTTONS;
    s.leftTrigger = 255;
    padHoldBack(&held, &s);
    CHECK(s.buttons == PAD_BUTTONS && s.leftTrigger == 255);
}

// Frames of the toggle at 72 Hz from a start time, with the menu and grip as
// given, counting the switches and saying Start's answer on the last
static int holdFor(PadToggle* t, int frames, int64_t* clock, int menu, int grip, int* startOk) {
    int fired = 0;
    for (int i = 0; i < frames; i++) {
        fired += padToggleStep(t, menu, grip, *clock, startOk);
        *clock += FRAME_NS;
    }
    return fired;
}

static void testTheToggle(void) {
    PadToggle t;
    padToggleReset(&t);
    int64_t clock = 5000000000LL;
    int startOk = -1;

    // Nothing held, nothing happens, and an untouched menu button may be Start
    CHECK(holdFor(&t, 100, &clock, 0, 0, &startOk) == 0);
    CHECK(startOk == 1);
    // The grip alone is a bumper and nothing more
    CHECK(holdFor(&t, 100, &clock, 0, 1, &startOk) == 0);

    // Grip down, then the menu button: never Start, and the switch comes on
    // the frame half a second in, the 37th at 72 Hz, not the one before
    CHECK(holdFor(&t, 36, &clock, 1, 1, &startOk) == 0);
    CHECK(startOk == 0);
    int fired = padToggleStep(&t, 1, 1, clock, &startOk);
    clock += FRAME_NS;
    CHECK(fired == 1);
    CHECK(startOk == 0);
    // Once a hold, however long it goes on
    CHECK(holdFor(&t, 500, &clock, 1, 1, &startOk) == 0);
    // The grip let go with the menu button still down: not Start either
    CHECK(holdFor(&t, 10, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 0);
    // Let go of both, and the menu button is Start again
    CHECK(holdFor(&t, 1, &clock, 0, 0, &startOk) == 0);
    CHECK(startOk == 1);
    CHECK(holdFor(&t, 1, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 1);

    // Menu button first is Start, until the grip joins it, and then the
    // switch counts from the join
    CHECK(holdFor(&t, 20, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 1);
    int64_t joined = clock;
    CHECK(holdFor(&t, 1, &clock, 1, 1, &startOk) == 0);
    CHECK(startOk == 0);
    CHECK(holdFor(&t, 35, &clock, 1, 1, &startOk) == 0);
    CHECK(clock - joined >= PAD_TOGGLE_HOLD_NS);
    CHECK(holdFor(&t, 1, &clock, 1, 1, &startOk) == 1);

    // A hold broken off short does nothing, and the next one starts afresh
    holdFor(&t, 1, &clock, 0, 0, &startOk);
    CHECK(holdFor(&t, 30, &clock, 1, 1, &startOk) == 0);
    CHECK(holdFor(&t, 1, &clock, 1, 0, &startOk) == 0);
    CHECK(holdFor(&t, 30, &clock, 1, 1, &startOk) == 0);
    CHECK(holdFor(&t, 7, &clock, 1, 1, &startOk) == 1);
    // Each hold switches once, so a second hold switches back
    holdFor(&t, 1, &clock, 0, 1, &startOk);
    CHECK(holdFor(&t, 40, &clock, 1, 1, &startOk) == 1);

    // Exactly the hold, measured on the clock rather than counted in frames:
    // a slow frame rate gets there in fewer frames
    padToggleReset(&t);
    CHECK(padToggleStep(&t, 1, 1, 1000, &startOk) == 0);
    CHECK(padToggleStep(&t, 1, 1, 1000 + PAD_TOGGLE_HOLD_NS - 1, &startOk) == 0);
    CHECK(padToggleStep(&t, 1, 1, 1000 + PAD_TOGGLE_HOLD_NS, &startOk) == 1);
    padToggleReset(&t);
    CHECK(padToggleStep(&t, 1, 1, 1000, &startOk) == 0);
    CHECK(padToggleStep(&t, 1, 1, 1000 + 2 * PAD_TOGGLE_HOLD_NS, &startOk) == 1);
    CHECK(PAD_TOGGLE_HOLD_NS == 500000000LL);

    // A reset forgets a hold under way and gives Start back
    padToggleReset(&t);
    holdFor(&t, 20, &clock, 1, 1, &startOk);
    padToggleReset(&t);
    CHECK(t.holding == 0 && t.menuTaken == 0);
    CHECK(holdFor(&t, 1, &clock, 1, 0, &startOk) == 0);
    CHECK(startOk == 1);
}

// The menu button and grip going through the toggle and into the pad the way
// the frame does it: Start never reaches the host for a switch made grip
// first, while the grip is the left bumper throughout
static void testTheSwitchOnThePad(void) {
    PadToggle t;
    padToggleReset(&t);
    int64_t clock = 0;
    PadHand left = idle();
    PadHand right = idle();
    int grips[2] = { 0, 0 };
    int startOk;
    int sawStart = 0;
    int sawBumper = 0;
    int fired = 0;
    for (int i = 0; i < 60; i++) {
        left.grip = 1.0f;
        left.menu = i >= 5;
        grips[0] = padGripDown(left.grip, grips[0]);
        fired += padToggleStep(&t, left.menu, grips[0], clock, &startOk);
        PadState s;
        padMap(&left, &right, grips, startOk, PAD_STICK_DEADZONE_DEFAULT, &s);
        sawStart |= (s.buttons & PAD_START) != 0;
        sawBumper |= (s.buttons & PAD_LB) != 0;
        clock += FRAME_NS;
    }
    CHECK(fired == 1);
    CHECK(!sawStart);
    CHECK(sawBumper);
}

int main(void) {
    testTheBits();
    testTheButtons();
    testTheGrips();
    testTheTriggers();
    testTheSticks();
    testTheDeadZoneSetting();
    testRestAndSame();
    testTheHoldBack();
    testTheToggle();
    testTheSwitchOnThePad();
    return checksDone("xr_gamepad");
}
