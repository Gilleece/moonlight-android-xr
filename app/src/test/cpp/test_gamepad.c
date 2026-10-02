// Gamepad mode: the controllers' readings into Moonlight's pad, the dead
// zones, the grips as bumpers, the controls held back until let go, the left
// menu button with the left grip switching modes after half a second, and the
// two chord shortcuts, both stick clicks or both triggers with both grips,
// held back from the pad while they form, and the host's rumble on the two
// controllers
#include <stdlib.h>
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

// Both controllers through a chord frame by frame at 72 Hz the way the frame
// does it, into the pad. What each frame sent is kept, so a test can say
// whether anything of the chord ever reached the host.
typedef struct {
    PadChord chord;
    int64_t clock;
    int grips[2];
    int fired;
    int abandoned;
    // OR of every frame's buttons, the highest trigger sent each side, and
    // the most a stick moved
    int sentButtons;
    int sentLT;
    int sentRT;
    int sentStick;
    // The last frame's pad and event
    PadState last;
    int event;
} ChordRun;

static void chordStart(ChordRun* r, int shortcut) {
    memset(r, 0, sizeof(*r));
    padChordReset(&r->chord, padChordParts(shortcut));
    r->clock = 7000000000LL;
    // Nothing held at the start, so the reset's wait is over on frame one
    PadHand none = idle();
    padChordStep(&r->chord, &none, &none, r->clock);
    r->clock += FRAME_NS;
}

static void chordFrames(ChordRun* r, int frames, const PadHand* left, const PadHand* right) {
    for (int i = 0; i < frames; i++) {
        r->event = padChordStep(&r->chord, left, right, r->clock);
        r->fired += r->event == PAD_CHORD_FIRED;
        r->abandoned += r->event == PAD_CHORD_ABANDONED;
        PadHand l, rr;
        padChordApply(&r->chord, left, right, &l, &rr);
        r->grips[0] = padGripDown(l.grip, r->grips[0]);
        r->grips[1] = padGripDown(rr.grip, r->grips[1]);
        padMap(&l, &rr, r->grips, 1, PAD_STICK_DEADZONE_DEFAULT, &r->last);
        r->sentButtons |= r->last.buttons;
        if (r->last.leftTrigger > r->sentLT) {
            r->sentLT = r->last.leftTrigger;
        }
        if (r->last.rightTrigger > r->sentRT) {
            r->sentRT = r->last.rightTrigger;
        }
        int moved = abs(r->last.leftX) + abs(r->last.leftY) + abs(r->last.rightX)
                + abs(r->last.rightY);
        if (moved > r->sentStick) {
            r->sentStick = moved;
        }
        r->clock += FRAME_NS;
    }
}

static void chordForget(ChordRun* r) {
    r->sentButtons = 0;
    r->sentLT = 0;
    r->sentRT = 0;
    r->sentStick = 0;
    r->fired = 0;
    r->abandoned = 0;
}

static void testTheChordsParts(void) {
    CHECK(PAD_SHORTCUT_MENU_GRIP == 0 && PAD_SHORTCUT_STICKS == 1
          && PAD_SHORTCUT_TRIGGERS_GRIPS == 2);
    CHECK(padChordParts(PAD_SHORTCUT_MENU_GRIP) == 0);
    CHECK(padChordParts(PAD_SHORTCUT_STICKS) == (PAD_PART_LS | PAD_PART_RS));
    CHECK(padChordParts(PAD_SHORTCUT_TRIGGERS_GRIPS)
          == (PAD_PART_LT | PAD_PART_RT | PAD_PART_LG | PAD_PART_RG));
    CHECK(padChordParts(7) == 0);
    CHECK(padChordParts(-1) == 0);
    // About a third of a second held, inside a sixth of one to get there
    CHECK(PAD_CHORD_HOLD_NS == 300000000LL);
    CHECK(PAD_CHORD_WINDOW_NS == 150000000LL);
}

