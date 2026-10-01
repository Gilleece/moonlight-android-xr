// The panels' fades and the splash's floor, ceiling, fade and dots, checked
// against a clock stepped by hand
#include "check.h"
#include "xr_notice.h"

#define MS 1000000LL
// A frame at 72 Hz, near enough
#define FRAME (14 * MS)

static void testFadeRisesAndLands(void) {
    Fade fade = { 0 };
    long now = 1000 * MS;
    // The first frame has nothing to measure against, so it does not move
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 1) == FADE_NONE);
    CHECK_NEAR(fade.level, 0.0, 1e-6);

    int done = FADE_NONE;
    int frames = 0;
    while (done == FADE_NONE && frames < 100) {
        now += FRAME;
        done = fadeStep(&fade, 1, now, FADE_NS, 1);
        frames++;
        if (done == FADE_NONE) {
            CHECK(fade.level > 0.0f && fade.level < 1.0f);
        }
    }
    CHECK(done == FADE_IN_DONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
    // 150 ms at 14 ms a frame lands on the eleventh frame after the first,
    // which began the run
    CHECK(frames == 11);
    CHECK(fade.frames == 12);
    CHECK(now - fade.fromNs == 11 * FRAME);

    // Held up, nothing more is said
    now += FRAME;
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 1) == FADE_NONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
}

static void testFadeFallsAndTurnsRound(void) {
    Fade fade = { 0 };
    long now = 5000 * MS;
    fadeStep(&fade, 1, now, FADE_NS, 0);
    CHECK_NEAR(fade.level, 1.0, 1e-6);

    // Halfway out, then shown again: it turns round from where it got to
    // rather than jumping, and the run starts again
    now += FRAME;
    fadeStep(&fade, 0, now, FADE_NS, 1);
    for (int i = 0; i < 4; i++) {
        now += FRAME;
        CHECK(fadeStep(&fade, 0, now, FADE_NS, 1) == FADE_NONE);
    }
    float halfway = fade.level;
    CHECK_NEAR(halfway, 1.0 - 5.0 * 14.0 / 150.0, 1e-4);
    now += FRAME;
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 1) == FADE_NONE);
    CHECK(fade.level > halfway);
    CHECK(fade.fromNs == now);
    CHECK(fade.frames == 1);

    int done = FADE_NONE;
    for (int i = 0; i < 100 && done == FADE_NONE; i++) {
        now += FRAME;
        done = fadeStep(&fade, 1, now, FADE_NS, 1);
    }
    CHECK(done == FADE_IN_DONE);

    // And all the way out
    done = FADE_NONE;
    for (int i = 0; i < 100 && done == FADE_NONE; i++) {
        now += FRAME;
        done = fadeStep(&fade, 0, now, FADE_NS, 1);
    }
    CHECK(done == FADE_OUT_DONE);
    CHECK_NEAR(fade.level, 0.0, 1e-6);
}

