// The display refresh rate choice and the frame budget behind it, against the
// rate lists the headsets actually offer
#include "check.h"
#include "xr_rate.h"

static const float QUEST2[] = { 60.0f, 72.0f, 80.0f, 90.0f, 120.0f };
static const float QUEST3[] = { 72.0f, 80.0f, 90.0f, 120.0f };
static const float PICO[] = { 72.0f, 90.0f };
// A runtime that reports its rates the way a video standard would
static const float NTSC[] = { 71.93f, 89.91f, 119.88f };

#define N(list) ((int)(sizeof(list) / sizeof(list[0])))

static void testStreamGetsItsOwnRate(void) {
    CHECK_NEAR(rateForStream(120, QUEST3, N(QUEST3)), 120, 0);
    CHECK_NEAR(rateForStream(90, QUEST3, N(QUEST3)), 90, 0);
    CHECK_NEAR(rateForStream(72, QUEST3, N(QUEST3)), 72, 0);
    CHECK_NEAR(rateForStream(60, QUEST2, N(QUEST2)), 60, 0);
    CHECK_NEAR(rateForStream(120, QUEST2, N(QUEST2)), 120, 0);
    CHECK_NEAR(rateForStream(90, PICO, N(PICO)), 90, 0);
    // Close enough counts, and the runtime's own value is what is asked for
    CHECK_NEAR(rateForStream(120, NTSC, N(NTSC)), 119.88f, 1e-4);
    CHECK_NEAR(rateForStream(90, NTSC, N(NTSC)), 89.91f, 1e-4);
}

static void testNoMatchTakesTheNearestAbove(void) {
    // A 60 fps stream on a headset without 60 goes to the nearest rate that
    // still shows every frame
    CHECK_NEAR(rateForStream(60, QUEST3, N(QUEST3)), 72, 0);
    CHECK_NEAR(rateForStream(60, PICO, N(PICO)), 72, 0);
    CHECK_NEAR(rateForStream(30, QUEST2, N(QUEST2)), 60, 0);
    CHECK_NEAR(rateForStream(30, QUEST3, N(QUEST3)), 72, 0);
    CHECK_NEAR(rateForStream(100, QUEST3, N(QUEST3)), 120, 0);
    CHECK_NEAR(rateForStream(75, QUEST2, N(QUEST2)), 80, 0);
}

static void testFasterThanAnythingTakesTheHighest(void) {
    CHECK_NEAR(rateForStream(120, PICO, N(PICO)), 90, 0);
    CHECK_NEAR(rateForStream(144, QUEST3, N(QUEST3)), 120, 0);
    CHECK_NEAR(rateForStream(240, NTSC, N(NTSC)), 119.88f, 1e-4);
}

static void testNothingToAskFor(void) {
    CHECK(rateForStream(90, QUEST3, 0) == 0.0f);
    CHECK(rateForStream(90, NULL, 4) == 0.0f);
    CHECK(rateForStream(0, QUEST3, N(QUEST3)) == 0.0f);
    CHECK(rateForStream(-60, QUEST3, N(QUEST3)) == 0.0f);
    float junk[] = { 0.0f, -1.0f };
    CHECK(rateForStream(90, junk, 2) == 0.0f);
    CHECK(rateChoose(90, QUEST3, 0, 1, 0, 20) == 0.0f);
}

static void testOffered(void) {
    CHECK_NEAR(rateOffered(80, QUEST3, N(QUEST3)), 80, 0);
    CHECK_NEAR(rateOffered(120.3f, QUEST3, N(QUEST3)), 120, 0);
    CHECK(rateOffered(60, QUEST3, N(QUEST3)) == 0.0f);
    CHECK(rateOffered(0, QUEST3, N(QUEST3)) == 0.0f);
}

static void testStepDownStopsAtTheFloor(void) {
    CHECK_NEAR(rateStepDown(120, QUEST3, N(QUEST3), RATE_FLOOR_HZ), 90, 0);
    CHECK_NEAR(rateStepDown(90, QUEST3, N(QUEST3), RATE_FLOOR_HZ), 80, 0);
    CHECK_NEAR(rateStepDown(80, QUEST3, N(QUEST3), RATE_FLOOR_HZ), 72, 0);
    CHECK(rateStepDown(72, QUEST3, N(QUEST3), RATE_FLOOR_HZ) == 0.0f);
    // The Quest 2's 60 is under the floor, so 72 is as low as the budget goes
    CHECK(rateStepDown(72, QUEST2, N(QUEST2), RATE_FLOOR_HZ) == 0.0f);
    CHECK_NEAR(rateStepDown(90, PICO, N(PICO), RATE_FLOOR_HZ), 72, 0);
    // A floor a hair over the runtime's own value still lets it through
    CHECK_NEAR(rateStepDown(89.91f, NTSC, N(NTSC), RATE_FLOOR_HZ), 71.93f, 1e-4);
    CHECK(rateStepDown(90, NULL, 3, RATE_FLOOR_HZ) == 0.0f);
}

