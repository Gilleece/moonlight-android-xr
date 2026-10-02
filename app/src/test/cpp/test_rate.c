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
    // Whether or not a multiple may be taken, the stream's own rate comes first
    for (int m = 0; m <= 1; m++) {
        CHECK_NEAR(rateForStream(120, QUEST3, N(QUEST3), m), 120, 0);
        CHECK_NEAR(rateForStream(90, QUEST3, N(QUEST3), m), 90, 0);
        CHECK_NEAR(rateForStream(72, QUEST3, N(QUEST3), m), 72, 0);
        CHECK_NEAR(rateForStream(60, QUEST2, N(QUEST2), m), 60, 0);
        CHECK_NEAR(rateForStream(120, QUEST2, N(QUEST2), m), 120, 0);
        CHECK_NEAR(rateForStream(90, PICO, N(PICO), m), 90, 0);
        // Close enough counts, and the runtime's own value is what is asked for
        CHECK_NEAR(rateForStream(120, NTSC, N(NTSC), m), 119.88f, 1e-4);
        CHECK_NEAR(rateForStream(90, NTSC, N(NTSC), m), 89.91f, 1e-4);
    }
}

static void testNoMatchTakesTheNearestAbove(void) {
    // Without a multiple, a 60 fps stream on a headset without 60 goes to
    // the nearest rate that still shows every frame
    CHECK_NEAR(rateForStream(60, QUEST3, N(QUEST3), 0), 72, 0);
    CHECK_NEAR(rateForStream(60, PICO, N(PICO), 0), 72, 0);
    CHECK_NEAR(rateForStream(30, QUEST2, N(QUEST2), 0), 60, 0);
    CHECK_NEAR(rateForStream(30, QUEST3, N(QUEST3), 0), 72, 0);
    CHECK_NEAR(rateForStream(45, QUEST3, N(QUEST3), 0), 72, 0);
    CHECK_NEAR(rateForStream(100, QUEST3, N(QUEST3), 0), 120, 0);
    CHECK_NEAR(rateForStream(75, QUEST2, N(QUEST2), 0), 80, 0);
}

static void testAWholeMultipleBeforeTheNearestAbove(void) {
    // 60 on 120 shows every frame for two refreshes; on 72 one in five is
    // held twice
    CHECK_NEAR(rateForStream(60, QUEST3, N(QUEST3), 1), 120, 0);
    CHECK_NEAR(rateForStream(45, QUEST3, N(QUEST3), 1), 90, 0);
    CHECK_NEAR(rateForStream(40, QUEST3, N(QUEST3), 1), 80, 0);
    CHECK_NEAR(rateForStream(30, QUEST3, N(QUEST3), 1), 90, 0);
    // The lowest multiple, not the highest
    CHECK_NEAR(rateForStream(24, QUEST3, N(QUEST3), 1), 72, 0);
    CHECK_NEAR(rateForStream(30, QUEST2, N(QUEST2), 1), 60, 0);
    // The runtime's own near multiples count
    CHECK_NEAR(rateForStream(60, NTSC, N(NTSC), 1), 119.88f, 1e-4);
    CHECK_NEAR(rateForStream(24, NTSC, N(NTSC), 1), 71.93f, 1e-4);
    // With no multiple offered, the nearest above as before
    CHECK_NEAR(rateForStream(60, PICO, N(PICO), 1), 72, 0);
    CHECK_NEAR(rateForStream(50, QUEST3, N(QUEST3), 1), 72, 0);
    CHECK_NEAR(rateForStream(100, QUEST3, N(QUEST3), 1), 120, 0);
    CHECK_NEAR(rateForStream(75, QUEST2, N(QUEST2), 1), 80, 0);
}

static void testFasterThanAnythingTakesTheHighest(void) {
    for (int m = 0; m <= 1; m++) {
        CHECK_NEAR(rateForStream(120, PICO, N(PICO), m), 90, 0);
        CHECK_NEAR(rateForStream(144, QUEST3, N(QUEST3), m), 120, 0);
        CHECK_NEAR(rateForStream(240, NTSC, N(NTSC), m), 119.88f, 1e-4);
    }
}