// Both stick clicks: neither reaches the pad while it forms, nor while held
// after it switches, nor does the stick they are pressed in with
static void testBothSticksSwitch(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();

    // L3, then R3 five frames later, with the sticks nudged as they go in
    left.stickClick = 1;
    left.stickX = 0.4f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_FORMING);
    CHECK(r.chord.held == PAD_PART_LS);
    chordFrames(&r, 4, &left, &right);
    right.stickClick = 1;
    right.stickY = -0.3f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.chord.held == (PAD_PART_LS | PAD_PART_RS));
    CHECK(r.chord.allDown);
    // 0.3 s from both being in is 21.6 frames at 72 Hz: the 22nd after
    chordFrames(&r, 21, &left, &right);
    CHECK(r.fired == 0);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.event == PAD_CHORD_FIRED);
    CHECK(r.chord.taken == (PAD_PART_LS | PAD_PART_RS));
    // Held on: nothing, and still nothing sent
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == 0);
    CHECK(r.sentStick == 0);
    CHECK(r.abandoned == 0);

    // L3 let go first and pressed again with R3 still in: the switch is
    // spent, so it goes through and switches nothing
    left.stickClick = 0;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.chord.held == PAD_PART_RS);
    left.stickClick = 1;
    chordFrames(&r, 50, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == PAD_LS_CLICK);
    CHECK((r.last.buttons & PAD_RS_CLICK) == 0);
    // Both let go: free again, and the next chord switches back
    left = idle();
    right = idle();
    chordFrames(&r, 2, &left, &right);
    CHECK(r.chord.held == 0 && !r.chord.spent);
    chordForget(&r);
    left.stickClick = 1;
    right.stickClick = 1;
    chordFrames(&r, 40, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == 0);
}

// One stick click on its own is the game's, a window late
static void testOneStickGoesThroughLate(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = 1;
    // The window is 10.8 frames: held back for 11, through on the 12th
    chordFrames(&r, 11, &left, &right);
    CHECK(r.sentButtons == 0);
    CHECK(r.abandoned == 0);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.abandoned == 1);
    CHECK(r.last.buttons == PAD_LS_CLICK);
    CHECK(r.chord.replay == 0);
    chordFrames(&r, 30, &left, &right);
    CHECK(r.last.buttons == PAD_LS_CLICK);
    // The other one joining late is just R3, not the shortcut
    right.stickClick = 1;
    chordFrames(&r, 60, &left, &right);
    CHECK(r.fired == 0);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK));
    // Let go, both are clean releases
    left = idle();
    right = idle();
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == 0);
}

// A quick click inside the window is replayed as one frame down
static void testAQuickClickIsATap(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();
    right.stickClick = 1;
    chordFrames(&r, 4, &left, &right);
    CHECK(r.sentButtons == 0);
    right.stickClick = 0;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_ABANDONED);
    CHECK(r.chord.replay == PAD_PART_RS);
    CHECK(r.last.buttons == PAD_RS_CLICK);
    CHECK(padChordSeen(&r.chord, PAD_PART_RS, 0.0f) == 1.0f);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == 0);
    CHECK(r.chord.replay == 0);
    // And a second click right after starts a window of its own
    right.stickClick = 1;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_FORMING);
    CHECK(r.last.buttons == 0);
}

// Both in, then one let go before the hold is over: the one let go is a tap,
// the one still in goes through, and nothing switches
static void testAChordLetGoShort(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_STICKS);
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = 1;
    right.stickClick = 1;
    left.stickY = 0.8f;
    chordFrames(&r, 15, &left, &right);
    CHECK(r.sentButtons == 0 && r.sentStick == 0);
    left.stickClick = 0;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_ABANDONED);
    CHECK(r.fired == 0);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK));
    // The stick comes back with its click no longer held
    CHECK(r.last.leftY == (int)(0.8f * PAD_STICK_FULL));
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == PAD_RS_CLICK);
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0);
}