static void testWarpOffIsTheStreamRate(void) {
    // However slow the frames, with the 3D off the stream decides alone
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 0, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 0, 90, 30), 120, 0);
    CHECK_NEAR(rateChoose(60, QUEST2, N(QUEST2), 0, 0, 30), 60, 0);
}

static void testWarpOnStartsAtTheStreamRate(void) {
    // Nothing measured yet, so the stream's rate is tried first
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(90, QUEST3, N(QUEST3), 1, 0, 0), 90, 0);
    // A frame that fits its period keeps the rate, edge included
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 0, 8.0f), 120, 0);
    CHECK_NEAR(rateChoose(90, QUEST3, N(QUEST3), 1, 0, 11.1f), 90, 0);
}

static void testWarpOverBudgetStepsDownOne(void) {
    // 9 ms does not fit 120 Hz's 8.3, so one step, not straight to the floor
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 0, 9.0f), 90, 0);
    // 12.35 ms, the model on every frame of a 4K stream on the Quest 3,
    // does not fit 90 Hz either, and the next step is 80
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 90, 12.35f), 80, 0);
    CHECK_NEAR(rateChoose(90, PICO, N(PICO), 1, 0, 12.0f), 72, 0);
    CHECK_NEAR(rateChoose(120, NTSC, N(NTSC), 1, 0, 9.0f), 89.91f, 1e-4);
}

static void testWarpNeverClimbsBackPastTheHeldRate(void) {
    // Stepped down to 90 once, the warp stays there however quick it looks
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 90, 0), 90, 0);
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 90, 4.0f), 90, 0);
    // A held rate above the stream's does not raise it
    CHECK_NEAR(rateChoose(72, QUEST3, N(QUEST3), 1, 90, 0), 72, 0);
    // A held rate the runtime does not offer is ignored rather than asked for
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 85, 0), 120, 0);
}

static void testFloorHolds(void) {
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 72, 20.0f), 72, 0);
    CHECK_NEAR(rateChoose(72, QUEST2, N(QUEST2), 1, 0, 20.0f), 72, 0);
    // A stream under the floor keeps its own rate and is never pushed lower
    CHECK_NEAR(rateChoose(60, QUEST2, N(QUEST2), 1, 0, 20.0f), 60, 0);
}

// Feeds a whole window of frames at a CPU and a GPU time, per frame at hz
static int feedWindow(RateBudget* b, long* now, float hz, float cpuMs, float gpuMs,
                      int warps) {
    long period = (long)(1e9 / hz);
    long end = *now + RATE_WINDOW_NS;
    int verdict = RATE_WINDOW_FILLING;
    int frame = 0;
    int frames = (int)(RATE_WINDOW_NS / period);
    while (verdict == RATE_WINDOW_FILLING) {
        rateBudgetCpu(b, (long)(cpuMs * 1e6));
        // Spread the warps over the window, the way a stream at a lower rate
        // than the display lands
        if (warps > 0 && (long)frame * warps / frames != (long)(frame + 1) * warps / frames) {
            rateBudgetGpu(b, (long)(gpuMs * 1e6));
        }
        frame++;
        *now += period;
        verdict = rateBudgetTick(b, *now, hz);
        CHECK(*now <= end + period);
    }
    return verdict;
}

static void testBudgetSettlesThenHolds(void) {
    RateBudget b;
    long now = 1000000000L;
    rateBudgetStart(&b, now, RATE_SETTLE_WINDOWS);
    CHECK(rateBudgetTick(&b, now + 1, 90) == RATE_WINDOW_FILLING);
    // Thrown away however bad it was
    CHECK(feedWindow(&b, &now, 90, 1.0f, 30.0f, 60) == RATE_WINDOW_SETTLING);
    CHECK_NEAR(b.frameMs, 30.0f, 0.01);
    CHECK(feedWindow(&b, &now, 90, 1.0f, 5.8f, 60) == RATE_WINDOW_HELD);
    CHECK_NEAR(b.gpuMs, 5.8f, 0.01);
    CHECK_NEAR(b.cpuMs, 1.0f, 0.01);
    CHECK_NEAR(b.frameMs, 5.8f, 0.01);
    CHECK(b.lastGpuFrames >= 59 && b.lastGpuFrames <= 61);
}

