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