// The hold and the window, on the clock rather than in frames
static void testTheChordsTiming(void) {
    PadChord c;
    PadHand left = idle();
    PadHand right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    padChordStep(&c, &left, &right, 0);
    left.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, 1000) == PAD_CHORD_FORMING);
    right.stickClick = 1;
    // The second part exactly as the window closes still counts
    CHECK(padChordStep(&c, &left, &right, 1000 + PAD_CHORD_WINDOW_NS) == PAD_CHORD_NOTHING);
    int64_t all = 1000 + PAD_CHORD_WINDOW_NS;
    CHECK(padChordStep(&c, &left, &right, all + PAD_CHORD_HOLD_NS - 1) == PAD_CHORD_NOTHING);
    CHECK(padChordStep(&c, &left, &right, all + PAD_CHORD_HOLD_NS) == PAD_CHORD_FIRED);

    // One nanosecond later is too late
    left = idle();
    right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    padChordStep(&c, &left, &right, 0);
    left.stickClick = 1;
    padChordStep(&c, &left, &right, 1000);
    right.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, 1001 + PAD_CHORD_WINDOW_NS) == PAD_CHORD_ABANDONED);
    CHECK(c.held == 0);
    CHECK(padChordStep(&c, &left, &right, 1000 + 5 * PAD_CHORD_HOLD_NS) == PAD_CHORD_NOTHING);

    // Both in the same frame
    left = idle();
    right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    padChordStep(&c, &left, &right, 0);
    left.stickClick = 1;
    right.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, 50) == PAD_CHORD_FORMING);
    CHECK(c.allDown);
    CHECK(padChordStep(&c, &left, &right, 50 + PAD_CHORD_HOLD_NS) == PAD_CHORD_FIRED);
}

// Both triggers and both grips: a trigger is held back from the moment it
// would reach the host, so not even its first travel leaks
static void testTriggersAndGripsSwitch(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    PadHand left = idle();
    PadHand right = idle();
    // Squeezed over a few frames, a hand at a time, triggers first
    float ramp[] = { 0.2f, 0.5f, 0.9f, 1.0f };
    for (int i = 0; i < 4; i++) {
        right.trigger = ramp[i];
        chordFrames(&r, 1, &left, &right);
    }
    CHECK(r.chord.held == PAD_PART_RT);
    right.grip = 0.7f;
    left.trigger = 0.95f;
    chordFrames(&r, 1, &left, &right);
    left.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.chord.allDown);
    chordFrames(&r, 25, &left, &right);
    CHECK(r.fired == 1);
    chordFrames(&r, 50, &left, &right);
    CHECK(r.fired == 1);
    CHECK(r.sentButtons == 0);
    CHECK(r.sentLT == 0 && r.sentRT == 0);
    // Let go a part at a time: each stays back until it is let go, and the
    // rest never come through either
    right.trigger = 0.0f;
    chordFrames(&r, 3, &left, &right);
    left.grip = 0.0f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.sentButtons == 0 && r.sentLT == 0 && r.sentRT == 0);
    left = idle();
    right = idle();
    chordFrames(&r, 2, &left, &right);
    CHECK(r.chord.held == 0);
    CHECK(r.sentButtons == 0 && r.sentLT == 0 && r.sentRT == 0);
}