static void testFadeWithoutTheExtensionCuts(void) {
    // A runtime that cannot fade a layer shows it or does not, the way the
    // panels always behaved
    Fade fade = { 0 };
    long now = 100 * MS;
    CHECK(fadeStep(&fade, 1, now, FADE_NS, 0) == FADE_NONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
    now += FRAME;
    CHECK(fadeStep(&fade, 0, now, FADE_NS, 0) == FADE_NONE);
    CHECK_NEAR(fade.level, 0.0, 1e-6);
    // A zero length fade is the same thing
    now += FRAME;
    CHECK(fadeStep(&fade, 1, now, 0, 1) == FADE_NONE);
    CHECK_NEAR(fade.level, 1.0, 1e-6);
}

static void testSplashHoldsItsFloor(void) {
    // Everything ready on the first frame still shows the splash for the floor
    Splash splash = { 0 };
    long start = 2000 * MS;
    long now = start;
    CHECK(splashStep(&splash, now, 0, SPLASH_FADE_NS) == 0);
    CHECK(splash.firstNs == start);
    CHECK_NEAR(splashLevel(&splash, now, SPLASH_FADE_NS), 1.0, 1e-6);
    int lifted = 0;
    while (!lifted && now - start < 10000 * MS) {
        now += FRAME;
        lifted = splashStep(&splash, now, 0, SPLASH_FADE_NS);
    }
    CHECK(lifted);
    CHECK(now - start >= SPLASH_FLOOR_NS);
    CHECK(now - start < SPLASH_FLOOR_NS + FRAME);
    CHECK(splash.waitingAtLift == 0);
    CHECK(splash.phase == SPLASH_FADING);

    // Then fades over its 300 ms and is gone
    CHECK_NEAR(splashLevel(&splash, now, SPLASH_FADE_NS), 1.0, 1e-6);
    CHECK_NEAR(splashLevel(&splash, now + 150 * MS, SPLASH_FADE_NS), 0.5, 1e-4);
    now += 299 * MS;
    splashStep(&splash, now, 0, SPLASH_FADE_NS);
    CHECK(splash.phase == SPLASH_FADING);
    now += 2 * MS;
    CHECK(splashStep(&splash, now, 0, SPLASH_FADE_NS) == 0);
    CHECK(splash.phase == SPLASH_GONE);
    CHECK_NEAR(splashLevel(&splash, now, SPLASH_FADE_NS), 0.0, 1e-6);
}

static void testSplashWaitsThenGivesUp(void) {
    // Still waiting past the floor keeps it up, and the last thing to land
    // lets it go
    Splash splash = { 0 };
    long start = 100 * MS;
    splashStep(&splash, start, SPLASH_WAIT_PANELS | SPLASH_WAIT_DEPTH, SPLASH_FADE_NS);
    CHECK(splashStep(&splash, start + 3000 * MS, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS) == 0);
    CHECK(splash.phase == SPLASH_UP);
    CHECK(splashStep(&splash, start + 3100 * MS, 0, SPLASH_FADE_NS) == 1);
    CHECK(splash.waitingAtLift == 0);
    CHECK(splash.liftNs == start + 3100 * MS);

    // A model that never answers is given up on at the ceiling, and what it
    // was waiting on is kept for the log
    Splash stuck = { 0 };
    splashStep(&stuck, start, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS);
    CHECK(splashStep(&stuck, start + SPLASH_CEILING_NS - MS, SPLASH_WAIT_DEPTH,
                     SPLASH_FADE_NS) == 0);
    CHECK(splashStep(&stuck, start + SPLASH_CEILING_NS, SPLASH_WAIT_DEPTH, SPLASH_FADE_NS) == 1);
    CHECK(stuck.waitingAtLift == SPLASH_WAIT_DEPTH);
    // Said once only
    CHECK(splashStep(&stuck, start + SPLASH_CEILING_NS + FRAME, SPLASH_WAIT_DEPTH,
                     SPLASH_FADE_NS) == 0);
}

static void testSplashCutsWithoutAFade(void) {
    Splash splash = { 0 };
    splashStep(&splash, 10 * MS, 0, 0);
    CHECK(splashStep(&splash, 10 * MS + SPLASH_FLOOR_NS, 0, 0) == 1);
    CHECK(splash.phase == SPLASH_GONE);
    CHECK_NEAR(splashLevel(&splash, 10 * MS + SPLASH_FLOOR_NS, 0), 0.0, 1e-6);
}

static void testSplashDotsStep(void) {
    Splash splash = { 0 };
    CHECK(splashRow(&splash, 500 * MS, 3) == 0);
    long start = 1000 * MS;
    splashStep(&splash, start, SPLASH_WAIT_ROOM, SPLASH_FADE_NS);
    CHECK(splashRow(&splash, start, 3) == 0);
    CHECK(splashRow(&splash, start + 299 * MS, 3) == 0);
    CHECK(splashRow(&splash, start + 300 * MS, 3) == 1);
    CHECK(splashRow(&splash, start + 600 * MS, 3) == 2);
    CHECK(splashRow(&splash, start + 900 * MS, 3) == 0);
    CHECK(splashRow(&splash, start + 1250 * MS, 3) == 1);
    CHECK(splashRow(&splash, start, 0) == 0);
}

int main(void) {
    testFadeRisesAndLands();
    testFadeFallsAndTurnsRound();
    testFadeWithoutTheExtensionCuts();
    testSplashHoldsItsFloor();
    testSplashWaitsThenGivesUp();
    testSplashCutsWithoutAFade();
    testSplashDotsStep();
    return checksDone("xr_notice");
}