static void testNothingToAskFor(void) {
    for (int m = 0; m <= 1; m++) {
        CHECK(rateForStream(90, QUEST3, 0, m) == 0.0f);
        CHECK(rateForStream(90, NULL, 4, m) == 0.0f);
        CHECK(rateForStream(0, QUEST3, N(QUEST3), m) == 0.0f);
        CHECK(rateForStream(-60, QUEST3, N(QUEST3), m) == 0.0f);
        float junk[] = { 0.0f, -1.0f };
        CHECK(rateForStream(90, junk, 2, m) == 0.0f);
    }
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

static void testTheMultipleUnlessASteppedDownRateIsHeld(void) {
    // A 60 fps stream lands on 120 with the 3D off, or on with nothing held
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 0, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 0, 0), 120, 0);
    CHECK_NEAR(rateChoose(45, QUEST3, N(QUEST3), 1, 0, 0), 90, 0);
    CHECK_NEAR(rateChoose(90, QUEST3, N(QUEST3), 1, 0, 0), 90, 0);
    CHECK_NEAR(rateChoose(24, QUEST3, N(QUEST3), 1, 0, 0), 72, 0);
    // A step down held below the multiple rules it out while the 3D is on
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 90, 0), 72, 0);
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 72, 0), 72, 0);
    CHECK_NEAR(rateChoose(45, QUEST3, N(QUEST3), 1, 80, 0), 72, 0);
    // and with it off the multiple is back
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 0, 90, 30), 120, 0);
    // A held rate at the multiple is not a step down
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 120, 0), 120, 0);
    // The step down still applies from the multiple, one offered rate at a
    // time, the way the budget asks with the rate in force as the held one
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 120, 9.0f), 90, 0);
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 0, 9.0f), 90, 0);
    // and from there the nearest above, which already fits
    CHECK_NEAR(rateChoose(60, QUEST3, N(QUEST3), 1, 90, 12.0f), 72, 0);
}

static void testFloorHolds(void) {
    CHECK_NEAR(rateChoose(120, QUEST3, N(QUEST3), 1, 72, 20.0f), 72, 0);
    CHECK_NEAR(rateChoose(72, QUEST2, N(QUEST2), 1, 0, 20.0f), 72, 0);
    // A stream under the floor keeps its own rate and is never pushed lower
    CHECK_NEAR(rateChoose(60, QUEST2, N(QUEST2), 1, 0, 20.0f), 60, 0);
}