static void testBudgetNeedsWindowsInARow(void) {
    RateBudget b;
    long now = 0;
    rateBudgetStart(&b, now, 0);
    // Two over, then a good one resets the run
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
    CHECK(feedWindow(&b, &now, 120, 1.0f, 7.0f, 120) == RATE_WINDOW_HELD);
    // Then RATE_OVER_WINDOWS in a row is the call to step down
    for (int i = 1; i < RATE_OVER_WINDOWS; i++) {
        CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
    }
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_OVER);
    // And the count starts again after it
    CHECK(feedWindow(&b, &now, 120, 1.0f, 9.0f, 120) == RATE_WINDOW_SLIPPING);
}

static void testBudgetTakesTheSlowerSide(void) {
    RateBudget b;
    long now = 0;
    rateBudgetStart(&b, now, 0);
    // A frame loop slow on the CPU is over even with a quick GPU
    CHECK(feedWindow(&b, &now, 90, 12.0f, 2.0f, 60) == RATE_WINDOW_SLIPPING);
    CHECK_NEAR(b.frameMs, 12.0f, 0.01);
}

static void testBudgetIgnoresAFewWarps(void) {
    RateBudget b;
    long now = 0;
    rateBudgetStart(&b, now, 0);
    // A desktop standing still: a handful of slow warps prove nothing
    CHECK(feedWindow(&b, &now, 90, 1.0f, 20.0f, RATE_MIN_GPU_SAMPLES - 5) == RATE_WINDOW_HELD);
    CHECK(b.gpuMs == 0.0f);
    CHECK_NEAR(b.frameMs, 1.0f, 0.01);
}

static void testBudgetAddsTheRoom(void) {
    RateBudget b;
    long now = 0;
    rateBudgetStart(&b, now, 0);
    for (int i = 0; i < 60; i++) {
        rateBudgetRoom(&b, 3000000L);
    }
    // 6 ms of warp and 3 of room is 9, over 120 Hz's period
    CHECK(feedWindow(&b, &now, 120, 1.0f, 6.0f, 60) == RATE_WINDOW_SLIPPING);
    CHECK_NEAR(b.gpuMs, 9.0f, 0.01);
}

static void testBudgetCountsMissedRefreshes(void) {
    RateBudget b;
    long now = 0;
    rateBudgetStart(&b, now, 0);
    rateBudgetMissed(&b, 2);
    rateBudgetMissed(&b, 0);
    rateBudgetMissed(&b, -3);
    rateBudgetMissed(&b, 1);
    feedWindow(&b, &now, 90, 1.0f, 1.0f, 60);
    CHECK(b.lastMissed == 3);
    // A fresh window starts from none
    feedWindow(&b, &now, 90, 1.0f, 1.0f, 60);
    CHECK(b.lastMissed == 0);
}

// A window at hz where every frame takes a quick warp but one frame in
// lateEvery misses the refresh after it
static int feedLate(RateBudget* b, long* now, float hz, int lateEvery) {
    long period = (long)(1e9 / hz);
    int verdict = RATE_WINDOW_FILLING;
    int frame = 0;
    while (verdict == RATE_WINDOW_FILLING) {
        rateBudgetCpu(b, 1000000L);
        rateBudgetGpu(b, 3000000L);
        int late = lateEvery > 0 && frame % lateEvery == 0;
        *now += late ? 2 * period : period;
        rateBudgetMissed(b, late ? 1 : 0);
        frame++;
        verdict = rateBudgetTick(b, *now, hz);
    }
    return verdict;
}