// The pieces of it on their own are the game's, a window late
static void testTriggersAndGripsAlone(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    PadHand left = idle();
    PadHand right = idle();
    // A trigger held part way, a throttle say: back for the window, then
    // through at what it reads, and on from there without a further wait
    right.trigger = 0.4f;
    chordFrames(&r, 11, &left, &right);
    CHECK(r.sentRT == 0);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.abandoned == 1);
    CHECK(r.last.rightTrigger == padTrigger(0.4f));
    right.trigger = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.rightTrigger == 255);
    // With it already through, the rest of the chord cannot make one
    left.trigger = 1.0f;
    left.grip = 1.0f;
    right.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.leftTrigger == 255);
    CHECK(r.last.buttons == (PAD_LB | PAD_RB));
    chordFrames(&r, 60, &left, &right);
    CHECK(r.fired == 0);

    // A trigger tapped inside the window comes through at the furthest it went
    left = idle();
    right = idle();
    chordFrames(&r, 2, &left, &right);
    chordForget(&r);
    left.trigger = 0.5f;
    chordFrames(&r, 1, &left, &right);
    left.trigger = 0.8f;
    chordFrames(&r, 1, &left, &right);
    left.trigger = 0.6f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.sentLT == 0);
    left.trigger = 0.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.leftTrigger == padTrigger(0.8f));
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.leftTrigger == 0);

    // A grip pressed as a bumper and let go inside it is one frame of LB
    chordForget(&r);
    left.grip = 0.9f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.sentButtons == 0);
    left.grip = 0.1f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == PAD_LB);
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == 0);

    // A grip only half way is not a bumper, so it starts nothing and is never
    // held back
    chordForget(&r);
    left.grip = 0.5f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.chord.held == 0 && !r.chord.forming);

    // A trigger under its dead zone neither
    left = idle();
    left.trigger = 0.1f;
    chordFrames(&r, 3, &left, &right);
    CHECK(r.chord.held == 0 && !r.chord.forming);
}

// All four in, then a trigger eased off past its release: given up
static void testAChordEasedOff(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    PadHand left = idle();
    PadHand right = idle();
    left.trigger = right.trigger = 1.0f;
    left.grip = right.grip = 1.0f;
    chordFrames(&r, 10, &left, &right);
    CHECK(r.chord.allDown);
    // Eased under the press but over the release still counts
    right.trigger = 0.5f;
    chordFrames(&r, 5, &left, &right);
    CHECK(r.abandoned == 0);
    right.trigger = 0.3f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.abandoned == 1);
    CHECK(r.fired == 0);
    // Everything still held goes through at once, the trigger at its travel
    CHECK(r.last.leftTrigger == 255);
    CHECK(r.last.rightTrigger == padTrigger(0.3f));
    CHECK(r.last.buttons == (PAD_LB | PAD_RB));
    chordFrames(&r, 60, &left, &right);
    CHECK(r.fired == 0);
}

// The menu and grip shortcut has no chord, so nothing is ever held back, and
// with it chosen the chords' controls are simply the pad's
static void testNoChordHoldsNothing(void) {
    ChordRun r;
    chordStart(&r, PAD_SHORTCUT_MENU_GRIP);
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = right.stickClick = 1;
    left.trigger = right.trigger = 1.0f;
    left.grip = right.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_NOTHING);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK | PAD_LB | PAD_RB));
    CHECK(r.last.leftTrigger == 255 && r.last.rightTrigger == 255);
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0 && r.chord.held == 0);
    CHECK(padChordSeen(&r.chord, PAD_PART_LT, 0.7f) == 0.7f);

    // With the sticks chosen, triggers and grips are not held either
    chordStart(&r, PAD_SHORTCUT_STICKS);
    left = idle();
    right = idle();
    left.trigger = right.trigger = 1.0f;
    left.grip = right.grip = 1.0f;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.event == PAD_CHORD_NOTHING);
    CHECK(r.last.leftTrigger == 255 && r.last.buttons == (PAD_LB | PAD_RB));
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0);
    // And with the triggers chosen, the stick clicks
    chordStart(&r, PAD_SHORTCUT_TRIGGERS_GRIPS);
    left = idle();
    right = idle();
    left.stickClick = right.stickClick = 1;
    chordFrames(&r, 1, &left, &right);
    CHECK(r.last.buttons == (PAD_LS_CLICK | PAD_RS_CLICK));
    chordFrames(&r, 100, &left, &right);
    CHECK(r.fired == 0);
}

