// Gamepad mode, see xr_gamepad.h
#include <math.h>

#include "xr_gamepad.h"

int padGripDown(float value, int was) {
    return was ? value > PAD_GRIP_OFF : value >= PAD_GRIP_ON;
}

static float clampUnit(float v) {
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

void padStick(float x, float y, float deadzone, int* outX, int* outY) {
    x = clampUnit(x);
    y = clampUnit(y);
    if (sqrtf(x * x + y * y) <= deadzone) {
        *outX = 0;
        *outY = 0;
        return;
    }
    // Toward zero, as the Java cast to short does for a real pad
    *outX = (int)(x * (float)PAD_STICK_FULL);
    *outY = (int)(y * (float)PAD_STICK_FULL);
}

int padTrigger(float value) {
    if (!(value > PAD_TRIGGER_DEADZONE)) {
        return 0;
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    return (int)(value * (float)PAD_TRIGGER_FULL);
}

float padDeadzoneFromPercent(int percent) {
    if (percent < 1) {
        percent = 1;
    }
    if (percent > 100) {
        percent = 100;
    }
    return (float)percent / 100.0f;
}

void padMap(const PadHand* left, const PadHand* right, const int gripDown[2], int startOk,
            float stickDeadzone, PadState* out) {
    int buttons = 0;
    if (right->lower) {
        buttons |= PAD_A;
    }
    if (right->upper) {
        buttons |= PAD_B;
    }
    if (left->lower) {
        buttons |= PAD_X;
    }
    if (left->upper) {
        buttons |= PAD_Y;
    }
    if (gripDown[0]) {
        buttons |= PAD_LB;
    }
    if (gripDown[1]) {
        buttons |= PAD_RB;
    }
    if (left->stickClick) {
        buttons |= PAD_LS_CLICK;
    }
    if (right->stickClick) {
        buttons |= PAD_RS_CLICK;
    }
    if (left->menu && startOk) {
        buttons |= PAD_START;
    }
    out->buttons = buttons;
    out->leftTrigger = padTrigger(left->trigger);
    out->rightTrigger = padTrigger(right->trigger);
    padStick(left->stickX, left->stickY, stickDeadzone, &out->leftX, &out->leftY);
    padStick(right->stickX, right->stickY, stickDeadzone, &out->rightX, &out->rightY);
}

void padRest(PadState* s) {
    s->buttons = 0;
    s->leftTrigger = 0;
    s->rightTrigger = 0;
    s->leftX = 0;
    s->leftY = 0;
    s->rightX = 0;
    s->rightY = 0;
}

int padAtRest(const PadState* s) {
    return s->buttons == 0 && s->leftTrigger == 0 && s->rightTrigger == 0 && s->leftX == 0
            && s->leftY == 0 && s->rightX == 0 && s->rightY == 0;
}

int padSame(const PadState* a, const PadState* b) {
    return a->buttons == b->buttons && a->leftTrigger == b->leftTrigger
            && a->rightTrigger == b->rightTrigger && a->leftX == b->leftX
            && a->leftY == b->leftY && a->rightX == b->rightX && a->rightY == b->rightY;
}

int padHeldIn(const PadState* s) {
    int held = s->buttons;
    if (s->leftTrigger != 0) {
        held |= PAD_HELD_LT;
    }
    if (s->rightTrigger != 0) {
        held |= PAD_HELD_RT;
    }
    if (s->leftX != 0 || s->leftY != 0) {
        held |= PAD_HELD_LS;
    }
    if (s->rightX != 0 || s->rightY != 0) {
        held |= PAD_HELD_RS;
    }
    return held;
}

void padHoldBack(int* held, PadState* s) {
    // Let go of since, so free again
    *held &= padHeldIn(s);
    s->buttons &= ~*held;
    if (*held & PAD_HELD_LT) {
        s->leftTrigger = 0;
    }
    if (*held & PAD_HELD_RT) {
        s->rightTrigger = 0;
    }
    if (*held & PAD_HELD_LS) {
        s->leftX = 0;
        s->leftY = 0;
    }
    if (*held & PAD_HELD_RS) {
        s->rightX = 0;
        s->rightY = 0;
    }
}

void padToggleReset(PadToggle* t) {
    t->holding = 0;
    t->sinceNs = 0;
    t->fired = 0;
    t->menuTaken = 0;
}

int padToggleStep(PadToggle* t, int menu, int grip, int64_t nowNs, int* startOk) {
    int fire = 0;
    if (menu && grip) {
        t->menuTaken = 1;
        if (!t->holding) {
            t->holding = 1;
            t->sinceNs = nowNs;
        }
        if (!t->fired && nowNs - t->sinceNs >= PAD_TOGGLE_HOLD_NS) {
            t->fired = 1;
            fire = 1;
        }
    }
    else {
        // Either let go starts the next hold afresh
        t->holding = 0;
        t->fired = 0;
    }
    if (!menu) {
        t->menuTaken = 0;
    }
    *startOk = !t->menuTaken;
    return fire;
}

int padChordParts(int shortcut) {
    if (shortcut == PAD_SHORTCUT_STICKS) {
        return PAD_PART_LS | PAD_PART_RS;
    }
    if (shortcut == PAD_SHORTCUT_TRIGGERS_GRIPS) {
        return PAD_PART_LT | PAD_PART_RT | PAD_PART_LG | PAD_PART_RG;
    }
    return 0;
}

void padChordReset(PadChord* c, int parts) {
    c->parts = parts;
    c->stirred = 0;
    c->pressed = 0;
    c->forming = 0;
    c->startNs = 0;
    c->allDown = 0;
    c->allNs = 0;
    c->pending = 0;
    for (int i = 0; i < PAD_PART_COUNT; i++) {
        c->peak[i] = 0.0f;
    }
    c->taken = 0;
    c->spent = 1;
    c->held = 0;
    c->replay = 0;
}

// Part i's reading off the two controllers, a click as 0 or 1
static float partValue(const PadHand* left, const PadHand* right, int i) {
    switch (i) {
        case 0: return left->stickClick ? 1.0f : 0.0f;
        case 1: return right->stickClick ? 1.0f : 0.0f;
        case 2: return left->trigger;
        case 3: return right->trigger;
        case 4: return left->grip;
        default: return right->grip;
    }
}

int padChordStep(PadChord* c, const PadHand* left, const PadHand* right, int64_t nowNs) {
    float value[PAD_PART_COUNT];
    int stirred = 0;
    int pressed = 0;
    for (int i = 0; i < PAD_PART_COUNT; i++) {
        int bit = 1 << i;
        value[i] = partValue(left, right, i);
        int was = (c->pressed & bit) != 0;
        int down = i < 2 ? value[i] > 0.5f : padGripDown(value[i], was);
        if (down) {
            pressed |= bit;
        }
        // A trigger reaches the host well before it counts as pressed
        if (down || (i >= 2 && i < 4 && value[i] > PAD_TRIGGER_DEADZONE)) {
            stirred |= bit;
        }
    }
    stirred &= c->parts;
    pressed &= c->parts;
    int restBefore = c->stirred == 0;
    c->stirred = stirred;
    c->pressed = pressed;
    c->replay = 0;
    c->taken &= stirred;
    if (c->parts == 0) {
        c->held = 0;
        return PAD_CHORD_NOTHING;
    }

    int event = PAD_CHORD_NOTHING;
    if (!c->forming && !c->spent && restBefore && stirred != 0) {
        c->forming = 1;
        c->startNs = nowNs;
        c->allDown = 0;
        c->pending = 0;
        for (int i = 0; i < PAD_PART_COUNT; i++) {
            c->peak[i] = 0.0f;
        }
        event = PAD_CHORD_FORMING;
    }
    if (c->forming) {
        c->pending |= stirred;
        for (int i = 0; i < PAD_PART_COUNT; i++) {
            if ((stirred & (1 << i)) && value[i] > c->peak[i]) {
                c->peak[i] = value[i];
            }
        }
        int all = pressed == c->parts;
        if (all && !c->allDown && nowNs - c->startNs <= PAD_CHORD_WINDOW_NS) {
            c->allDown = 1;
            c->allNs = nowNs;
        }
        if ((c->pending & ~stirred) != 0 || (c->allDown && !all)
                || (!c->allDown && nowNs - c->startNs > PAD_CHORD_WINDOW_NS)) {
            // Given up: what was let go meanwhile is a tap, the rest goes on
            c->replay = c->pending & ~stirred;
            c->pending = 0;
            c->forming = 0;
            event = PAD_CHORD_ABANDONED;
        }
        else if (c->allDown && nowNs - c->allNs >= PAD_CHORD_HOLD_NS) {
            c->taken = stirred;
            c->pending = 0;
            c->forming = 0;
            c->spent = 1;
            event = PAD_CHORD_FIRED;
        }
    }
    else if (c->spent && stirred == 0) {
        c->spent = 0;
    }
    c->held = c->pending | c->taken;
    return event;
}

static int partIndex(int part) {
    for (int i = 0; i < PAD_PART_COUNT; i++) {
        if (part == (1 << i)) {
            return i;
        }
    }
    return -1;
}

float padChordSeen(const PadChord* c, int part, float raw) {
    if (c->replay & part) {
        int i = partIndex(part);
        return i >= 0 ? c->peak[i] : raw;
    }
    if (c->held & part) {
        return 0.0f;
    }
    return raw;
}

void padChordApply(const PadChord* c, const PadHand* left, const PadHand* right,
                   PadHand* outLeft, PadHand* outRight) {
    *outLeft = *left;
    *outRight = *right;
    outLeft->stickClick = padChordSeen(c, PAD_PART_LS, left->stickClick ? 1.0f : 0.0f) > 0.5f;
    outRight->stickClick = padChordSeen(c, PAD_PART_RS, right->stickClick ? 1.0f : 0.0f) > 0.5f;
    outLeft->trigger = padChordSeen(c, PAD_PART_LT, left->trigger);
    outRight->trigger = padChordSeen(c, PAD_PART_RT, right->trigger);
    outLeft->grip = padChordSeen(c, PAD_PART_LG, left->grip);
    outRight->grip = padChordSeen(c, PAD_PART_RG, right->grip);
    if (c->held & PAD_PART_LS) {
        outLeft->stickX = 0.0f;
        outLeft->stickY = 0.0f;
    }
    if (c->held & PAD_PART_RS) {
        outRight->stickX = 0.0f;
        outRight->stickY = 0.0f;
    }
}

float padRumbleAmplitude(int motor) {
    return (float)(motor & 0xffff) / 65535.0f;
}

void padRumbleReset(PadRumble* r) {
    for (int h = 0; h < 2; h++) {
        r->want[h] = 0.0f;
        r->given[h] = 0.0f;
        r->givenNs[h] = 0;
    }
    r->fresh = 0;
}

void padRumbleAsk(PadRumble* r, int lowMotor, int highMotor) {
    r->want[0] = padRumbleAmplitude(lowMotor);
    r->want[1] = padRumbleAmplitude(highMotor);
    r->fresh = 1;
}

void padRumbleStep(PadRumble* r, const int live[2], int64_t nowNs, int action[2], float amp[2]) {
    for (int h = 0; h < 2; h++) {
        float target = live[h] ? r->want[h] : 0.0f;
        action[h] = PAD_RUMBLE_KEEP;
        amp[h] = target;
        if (target <= 0.0f) {
            if (r->given[h] > 0.0f) {
                action[h] = PAD_RUMBLE_STOP;
                r->given[h] = 0.0f;
            }
            continue;
        }
        if (r->fresh || r->given[h] != target || nowNs - r->givenNs[h] >= PAD_RUMBLE_REARM_NS) {
            action[h] = PAD_RUMBLE_APPLY;
            r->given[h] = target;
            r->givenNs[h] = nowNs;
        }
    }
    r->fresh = 0;
}