// Feeds a whole window of frames at a CPU and a GPU time, per frame at hz
static int feedWindow(RateBudget* b, int64_t* now, float hz, float cpuMs, float gpuMs,
                      int warps) {
    int64_t period = (int64_t)(1e9 / hz);
    int64_t end = *now + RATE_WINDOW_NS;
    int verdict = RATE_WINDOW_FILLING;
    int frame = 0;
    int frames = (int)(RATE_WINDOW_NS / period);
    while (verdict == RATE_WINDOW_FILLING) {
        rateBudgetCpu(b, (int64_t)(cpuMs * 1e6));
        // Spread the warps over the window, the way a stream at a lower rate
        // than the display lands
        if (warps > 0 && (long)frame * warps / frames != (long)(frame + 1) * warps / frames) {
            rateBudgetGpu(b, (int64_t)(gpuMs * 1e6));
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
    int64_t now = 1000000000LL;
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
    int64_t now = 0;
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
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    // A frame loop slow on the CPU is over even with a quick GPU
    CHECK(feedWindow(&b, &now, 90, 12.0f, 2.0f, 60) == RATE_WINDOW_SLIPPING);
    CHECK_NEAR(b.frameMs, 12.0f, 0.01);
}

static void testBudgetIgnoresAFewWarps(void) {
    RateBudget b;
    int64_t now = 0;
    rateBudgetStart(&b, now, 0);
    // A desktop standing still: a handful of slow warps prove nothing
    CHECK(feedWindow(&b, &now, 90, 1.0f, 20.0f, RATE_MIN_GPU_SAMPLES - 5) == RATE_WINDOW_HELD);
    CHECK(b.gpuMs == 0.0f);
    CHECK_NEAR(b.frameMs, 1.0f, 0.01);
}

static void testBudgetAddsTheRoom(void) {
    RateBudget b;
    int64_t now = 0;
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
    int64_t now = 0;
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
static int feedLate(RateBudget* b, int64_t* now, float hz, int lateEvery) {
    int64_t period = (int64_t)(1e9 / hz);
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
    int64_t now = 0;
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
    int64_t now = 0;
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

// A small deterministic jitter, up to amp ns either way
static int64_t jitter(int i, int64_t amp) {
    unsigned h = (unsigned)i * 2654435761u;
    return (int64_t)(h % (unsigned)(2 * amp + 1)) - amp;
}

// Frames at fps for seconds through the gate at perSecond, counting the
// captures and the shortest and longest gap between two
static int gateRun(float fps, int perSecond, float seconds, int64_t amp, int64_t* minGap,
                   int64_t* maxGap) {
    DepthGate g = { 0, 0 };
    int frames = (int)(fps * seconds);
    int taken = 0;
    int64_t last = -1;
    *minGap = INT64_MAX;
    *maxGap = 0;
    for (int i = 0; i < frames; i++) {
        int64_t t = 1000000000LL + (int64_t)(i * (1e9 / fps)) + (i > 0 ? jitter(i, amp) : 0);
        if (depthGateDue(&g, t, perSecond)) {
            if (last >= 0) {
                int64_t gap = t - last;
                *minGap = gap < *minGap ? gap : *minGap;
                *maxGap = gap > *maxGap ? gap : *maxGap;
            }
            last = t;
            taken++;
        }
    }
    return taken;
}

static void testGateHoldsTheRateWhateverTheFrameRate(void) {
    int64_t lo, hi;
    // 20 a second at 60 fps is every third frame, evenly, frame timing
    // jitter and all
    CHECK(gateRun(60, 20, 10, 1000000, &lo, &hi) == 200);
    CHECK(lo >= 48000000 && hi <= 52000000);
    // and at 120 fps it is still 20, where a cadence of 3 would have taken 40
    CHECK(gateRun(120, 20, 10, 1000000, &lo, &hi) == 200);
    CHECK(lo >= 48000000 && hi <= 52000000);
    // 90 fps does not divide by 20, so four and five frames alternate and
    // the average still holds
    int taken = gateRun(90, 20, 10, 500000, &lo, &hi);
    CHECK(taken >= 199 && taken <= 201);
    CHECK(lo >= 44000000 && hi <= 56000000);
    // The Gen 1 start, 12 a second at 72
    taken = gateRun(72, 12, 10, 1000000, &lo, &hi);
    CHECK(taken >= 119 && taken <= 121);
    // A stream slower than the rate gives every frame and no more
    CHECK(gateRun(24, 45, 10, 1000000, &lo, &hi) == 240);
    CHECK(gateRun(24, 20, 10, 0, &lo, &hi) >= 199);
    // The floor and the ceiling
    CHECK(gateRun(60, 5, 10, 1000000, &lo, &hi) == 50);
    taken = gateRun(60, 45, 10, 1000000, &lo, &hi);
    CHECK(taken >= 449 && taken <= 451);
}

// 60 fps content on a 72 Hz frame loop, which only sees a new frame at its
// own refreshes, so the gaps are a refresh or two
static void testGateThroughARefreshGrid(void) {
    DepthGate g = { 0, 0 };
    int taken = 0;
    int64_t refresh = 1000000000LL / 72;
    int64_t lastFrame = -1;
    for (int r = 0; r < 72 * 10; r++) {
        int64_t t = 1000000000LL + r * refresh;
        int64_t frame = (r * refresh) / (1000000000LL / 60);
        if (frame == lastFrame) {
            continue;
        }
        lastFrame = frame;
        taken += depthGateDue(&g, t, 20);
    }
    CHECK(taken >= 199 && taken <= 201);
}

static void testGateAfterAStallDoesNotBurst(void) {
    DepthGate g = { 0, 0 };
    int64_t t = 1000000000LL;
    int64_t frame = 1000000000LL / 60;
    CHECK(depthGateDue(&g, t, 20));
    // Nothing from the decoder for a second
    t += 1000000000LL;
    CHECK(depthGateDue(&g, t, 20));
    // and the next frames go back to every third, not one each to catch up
    int taken = 0;
    for (int i = 1; i <= 6; i++) {
        taken += depthGateDue(&g, t + i * frame, 20);
    }
    CHECK(taken == 2);
}

static void testGateFollowsATargetChange(void) {
    DepthGate g = { 0, 0 };
    int64_t frame = 1000000000LL / 120;
    int taken = 0;
    for (int i = 0; i < 120; i++) {
        taken += depthGateDue(&g, 1000000000LL + i * frame, 20);
    }
    CHECK(taken == 20);
    // Halved, the next second takes half as many
    taken = 0;
    for (int i = 120; i < 240; i++) {
        taken += depthGateDue(&g, 1000000000LL + i * frame, 10);
    }
    CHECK(taken >= 10 && taken <= 11);
    // Nothing to run at, nothing taken
    CHECK(!depthGateDue(&g, 9000000000LL, 0));
}

static void testGateCountsFromAForcedCapture(void) {
    DepthGate g = { 0, 0 };
    int64_t frame = 1000000000LL / 60;
    int64_t t = 5000000000LL;
    // The 3D back on takes the frame in hand, and the gate counts from it
    depthGateTaken(&g, t, 20);
    CHECK(!depthGateDue(&g, t + frame, 20));
    CHECK(!depthGateDue(&g, t + 2 * frame, 20));
    CHECK(depthGateDue(&g, t + 3 * frame, 20));
}

static void testDepthRateClamp(void) {
    CHECK(depthRateClamp(0) == DEPTH_RATE_MIN);
    CHECK(depthRateClamp(-3) == DEPTH_RATE_MIN);
    CHECK(depthRateClamp(5) == 5);
    CHECK(depthRateClamp(20) == 20);
    CHECK(depthRateClamp(45) == 45);
    CHECK(depthRateClamp(60) == DEPTH_RATE_MAX);
}

#define SEC 1000000000LL

static void testGovernorCutsBeforeTheDisplay(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    CHECK(g.target == 20 && g.cap == 20);
    // The first budget miss halves the depth rate and leaves the display
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 10);
    // A miss inside the 10 s after it is held, to give the cut time to show
    CHECK(depthGovernorWindow(&g, 16 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    CHECK(depthGovernorWindow(&g, 20 * SEC - 1, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    CHECK(g.target == 10);
    // Still missed once it is up: halved again, down to the floor
    CHECK(depthGovernorWindow(&g, 20 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 5);
    CHECK(depthGovernorWindow(&g, 26 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    // and only from the floor, still missed, is it the display's turn
    CHECK(depthGovernorWindow(&g, 32 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_DISPLAY);
    CHECK(g.target == 5);
    CHECK(depthGovernorWindow(&g, 40 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_DISPLAY);
}

// The way the frame loop drives it: a verdict every 2 s window, a miss
// called after RATE_OVER_WINDOWS over in a row. Returns the moves in order.
static int ladder(DepthGovernor* g, int64_t* now, int windows, int over, int live, int* moves,
                  int64_t* when, int max) {
    int n = 0, overRun = 0;
    for (int w = 0; w < windows; w++) {
        *now += RATE_WINDOW_NS;
        int verdict = RATE_WINDOW_HELD;
        if (over) {
            verdict = ++overRun < RATE_OVER_WINDOWS ? RATE_WINDOW_SLIPPING : RATE_WINDOW_OVER;
            if (verdict == RATE_WINDOW_OVER) {
                overRun = 0;
            }
        }
        int move = depthGovernorWindow(g, *now, verdict, live);
        if (move != DEPTH_MOVE_NONE && n < max) {
            moves[n] = move;
            when[n] = *now;
            n++;
        }
    }
    return n;
}

static void testGovernorLadderTiming(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    int64_t now = 0;
    int moves[16];
    int64_t when[16];
    // 60 s over budget from the start
    int n = ladder(&g, &now, 30, 1, 1, moves, when, 16);
    // 6 s: cut to 10. 12 s: held. 18 s: cut to 5. 24 s: held. 30 s on:
    // the display, every 6 s
    CHECK(n >= 5);
    CHECK(moves[0] == DEPTH_MOVE_CUT && when[0] == 6 * SEC);
    CHECK(moves[1] == DEPTH_MOVE_HOLDING && when[1] == 12 * SEC);
    CHECK(moves[2] == DEPTH_MOVE_CUT && when[2] == 18 * SEC);
    CHECK(moves[3] == DEPTH_MOVE_HOLDING && when[3] == 24 * SEC);
    CHECK(moves[4] == DEPTH_MOVE_DISPLAY && when[4] == 30 * SEC);
    CHECK(g.target == 5);

    // Then the budget holds. 30 s of it, counted from the start of the first
    // held window, before the first step back up
    n = ladder(&g, &now, 40, 0, 1, moves, when, 16);
    int64_t heldFrom = 60 * SEC;
    CHECK(n == 2);
    CHECK(moves[0] == DEPTH_MOVE_RAISED && when[0] == heldFrom + 30 * SEC);
    CHECK(moves[1] == DEPTH_MOVE_RAISED && when[1] == heldFrom + 60 * SEC);
    // One step at a time, and never past the preference
    CHECK(g.target == 20);
    n = ladder(&g, &now, 40, 0, 1, moves, when, 16);
    CHECK(n == 0);
    CHECK(g.target == 20);
}

static void testGovernorRecoveryNeedsAnUnbrokenRun(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 45);
    int64_t now = 100 * SEC;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 22);
    // 28 s held, then one window over: the count starts again
    for (int i = 0; i < 14; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    now += RATE_WINDOW_NS;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_SLIPPING, 1) == DEPTH_MOVE_NONE);
    for (int i = 0; i < 14; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    now += RATE_WINDOW_NS;
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_RAISED);
    // Doubled, but capped at the preference
    CHECK(g.target == 44);
    for (int i = 0; i < 15; i++) {
        now += RATE_WINDOW_NS;
        depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1);
    }
    CHECK(g.target == 45);
    // A budget restarted after a rate change or a focus loss breaks the run too
    depthGovernorStart(&g, 20);
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    for (int i = 0; i < 10; i++) {
        now += RATE_WINDOW_NS;
        depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1);
    }
    now += RATE_WINDOW_NS;
    depthGovernorWindow(&g, now, RATE_WINDOW_SETTLING, 1);
    for (int i = 0; i < 14; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    CHECK(g.target == 10);
}

static void testGovernorWithNoModelRunning(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    // 3D off, or the warp on a test pattern: nothing to cut, the display's turn
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 0) == DEPTH_MOVE_DISPLAY);
    CHECK(g.target == 20);
    // and nothing climbs while it is not running
    depthGovernorStart(&g, 20);
    depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1);
    int64_t now = 10 * SEC;
    for (int i = 0; i < 30; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 0) == DEPTH_MOVE_NONE);
    }
    CHECK(g.target == 10);
    // Filling windows are not judged at all
    CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_FILLING, 1) == DEPTH_MOVE_NONE);
}

static void testGovernorFloorAndCap(void) {
    DepthGovernor g;
    // A preference already at the floor goes straight to the display
    depthGovernorStart(&g, 5);
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_DISPLAY);
    // 6 halves to 3, held at 5
    depthGovernorStart(&g, 6);
    CHECK(depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 5);
    // 12, the Gen 1 start: 6, then 5
    depthGovernorStart(&g, 12);
    depthGovernorWindow(&g, 10 * SEC, RATE_WINDOW_OVER, 1);
    CHECK(g.target == 6);
    depthGovernorWindow(&g, 20 * SEC, RATE_WINDOW_OVER, 1);
    CHECK(g.target == 5);
    // Out of range preferences come in range
    depthGovernorStart(&g, 99);
    CHECK(g.cap == 45 && g.target == 45);
    depthGovernorStart(&g, 1);
    CHECK(g.cap == 5 && g.target == 5);
}

