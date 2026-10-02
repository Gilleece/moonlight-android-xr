// The display refresh rate choice, the frame budget behind it, and the depth
// rate spent before it. No OpenXR calls and no context, so the host tests
// reach all of it.
#include <math.h>
#include <stddef.h>

#include "xr_rate.h"

float rateOffered(float hz, const float* rates, int count) {
    if (rates == NULL || hz <= 0.0f) {
        return 0.0f;
    }
    for (int i = 0; i < count; i++) {
        if (fabsf(rates[i] - hz) <= RATE_TOLERANCE) {
            return rates[i];
        }
    }
    return 0.0f;
}

// Whether hz shows a stream at fps for the same whole number of refreshes,
// two or more, every frame
static int wholeMultiple(float hz, float fps) {
    float n = roundf(hz / fps);
    return n >= 2.0f && fabsf(hz - n * fps) <= RATE_TOLERANCE;
}

float rateForStream(float fps, const float* rates, int count, int multiples) {
    if (rates == NULL || fps <= 0.0f) {
        return 0.0f;
    }
    float exact = rateOffered(fps, rates, count);
    if (exact > 0.0f) {
        return exact;
    }
    // Nothing matches, so the lowest whole multiple where it may, which
    // judders no more than the stream's own rate would; then the nearest rate
    // that still shows every frame, and failing that the fastest there is,
    // which drops the fewest
    float multiple = 0.0f, above = 0.0f, highest = 0.0f;
    for (int i = 0; i < count; i++) {
        float hz = rates[i];
        if (hz <= 0.0f) {
            continue;
        }
        if (hz > highest) {
            highest = hz;
        }
        if (hz > fps && (above == 0.0f || hz < above)) {
            above = hz;
        }
        if (multiples && wholeMultiple(hz, fps) && (multiple == 0.0f || hz < multiple)) {
            multiple = hz;
        }
    }
    if (multiple > 0.0f) {
        return multiple;
    }
    return above > 0.0f ? above : highest;
}

float rateStepDown(float hz, const float* rates, int count, float floorHz) {
    if (rates == NULL) {
        return 0.0f;
    }
    float best = 0.0f;
    for (int i = 0; i < count; i++) {
        float r = rates[i];
        if (r < hz - RATE_TOLERANCE && r >= floorHz - RATE_TOLERANCE && r > best) {
            best = r;
        }
    }
    return best;
}

float rateChoose(float fps, const float* rates, int count, int warpOn, float heldHz,
                 float frameMs) {
    float want = rateForStream(fps, rates, count, 1);
    if (want <= 0.0f || !warpOn) {
        return want;
    }
    // The multiple costs more refreshes than the rate above the stream, so
    // once the warp has been stepped down below it the stream goes there
    if (heldHz > 0.0f && heldHz < want - RATE_TOLERANCE) {
        want = rateForStream(fps, rates, count, 0);
    }
    // A rate the warp was measured not to hold is not tried again while it
    // runs, so a step down is never undone a few seconds later
    if (heldHz > 0.0f && heldHz < want - RATE_TOLERANCE) {
        float held = rateOffered(heldHz, rates, count);
        want = held > 0.0f ? held : want;
    }
    if (frameMs > 0.0f && frameMs > 1000.0f / want) {
        float lower = rateStepDown(want, rates, count, RATE_FLOOR_HZ);
        if (lower > 0.0f) {
            return lower;
        }
    }
    return want;
}

static void clearWindow(RateBudget* b, int64_t nowNs) {
    b->startNs = nowNs;
    b->cpuTotalNs = 0;
    b->cpuFrames = 0;
    b->gpuTotalNs = 0;
    b->gpuFrames = 0;
    b->roomTotalNs = 0;
    b->roomFrames = 0;
    b->missed = 0;
}

void rateBudgetStart(RateBudget* b, int64_t nowNs, int settle) {
    clearWindow(b, nowNs);
    b->settle = settle;
    b->overWindows = 0;
}

void rateBudgetCpu(RateBudget* b, int64_t frameNs) {
    if (frameNs > 0) {
        b->cpuTotalNs += frameNs;
        b->cpuFrames++;
    }
}

void rateBudgetGpu(RateBudget* b, int64_t gpuNs) {
    if (gpuNs > 0) {
        b->gpuTotalNs += gpuNs;
        b->gpuFrames++;
    }
}

void rateBudgetRoom(RateBudget* b, int64_t gpuNs) {
    if (gpuNs > 0) {
        b->roomTotalNs += gpuNs;
        b->roomFrames++;
    }
}