static void testBudgetCountsABusyGpuByItsPace(void) {
    RateBudget b;
    long now = 0;
    rateBudgetStart(&b, now, 0);
    // Every warp fits 80 Hz's 12.5 ms, but a third of the refreshes go by
    // without a frame: the loop's pace is the frame time then
    CHECK(feedLate(&b, &now, 80, 2) == RATE_WINDOW_SLIPPING);
    CHECK_NEAR(b.gpuMs, 3.0f, 0.01);
    CHECK_NEAR(b.paceMs, 18.75f, 0.2);
    CHECK_NEAR(b.frameMs, b.paceMs, 1e-6);
    CHECK(b.lastMissed * 3 >= b.lastFrames + b.lastMissed - 2);
    // A miss in twenty is a hiccup and the warp's time stands
    rateBudgetStart(&b, now, 0);
    CHECK(feedLate(&b, &now, 80, 20) == RATE_WINDOW_HELD);
    CHECK_NEAR(b.frameMs, 3.0f, 0.01);
    CHECK(b.paceMs > 12.5f);
    // Exactly the share is not past it, one in eight is
    rateBudgetStart(&b, now, 0);
    CHECK(feedLate(&b, &now, 80, 9) == RATE_WINDOW_HELD);
    rateBudgetStart(&b, now, 0);
    CHECK(feedLate(&b, &now, 80, 8) == RATE_WINDOW_SLIPPING);
    CHECK(b.frameMs > 12.5f);
    // And the stepped down rate it leads to, through the same choice
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 80, b.frameMs), 72, 0);
}

static void testBudgetWithNothingMeasured(void) {
    RateBudget b;
    long now = 0;
    rateBudgetStart(&b, now, 0);
    now += RATE_WINDOW_NS;
    CHECK(rateBudgetTick(&b, now, 90) == RATE_WINDOW_HELD);
    CHECK(b.frameMs == 0.0f);
    // And with no rate to judge against, however slow the frames, nothing is
    // over
    rateBudgetStart(&b, now, 0);
    for (int i = 0; i < 100; i++) {
        rateBudgetCpu(&b, 30000000L);
    }
    now += RATE_WINDOW_NS;
    CHECK(rateBudgetTick(&b, now, 0) == RATE_WINDOW_HELD);
    CHECK_NEAR(b.frameMs, 30.0f, 0.01);
}

// The rate a session starts on is never news: a change only goes on the toast
// once the session has drawn a focused frame and its first request has landed
static void testTheFirstRateIsNoNews(void) {
    // Asked for 90, not focused yet, landed or not: not settled
    CHECK(!rateSettled(0, 0, 90.0f, 0));
    CHECK(!rateSettled(0, 0, 90.0f, 1));
    // Focused but the request still on its way: not yet, so its landing is
    // not toasted
    CHECK(!rateSettled(0, 1, 90.0f, 0));
    // Focused with it landed, found in force or given up on
    CHECK(rateSettled(0, 1, 90.0f, 1));
    // Nothing asked for, as without the extension: the focused frame will do
    CHECK(rateSettled(0, 1, 0.0f, 0));
    CHECK(!rateSettled(0, 0, 0.0f, 0));
    CHECK(rateSettled(0, 1, NAN, 0));
    // And once settled a later request on its way, a step down or a forced
    // rate, does not unsettle it, so its landing is toasted
    CHECK(rateSettled(1, 1, 72.0f, 0));
    CHECK(rateSettled(1, 0, 72.0f, 0));

    // A session start frame by frame, the way the frame loop latches it
    int settled = 0;
    int toasts = 0;
    // Asked at session begin, before focus; lands while visible
    float asked = 90.0f;
    int confirmed = 0;
    settled = rateSettled(settled, 0, asked, confirmed);
    confirmed = 1;
    toasts += settled;
    settled = rateSettled(settled, 0, asked, confirmed);
    CHECK(!settled);
    // The first focused frame settles it
    settled = rateSettled(settled, 1, asked, confirmed);
    CHECK(settled);
    // A forced 120 mid session: asked again, its landing toasted
    asked = 120.0f;
    confirmed = 0;
    settled = rateSettled(settled, 1, asked, confirmed);
    toasts += settled;
    CHECK(toasts == 1);
}

int main(void) {
    testStreamGetsItsOwnRate();
    testNoMatchTakesTheNearestAbove();
    testFasterThanAnythingTakesTheHighest();
    testNothingToAskFor();
    testOffered();
    testStepDownStopsAtTheFloor();
    testWarpOffIsTheStreamRate();
    testWarpOnStartsAtTheStreamRate();
    testWarpOverBudgetStepsDownOne();
    testWarpNeverClimbsBackPastTheHeldRate();
    testFloorHolds();
    testBudgetSettlesThenHolds();
    testBudgetNeedsWindowsInARow();
    testBudgetTakesTheSlowerSide();
    testBudgetIgnoresAFewWarps();
    testBudgetAddsTheRoom();
    testBudgetCountsMissedRefreshes();
    testBudgetCountsABusyGpuByItsPace();
    testBudgetWithNothingMeasured();
    testTheFirstRateIsNoNews();
    return checksDone("xr_rate");
}