// A reset with the chord's controls already down never switches by itself:
// they have to be let go first, and they go straight through meanwhile
static void testAResetWaitsForRest(void) {
    PadChord c;
    PadHand left = idle();
    PadHand right = idle();
    left.stickClick = right.stickClick = 1;
    padChordReset(&c, padChordParts(PAD_SHORTCUT_STICKS));
    int64_t t = 0;
    int fired = 0;
    for (int i = 0; i < 100; i++) {
        fired += padChordStep(&c, &left, &right, t) == PAD_CHORD_FIRED;
        CHECK(c.held == 0);
        t += FRAME_NS;
    }
    CHECK(fired == 0);
    left = idle();
    right = idle();
    padChordStep(&c, &left, &right, t);
    CHECK(!c.spent);
    // Without parts, a step is nothing at all
    padChordReset(&c, 0);
    left.stickClick = right.stickClick = 1;
    CHECK(padChordStep(&c, &left, &right, t) == PAD_CHORD_NOTHING);
    CHECK(c.held == 0 && c.replay == 0);
}

// Readings that are not numbers neither start a chord nor finish one
static void testTheChordIgnoresNonsense(void) {
    PadChord c;
    PadHand left = idle();
    PadHand right = idle();
    padChordReset(&c, padChordParts(PAD_SHORTCUT_TRIGGERS_GRIPS));
    padChordStep(&c, &left, &right, 0);
    left.trigger = NAN;
    left.grip = NAN;
    CHECK(padChordStep(&c, &left, &right, FRAME_NS) == PAD_CHORD_NOTHING);
    CHECK(c.held == 0);
}

// The host's 16 bit motor levels as amplitudes, a Java short's sign bit
// included
static void testTheRumbleLevels(void) {
    CHECK(padRumbleAmplitude(0) == 0.0f);
    CHECK(padRumbleAmplitude(0xffff) == 1.0f);
    CHECK_NEAR(padRumbleAmplitude(0x8000), 32768.0 / 65535.0, 1e-6);
    CHECK_NEAR(padRumbleAmplitude(0x4000), 16384.0 / 65535.0, 1e-6);
    CHECK_NEAR(padRumbleAmplitude(1), 1.0 / 65535.0, 1e-9);
    // As a short arrives sign extended
    CHECK(padRumbleAmplitude(-1) == 1.0f);
    CHECK_NEAR(padRumbleAmplitude((short)0x8000), 32768.0 / 65535.0, 1e-6);
    CHECK_NEAR(padRumbleAmplitude((short)0xc000), 49152.0 / 65535.0, 1e-6);
    // Never past either end
    for (int m = -70000; m <= 70000; m += 997) {
        float a = padRumbleAmplitude(m);
        CHECK(a >= 0.0f && a <= 1.0f);
    }
    // Low on the left, high on the right
    PadRumble r;
    padRumbleReset(&r);
    padRumbleAsk(&r, 0xffff, 0x4000);
    CHECK(r.want[0] == 1.0f);
    CHECK_NEAR(r.want[1], 16384.0 / 65535.0, 1e-6);
    CHECK(r.fresh);
}

