// The display refresh rate choice and the frame budget behind it. No OpenXR
// calls and no context, so the host tests reach all of it.
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

float rateForStream(float fps, const float* rates, int count) {
    if (rates == NULL || fps <= 0.0f) {
        return 0.0f;
    }
    float exact = rateOffered(fps, rates, count);
    if (exact > 0.0f) {
        return exact;
    }
    // Nothing matches, so the nearest rate that still shows every frame, and
    // failing that the fastest there is, which drops the fewest
    float above = 0.0f, highest = 0.0f;
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
    float want = rateForStream(fps, rates, count);
    if (want <= 0.0f || !warpOn) {
        return want;
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

static void clearWindow(RateBudget* b, long nowNs) {
    b->startNs = nowNs;
    b->cpuTotalNs = 0;
    b->cpuFrames = 0;
    b->gpuTotalNs = 0;
    b->gpuFrames = 0;
    b->roomTotalNs = 0;
    b->roomFrames = 0;
    b->missed = 0;
}

void rateBudgetStart(RateBudget* b, long nowNs, int settle) {
    clearWindow(b, nowNs);
    b->settle = settle;
    b->overWindows = 0;
}

void rateBudgetCpu(RateBudget* b, long frameNs) {
    if (frameNs > 0) {
        b->cpuTotalNs += frameNs;
        b->cpuFrames++;
    }
}

void rateBudgetGpu(RateBudget* b, long gpuNs) {
    if (gpuNs > 0) {
        b->gpuTotalNs += gpuNs;
        b->gpuFrames++;
    }
}

void rateBudgetRoom(RateBudget* b, long gpuNs) {
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

int rateBudgetTick(RateBudget* b, long nowNs, float hz) {
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