void rateBudgetMissed(RateBudget* b, long refreshes) {
    if (refreshes > 0) {
        b->missed += refreshes;
    }
}

int rateBudgetTick(RateBudget* b, int64_t nowNs, float hz) {
    if (nowNs - b->startNs < RATE_WINDOW_NS) {
        return RATE_WINDOW_FILLING;
    }

    // The frame time is whichever side is slower: the frame loop on the CPU,
    // or a warp and the room it draws with it on the GPU
    b->cpuMs = b->cpuFrames > 0 ? (float)(b->cpuTotalNs / (double)b->cpuFrames / 1e6) : 0.0f;
    b->gpuMs = 0.0f;
    if (b->gpuFrames >= RATE_MIN_GPU_SAMPLES) {
        b->gpuMs = (float)(b->gpuTotalNs / (double)b->gpuFrames / 1e6);
        if (b->roomFrames > 0) {
            b->gpuMs += (float)(b->roomTotalNs / (double)b->roomFrames / 1e6);
        }
    }
    b->frameMs = b->cpuMs > b->gpuMs ? b->cpuMs : b->gpuMs;
    // And the loop's own pace, once it misses refreshes often enough that
    // the GPU being busy end to end is the likelier story than a hiccup
    b->paceMs = b->cpuFrames > 0
            ? (float)((nowNs - b->startNs) / (double)b->cpuFrames / 1e6) : 0.0f;
    if (b->missed * 100 > (b->cpuFrames + b->missed) * RATE_MISSED_PERCENT
            && b->paceMs > b->frameMs) {
        b->frameMs = b->paceMs;
    }
    b->lastMissed = b->missed;
    b->lastFrames = b->cpuFrames;
    b->lastGpuFrames = b->gpuFrames;
    clearWindow(b, nowNs);

    if (b->settle > 0) {
        b->settle--;
        return RATE_WINDOW_SETTLING;
    }
    if (hz <= 0.0f || b->frameMs <= 1000.0f / hz) {
        b->overWindows = 0;
        return RATE_WINDOW_HELD;
    }
    if (++b->overWindows < RATE_OVER_WINDOWS) {
        return RATE_WINDOW_SLIPPING;
    }
    b->overWindows = 0;
    return RATE_WINDOW_OVER;
}

int rateSettled(int settled, int focusedFrame, float asked, int confirmed) {
    if (settled) {
        return 1;
    }
    return focusedFrame && (!(asked > 0.0f) || confirmed);
}

int depthRateClamp(int perSecond) {
    return perSecond < DEPTH_RATE_MIN ? DEPTH_RATE_MIN
            : perSecond > DEPTH_RATE_MAX ? DEPTH_RATE_MAX : perSecond;
}

int depthGateDue(DepthGate* g, int64_t nowNs, int perSecond) {
    int64_t gap = g->lastNs != 0 ? nowNs - g->lastNs : 0;
    g->lastNs = nowNs;
    if (perSecond <= 0) {
        return 0;
    }
    int64_t period = 1000000000LL / perSecond;
    // Taken a little early when this frame is nearer the due time than the
    // next one is likely to be, so a frame landing just short of it is not
    // passed over for one a whole frame late
    int64_t slack = (gap < period ? gap : period) / 2;
    if (g->dueNs != 0 && nowNs + slack < g->dueNs) {
        return 0;
    }
    g->dueNs = g->dueNs == 0 || nowNs - g->dueNs >= period ? nowNs + period
            : g->dueNs + period;
    return 1;
}

void depthGateTaken(DepthGate* g, int64_t nowNs, int perSecond) {
    g->lastNs = nowNs;
    g->dueNs = perSecond > 0 ? nowNs + 1000000000LL / perSecond : 0;
}

void depthGovernorStart(DepthGovernor* g, int cap) {
    g->cap = depthRateClamp(cap);
    g->target = g->cap;
    g->cutNs = 0;
    g->heldSinceNs = 0;
    g->throttled = 0;
    g->failedTarget = 0;
    g->recoverNs = DEPTH_RECOVER_NS;
    g->raisedNs = 0;
    g->cappedSaid = 0;
}

int depthGovernorSetCap(DepthGovernor* g, int cap) {
    if (depthRateClamp(cap) == g->cap) {
        return 0;
    }
    // The runtime's throttle is not the setting's to forget
    int throttled = g->throttled;
    depthGovernorStart(g, cap);
    g->throttled = throttled;
    return 1;
}