static void testGovernorOnAThrottle(void) {
    DepthGovernor g;
    depthGovernorStart(&g, 20);
    // The runtime warning of a throttle cuts at once, without a budget miss
    CHECK(depthGovernorThrottle(&g, 10 * SEC, 1, 1, 1) == DEPTH_MOVE_CUT);
    CHECK(g.target == 10);
    // A second domain a moment later is held like any other move in the hold
    CHECK(depthGovernorThrottle(&g, 11 * SEC, 1, 1, 1) == DEPTH_MOVE_HOLDING);
    CHECK(g.target == 10);
    // and so is the budget missing inside it
    CHECK(depthGovernorWindow(&g, 16 * SEC, RATE_WINDOW_OVER, 1) == DEPTH_MOVE_HOLDING);
    // While throttled nothing climbs, however long the budget holds
    int64_t now = 20 * SEC;
    for (int i = 0; i < 30; i++) {
        now += RATE_WINDOW_NS;
        CHECK(depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_NONE);
    }
    // Back to normal, the run starts then
    CHECK(depthGovernorThrottle(&g, now, 0, 0, 1) == DEPTH_MOVE_NONE);
    int raised = 0;
    for (int i = 0; i < 15; i++) {
        now += RATE_WINDOW_NS;
        raised += depthGovernorWindow(&g, now, RATE_WINDOW_HELD, 1) == DEPTH_MOVE_RAISED;
    }
    CHECK(raised == 1);
    CHECK(g.target == 20);
    // At the floor a throttle has nothing left to cut and never moves the
    // display; nor does one with no model running
    depthGovernorStart(&g, 5);
    CHECK(depthGovernorThrottle(&g, now, 1, 1, 1) == DEPTH_MOVE_NONE);
    depthGovernorStart(&g, 20);
    CHECK(depthGovernorThrottle(&g, now, 1, 1, 0) == DEPTH_MOVE_NONE);
    CHECK(g.target == 20 && g.throttled);
}

int main(void) {
    testStreamGetsItsOwnRate();
    testNoMatchTakesTheNearestAbove();
    testAWholeMultipleBeforeTheNearestAbove();
    testFasterThanAnythingTakesTheHighest();
    testNothingToAskFor();
    testOffered();
    testStepDownStopsAtTheFloor();
    testWarpOffIsTheStreamRate();
    testWarpOnStartsAtTheStreamRate();
    testWarpOverBudgetStepsDownOne();
    testWarpNeverClimbsBackPastTheHeldRate();
    testTheMultipleUnlessASteppedDownRateIsHeld();
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
    testGateHoldsTheRateWhateverTheFrameRate();
    testGateThroughARefreshGrid();
    testGateAfterAStallDoesNotBurst();
    testGateFollowsATargetChange();
    testGateCountsFromAForcedCapture();
    testDepthRateClamp();
    testGovernorCutsBeforeTheDisplay();
    testGovernorLadderTiming();
    testGovernorRecoveryNeedsAnUnbrokenRun();
    testGovernorWithNoModelRunning();
    testGovernorFloorAndCap();
    testGovernorOnAThrottle();
    return checksDone("xr_rate");
}
