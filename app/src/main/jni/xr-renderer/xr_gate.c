// Who may point, as plain arithmetic, see xr_gate.h
#include "xr_gate.h"

#include <string.h>

int profileKind(int bound, int handProfile) {
    if (!bound) {
        return PROFILE_NONE;
    }
    return handProfile ? PROFILE_HANDS : PROFILE_CONTROLLER;
}

int gazeUsable(unsigned flags) {
    const unsigned needed = GATE_POSITION_VALID | GATE_ORIENTATION_VALID
            | GATE_ORIENTATION_TRACKED;
    return (flags & needed) == needed;
}

int aimFullyTracked(unsigned flags) {
    const unsigned needed = GATE_POSITION_TRACKED | GATE_ORIENTATION_TRACKED;
    return (flags & needed) == needed;
}

int controllerInUse(int kind, int tracked, int awake) {
    return kind == PROFILE_CONTROLLER && tracked && awake;
}

void controllerClockReset(ControllerClock* c) {
    memset(c, 0, sizeof(*c));
}

int controllerClockStep(ControllerClock* c, float dt, int moved, int pressed, int holding,
                        int triggerDown, int pressWakes, int sleepOn, float wakeSec,
                        float sleepSec, int* swallow) {
    int event = CLOCK_SAME;
    if (pressWakes && pressed && !c->awake) {
        c->awake = 1;
        c->movingFor = 0.0f;
        c->stillFor = 0.0f;
        c->wakeTrigger = triggerDown;
        event = CLOCK_PRESSED;
    }
    if (moved) {
        c->movingFor += dt;
        c->stillFor = 0.0f;
        if (!pressWakes && !c->awake && c->movingFor >= wakeSec) {
            c->awake = 1;
            event = CLOCK_PICKED_UP;
        }
    }
    else if (c->awake && (holding || pressed || !sleepOn)) {
        // A drag or a long scroll holds a controller dead still for longer
        // than the sleep, and dropping its ray under the user would end both
        c->stillFor = 0.0f;
        c->movingFor = 0.0f;
    }
    else {
        c->stillFor += dt;
        c->movingFor = 0.0f;
        if (c->awake && sleepOn && c->stillFor >= sleepSec) {
            c->awake = 0;
            event = CLOCK_PUT_DOWN;
        }
    }
    // Held rather than spent once, so the press that woke it never lands, and
    // letting go hands the trigger back
    if (!triggerDown) {
        c->wakeTrigger = 0;
    }
    *swallow = triggerDown && c->wakeTrigger;
    return event;
}

int gazeBridgeUpdate(GazeBridge* b, int gazeOn, int controllerAwake, int focused,
                     int pressHeld, long nowNs, float bridgeSec, float* outSec) {
    *outSec = 0.0f;
    if (!gazeOn) {
        // The hands point anyway with no eyes, so dropping it switches nothing
        b->missingSince = 0;
        b->back = 0;
        if (b->bridged) {
            b->bridged = 0;
            *outSec = (float)((nowNs - b->bridgedSince) * 1e-9);
            return BRIDGE_OFF;
        }
        return BRIDGE_SAME;
    }
    if (pressHeld) {
        return BRIDGE_SAME;
    }
    if (b->bridged) {
        if (b->back) {
            b->bridged = 0;
            *outSec = (float)((nowNs - b->bridgedSince) * 1e-9);
            return BRIDGE_OFF;
        }
        return BRIDGE_SAME;
    }
    // Only while the eyes are the pointer and the session is being watched
    if (b->missingSince == 0 || controllerAwake || !focused) {
        return BRIDGE_SAME;
    }
    float missing = (float)((nowNs - b->missingSince) * 1e-9);
    if (missing < bridgeSec) {
        return BRIDGE_SAME;
    }
    b->bridged = 1;
    b->bridgedSince = nowNs;
    *outSec = missing;
    return BRIDGE_ON;
}

void gazeBridgeTrack(GazeBridge* b, int asked, int usable, long nowNs) {
    if (!asked) {
        b->missingSince = 0;
        b->back = 0;
        return;
    }
    b->back = usable;
    if (usable) {
        b->missingSince = 0;
    }
    else if (b->missingSince == 0) {
        b->missingSince = nowNs;
    }
}