int depthGovernorCeiling(const DepthGovernor* g) {
    if (g->failedTarget <= 0) {
        return g->cap;
    }
    int half = g->failedTarget / 2 < DEPTH_RATE_MIN ? DEPTH_RATE_MIN : g->failedTarget / 2;
    return half < g->cap ? half : g->cap;
}

int64_t depthRecoverBackoff(int64_t waitNs) {
    return waitNs * 2 > DEPTH_RECOVER_MAX_NS ? DEPTH_RECOVER_MAX_NS : waitNs * 2;
}

// Halves the target unless a cut is still being held or it is at the floor
static int depthCut(DepthGovernor* g, int64_t nowNs) {
    if (g->cutNs != 0 && nowNs - g->cutNs < DEPTH_HOLD_NS) {
        return DEPTH_MOVE_HOLDING;
    }
    if (g->target <= DEPTH_RATE_MIN) {
        return DEPTH_MOVE_DISPLAY;
    }
    g->target = g->target / 2 < DEPTH_RATE_MIN ? DEPTH_RATE_MIN : g->target / 2;
    g->cutNs = nowNs;
    g->cappedSaid = 0;
    return DEPTH_MOVE_CUT;
}

static int depthHeld(DepthGovernor* g, int64_t nowNs, int live) {
    if (!live || g->throttled) {
        g->heldSinceNs = 0;
        return DEPTH_MOVE_NONE;
    }
    // The window that just held began a window ago
    if (g->heldSinceNs == 0) {
        g->heldSinceNs = nowNs - RATE_WINDOW_NS;
    }
    // A step up that has gone this long without a cut held, and the wait it
    // may have built up goes
    if (g->raisedNs != 0 && nowNs - g->raisedNs >= DEPTH_RAISE_HELD_NS) {
        g->raisedNs = 0;
        if (g->recoverNs != DEPTH_RECOVER_NS) {
            g->recoverNs = DEPTH_RECOVER_NS;
            return DEPTH_MOVE_RAISE_HELD;
        }
    }
    if (nowNs - g->heldSinceNs < g->recoverNs) {
        return DEPTH_MOVE_NONE;
    }
    int ceiling = depthGovernorCeiling(g);
    if (g->target < ceiling) {
        g->target = g->target * 2 > ceiling ? ceiling : g->target * 2;
        g->heldSinceNs = nowNs;
        g->raisedNs = nowNs;
        return DEPTH_MOVE_RAISED;
    }
    if (g->target < g->cap && !g->cappedSaid) {
        g->cappedSaid = 1;
        return DEPTH_MOVE_CAPPED;
    }
    return DEPTH_MOVE_NONE;
}

int depthGovernorWindow(DepthGovernor* g, int64_t nowNs, int verdict, int live) {
    switch (verdict) {
        case RATE_WINDOW_HELD:
            return depthHeld(g, nowNs, live);
        case RATE_WINDOW_OVER: {
            g->heldSinceNs = 0;
            if (!live) {
                return DEPTH_MOVE_DISPLAY;
            }
            int was = g->target;
            int move = depthCut(g, nowNs);
            if (move == DEPTH_MOVE_CUT) {
                // Remembered for the session, so the climb back stops short of it
                g->failedTarget = was;
                // and a step up that led here makes the next one wait longer
                if (g->raisedNs != 0) {
                    g->recoverNs = depthRecoverBackoff(g->recoverNs);
                    g->raisedNs = 0;
                }
            }
            return move;
        }
        case RATE_WINDOW_SLIPPING:
        case RATE_WINDOW_SETTLING:
            // Over, or the budget started again after a change: either way
            // the run of held windows is broken
            g->heldSinceNs = 0;
            return DEPTH_MOVE_NONE;
        default:
            return DEPTH_MOVE_NONE;
    }
}

int depthGovernorThrottle(DepthGovernor* g, int64_t nowNs, int worse, int throttled, int live) {
    g->throttled = throttled;
    if (!worse) {
        return DEPTH_MOVE_NONE;
    }
    g->heldSinceNs = 0;
    if (!live) {
        return DEPTH_MOVE_NONE;
    }
    int move = depthCut(g, nowNs);
    if (move == DEPTH_MOVE_CUT) {
        // Not the step up's fault, so it is neither judged nor backed off
        g->raisedNs = 0;
    }
    // A throttle is the runtime's business, so it never moves the display
    return move == DEPTH_MOVE_DISPLAY ? DEPTH_MOVE_NONE : move;
}
