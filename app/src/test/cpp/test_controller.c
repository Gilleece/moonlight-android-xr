// The controllers as drawn: when the ray's beam goes up
#include "check.h"
#include "xr_controller.h"

static void testRaySwitch(void) {
    // The setting is where a session starts, and each flip turns it over
    CHECK(raySwitchOn(1, 0) == 1);
    CHECK(raySwitchOn(1, 1) == 0);
    CHECK(raySwitchOn(0, 0) == 0);
    CHECK(raySwitchOn(0, 1) == 1);
    // Any non zero reads as set
    CHECK(raySwitchOn(5, 0) == 1);
    CHECK(raySwitchOn(5, 7) == 0);

    // The flip a row's cell asks for lands the switch on that cell, from
    // either setting
    for (int setting = 0; setting < 2; setting++) {
        for (int want = 0; want < 2; want++) {
            CHECK(raySwitchOn(setting, rayFlipFor(setting, want)) == want);
        }
    }
    // Asking for what is already in force asks for no flip
    CHECK(rayFlipFor(1, 1) == 0);
    CHECK(rayFlipFor(0, 0) == 0);
}

static void testRayDrawn(void) {
    // Every combination, against the rule written out: the beam shows while
    // the switch is on, and while any panel is up whatever the switch says
    for (int setting = 0; setting < 2; setting++) {
        for (int flipped = 0; flipped < 2; flipped++) {
            for (int panel = 0; panel < 2; panel++) {
                int on = setting != flipped;
                CHECK(rayDrawn(setting, flipped, panel) == (on || panel));
            }
        }
    }

    // A session started with the setting off: hidden, back while the cog is
    // open, hidden again once it closes
    CHECK(!rayDrawn(0, 0, 0));
    CHECK(rayDrawn(0, 0, 1));
    CHECK(!rayDrawn(0, 0, 0));
    // The bar button turns it on for the session, and a panel changes nothing
    CHECK(rayDrawn(0, 1, 0));
    CHECK(rayDrawn(0, 1, 1));
    // Started on and switched off from the bar, the same as the setting off
    CHECK(!rayDrawn(1, 1, 0));
    CHECK(rayDrawn(1, 1, 1));
}

int main(void) {
    testRaySwitch();
    testRayDrawn();
    return checksDone("xr_controller");
}