static void testTheRumblePulses(void) {
    PadRumble r;
    int live[2] = { 1, 1 };
    int action[2];
    float amp[2];
    padRumbleReset(&r);
    // Nothing asked, nothing done
    padRumbleStep(&r, live, 0, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);

    // Asked: both at once, each its own motor
    int64_t t = 1000000000LL;
    padRumbleAsk(&r, 0x8000, 0xffff);
    padRumbleStep(&r, live, t, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_APPLY);
    CHECK_NEAR(amp[0], 32768.0 / 65535.0, 1e-6);
    CHECK(amp[1] == 1.0f);
    CHECK(!r.fresh);
    // Not again until the rearm, which comes well inside a pulse
    CHECK(PAD_RUMBLE_REARM_NS < PAD_RUMBLE_PULSE_NS);
    CHECK(PAD_RUMBLE_PULSE_NS - PAD_RUMBLE_REARM_NS > 2 * FRAME_NS);
    int64_t last = t;
    int applies = 0;
    int64_t worstGap = 0;
    for (int64_t now = t + FRAME_NS; now < t + 1000000000LL; now += FRAME_NS) {
        padRumbleStep(&r, live, now, action, amp);
        CHECK(action[0] == action[1]);
        CHECK(action[0] != PAD_RUMBLE_STOP);
        if (action[0] == PAD_RUMBLE_APPLY) {
            applies++;
            if (now - last > worstGap) {
                worstGap = now - last;
            }
            last = now;
        }
    }
    // A held rumble is armed again about every 50 ms, each pulse running
    // into the next, so a second of it never stutters
    CHECK(applies >= 15 && applies <= 20);
    CHECK(worstGap < PAD_RUMBLE_PULSE_NS);
    CHECK(worstGap >= PAD_RUMBLE_REARM_NS);

    // A fresh word from the host arms at once, even with the same values
    padRumbleStep(&r, live, last + FRAME_NS, action, amp);
    padRumbleAsk(&r, 0x8000, 0xffff);
    padRumbleStep(&r, live, last + 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_APPLY);
    last += 2 * FRAME_NS;

    // A change on one motor alone: that side applies, the other waits
    padRumbleStep(&r, live, last + FRAME_NS, action, amp);
    r.want[1] = 0.25f;
    padRumbleStep(&r, live, last + 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_APPLY);
    CHECK(amp[1] == 0.25f);
    last += 2 * FRAME_NS;

    // The low motor let go: the left stops, once, and the right goes on
    padRumbleAsk(&r, 0, 0x4000);
    padRumbleStep(&r, live, last + FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_STOP && action[1] == PAD_RUMBLE_APPLY);
    padRumbleStep(&r, live, last + 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP);

    // 0/0 stops both, once
    padRumbleAsk(&r, 0, 0);
    padRumbleStep(&r, live, last + 3 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_STOP);
    for (int i = 0; i < 20; i++) {
        padRumbleStep(&r, live, last + (4 + i) * FRAME_NS, action, amp);
        CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
    }
}

// A controller that is not the pad's, or the pad out of the session's reach,
// is stopped and left alone, and picks the host's rumble up again when it is
// back while the host still wants it
static void testTheRumbleOffThePad(void) {
    PadRumble r;
    int both[2] = { 1, 1 };
    int leftOnly[2] = { 1, 0 };
    int none[2] = { 0, 0 };
    int action[2];
    float amp[2];
    padRumbleReset(&r);
    padRumbleAsk(&r, 0xffff, 0xffff);
    padRumbleStep(&r, leftOnly, 0, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_KEEP);
    padRumbleStep(&r, both, FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_APPLY);
    // Out of the pad: both stop, once
    padRumbleStep(&r, none, 2 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_STOP && action[1] == PAD_RUMBLE_STOP);
    padRumbleStep(&r, none, 3 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
    // A word from the host meanwhile arms nothing
    padRumbleAsk(&r, 0xffff, 0);
    padRumbleStep(&r, none, 4 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
    // Back: what the host still wants, at once
    padRumbleStep(&r, both, 5 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_APPLY && action[1] == PAD_RUMBLE_KEEP);
    CHECK(amp[0] == 1.0f);
    // A reset forgets what was asked and stops nothing, so whatever runs is
    // stopped before it
    padRumbleReset(&r);
    padRumbleStep(&r, both, 6 * FRAME_NS, action, amp);
    CHECK(action[0] == PAD_RUMBLE_KEEP && action[1] == PAD_RUMBLE_KEEP);
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
    testTheChordsParts();
    testBothSticksSwitch();
    testOneStickGoesThroughLate();
    testAQuickClickIsATap();
    testAChordLetGoShort();
    testTheChordsTiming();
    testTriggersAndGripsSwitch();
    testTriggersAndGripsAlone();
    testAChordEasedOff();
    testNoChordHoldsNothing();
    testAResetWaitsForRest();
    testTheChordIgnoresNonsense();
    testTheRumbleLevels();
    testTheRumblePulses();
    testTheRumbleOffThePad();
    return checksDone("xr_gamepad");
}
