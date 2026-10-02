// The display refresh rate the session asks the runtime for. Matched to the
// stream at the start, stepped down while the 3D warp runs if the frame loop's
// own frame time does not fit the period, and asked for once more if the
// runtime moves the display off it. Which rate is xr_rate.c's business; this
// is the OpenXR side of it and the bookkeeping on the frame loop. And the
// CPU and GPU levels the session asks for, the other half of what the
// runtime is asked to do for the stream.
#include "xr_renderer.h"

// A rate as the log shows it: whole when it is whole, else to two places
static const char* hzText(float hz, char* buf, size_t size) {
    if (fabsf(hz - roundf(hz)) < 0.01f) {
        snprintf(buf, size, "%.0f", hz);
    }
    else {
        snprintf(buf, size, "%.2f", hz);
    }
    return buf;
}

static int sameRate(float a, float b) {
    return fabsf(a - b) <= RATE_TOLERANCE;
}

static int warpRunning(XrCtx* ctx) {
    return ctx->stereoMode != DEPTH_MODE_OFF && ctx->stereoLive;
}

void probeDisplayExtensions(XrCtx* ctx) {
    // Only declared under XR_EXTENSION_PROTOTYPES, so they come through the
    // loader like the hand tracking ones do
    if (ctx->refreshRateSupported) {
        xrGetInstanceProcAddr(ctx->instance, "xrEnumerateDisplayRefreshRatesFB",
                              (PFN_xrVoidFunction*)&ctx->pfnEnumerateDisplayRefreshRates);
        xrGetInstanceProcAddr(ctx->instance, "xrGetDisplayRefreshRateFB",
                              (PFN_xrVoidFunction*)&ctx->pfnGetDisplayRefreshRate);
        xrGetInstanceProcAddr(ctx->instance, "xrRequestDisplayRefreshRateFB",
                              (PFN_xrVoidFunction*)&ctx->pfnRequestDisplayRefreshRate);
        if (ctx->pfnEnumerateDisplayRefreshRates == NULL
                || ctx->pfnGetDisplayRefreshRate == NULL
                || ctx->pfnRequestDisplayRefreshRate == NULL) {
            LOGW("display refresh rate offered but its entry points are missing");
            ctx->refreshRateSupported = 0;
        }
    }
    LOGEV("display refresh rate control %s", ctx->refreshRateSupported
          ? "available (XR_FB_display_refresh_rate)"
          : "not offered by this runtime, the display stays on the runtime's rate");

    if (ctx->perfSettingsSupported) {
        xrGetInstanceProcAddr(ctx->instance, "xrPerfSettingsSetPerformanceLevelEXT",
                              (PFN_xrVoidFunction*)&ctx->pfnPerfSettingsSetPerformanceLevel);
        if (ctx->pfnPerfSettingsSetPerformanceLevel == NULL) {
            LOGW("performance settings offered but the entry point is missing");
            ctx->perfSettingsSupported = 0;
        }
    }
    LOGEV("performance levels %s", ctx->perfSettingsSupported
          ? "available (XR_EXT_performance_settings)"
          : "not offered by this runtime, the clocks are the runtime's to choose");
}

// What the display offers and what it is on. Session scoped, so it cannot be
// asked any earlier than this.
void startDisplay(XrCtx* ctx) {
    if (!ctx->refreshRateSupported) {
        return;
    }

    uint32_t count = 0;
    if (!checkXr(ctx->pfnEnumerateDisplayRefreshRates(ctx->session, 0, &count, NULL),
                 "count display refresh rates") || count == 0) {
        ctx->refreshRateSupported = 0;
        return;
    }
    float* rates = calloc(count, sizeof(float));
    if (rates == NULL) {
        ctx->refreshRateSupported = 0;
        return;
    }
    char line[160];
    int used = 0;
    line[0] = '\0';
    if (checkXr(ctx->pfnEnumerateDisplayRefreshRates(ctx->session, count, &count, rates),
                "enumerate display refresh rates")) {
        for (uint32_t i = 0; i < count && ctx->displayRateCount < RATE_MAX; i++) {
            if (rates[i] <= 0.0f) {
                continue;
            }
            ctx->displayRates[ctx->displayRateCount++] = rates[i];
            char hz[16];
            if (used < (int)sizeof(line) - 1) {
                used += snprintf(line + used, sizeof(line) - used, "%s%s",
                                 used > 0 ? " " : "", hzText(rates[i], hz, sizeof(hz)));
            }
        }
    }
    free(rates);
    if (ctx->displayRateCount == 0) {
        ctx->refreshRateSupported = 0;
        LOGEV("display refresh rate control offers no rates, left to the runtime");
        return;
    }

    float current = 0.0f;
    if (checkXr(ctx->pfnGetDisplayRefreshRate(ctx->session, &current), "get display refresh rate")) {
        ctx->displayRate = current;
    }
    char now[16];
    LOGEV("display rates offered: %s Hz, display on %s Hz, stream %d fps", line,
          hzText(current, now, sizeof(now)), ctx->streamFps);
}

// Sends one request. The runtime takes a second or two to move, and says so
// with an event, so this only notes when it went.
static int askRate(XrCtx* ctx, float hz) {
    XrResult res = ctx->pfnRequestDisplayRefreshRate(ctx->session, hz);
    if (XR_FAILED(res)) {
        char text[16];
        LOGW("display rate %s Hz refused: %d", hzText(hz, text, sizeof(text)), res);
        return 0;
    }
    ctx->rateAsked = hz;
    ctx->rateAskedNs = nowNs();
    ctx->rateConfirmed = sameRate(ctx->displayRate, hz);
    rateBudgetStart(&ctx->rateBudget, ctx->rateAskedNs, RATE_SETTLE_WINDOWS);
    return 1;
}

// Works out the rate for how things stand now, and asks for it unless the
// display is already there. why is what changed, for the log.
static void applyRate(XrCtx* ctx, const char* why) {
    ctx->rateWarpOn = warpRunning(ctx);
    if (!ctx->refreshRateSupported || ctx->session == XR_NULL_HANDLE) {
        return;
    }

    char hz[16], fromHz[16], extra[64];
    float want;
    extra[0] = '\0';
    if (ctx->refreshKnob > 0) {
        want = rateOffered((float)ctx->refreshKnob, ctx->displayRates, ctx->displayRateCount);
        if (want <= 0.0f) {
            // Not a rate this display has, so the stream's rule picks one for it
            want = rateForStream((float)ctx->refreshKnob, ctx->displayRates,
                                 ctx->displayRateCount);
            snprintf(extra, sizeof(extra), ", %d Hz is not offered", ctx->refreshKnob);
        }
    }
    else {
        want = rateChoose((float)ctx->streamFps, ctx->displayRates, ctx->displayRateCount,
                          ctx->rateWarpOn, ctx->warpRateHeld, 0.0f);
        if (ctx->rateWarpOn && ctx->warpRateHeld > 0.0f && sameRate(want, ctx->warpRateHeld)) {
            snprintf(extra, sizeof(extra), ", held there while the 3D is on");
        }
    }
    if (want <= 0.0f) {
        if (ctx->rateAsked <= 0.0f) {
            LOGEV("display rate left to the runtime, no stream frame rate (%d)", ctx->streamFps);
        }
        return;
    }
    // Already there, or on its way there
    if (sameRate(want, ctx->rateAsked)
            && (sameRate(want, ctx->displayRate) || !ctx->rateConfirmed)) {
        return;
    }
    if (askRate(ctx, want)) {
        if (ctx->refreshKnob > 0) {
            LOGEV("display rate: asking for %s Hz, forced by %s%s (%s), display on %s Hz",
                  hzText(want, hz, sizeof(hz)), PROP_REFRESH, extra, why,
                  hzText(ctx->displayRate, fromHz, sizeof(fromHz)));
        }
        else {
            LOGEV("display rate: asking for %s Hz for a %d fps stream%s (%s), display on %s Hz",
                  hzText(want, hz, sizeof(hz)), ctx->streamFps, extra, why,
                  hzText(ctx->displayRate, fromHz, sizeof(fromHz)));
        }
    }
}

void displaySessionBegun(XrCtx* ctx) {
    rateBudgetStart(&ctx->rateBudget, nowNs(), RATE_SETTLE_WINDOWS);
    applyRate(ctx, "session start");
}

void displayFocused(XrCtx* ctx) {
    rateBudgetStart(&ctx->rateBudget, nowNs(), RATE_SETTLE_WINDOWS);
    if (!ctx->refreshRateSupported || ctx->rateAsked <= 0.0f) {
        return;
    }
    // A rate asked for while the session was not focused can be dropped on
    // the way back in. One still on its way is left to land.
    float current = 0.0f;
    if (XR_SUCCEEDED(ctx->pfnGetDisplayRefreshRate(ctx->session, &current)) && current > 0.0f) {
        ctx->displayRate = current;
    }
    if (!sameRate(ctx->displayRate, ctx->rateAsked) && ctx->rateConfirmed) {
        char hz[16], now[16];
        LOGEV("display on %s Hz on focus, asking for %s Hz again",
              hzText(ctx->displayRate, now, sizeof(now)), hzText(ctx->rateAsked, hz, sizeof(hz)));
        ctx->rateMovedNs = 0;
        askRate(ctx, ctx->rateAsked);
    }
}

void displayRateChanged(XrCtx* ctx, float from, float to) {
    char fromHz[16], toHz[16], asked[16];
    LOGEV("display rate changed %s to %s Hz", hzText(from, fromHz, sizeof(fromHz)),
          hzText(to, toHz, sizeof(toHz)));
    ctx->displayRate = to;
    rateBudgetStart(&ctx->rateBudget, nowNs(), RATE_SETTLE_WINDOWS);
    // Said on the toast as well, whoever moved it, once the session's own
    // first rate has landed: the session start is no news
    if (ctx->rateSettled) {
        noticePush(&ctx->notices, TOAST_RATE, (int)roundf(to));
    }
    else {
        LOGI("display rate change not toasted, the session's first rate is still landing");
    }
    if (ctx->rateAsked <= 0.0f) {
        return;
    }
    if (sameRate(to, ctx->rateAsked)) {
        ctx->rateConfirmed = 1;
        ctx->rateMovedNs = 0;
        return;
    }
    // Out of focus the system has the display, and the rate is asked for
    // again when focus comes back
    if (ctx->sessionState != XR_SESSION_STATE_FOCUSED) {
        return;
    }
    // Not where it was asked to be: a throttle, the system, or another app.
    // Asked for once more after a pause rather than fought frame by frame.
    if (ctx->rateMovedNs == 0) {
        ctx->rateMovedNs = nowNs();
        ctx->rateConfirmed = 1;
        LOGEV("display rate: the runtime moved the display to %s Hz off the %s asked for, "
              "%s in %d s", toHz, hzText(ctx->rateAsked, asked, sizeof(asked)),
              ctx->rateReasks < RATE_REASK_MAX ? "asking again" : "looking again",
              RATE_REASK_MS / 1000);
    }
}

void displayFrameBegun(XrCtx* ctx, const XrFrameState* state) {
    ctx->frameBeganNs = nowNs();
    if (state->predictedDisplayPeriod <= 0) {
        return;
    }
    ctx->displayPeriodNs = (long)state->predictedDisplayPeriod;
    // A frame the loop was late for shows as the predicted time jumping by
    // more than one refresh
    if (ctx->lastDisplayTime != 0 && ctx->sessionState == XR_SESSION_STATE_FOCUSED) {
        XrTime step = state->predictedDisplayTime - ctx->lastDisplayTime;
        long missed = (long)((step + state->predictedDisplayPeriod / 2)
                             / state->predictedDisplayPeriod) - 1;
        rateBudgetMissed(&ctx->rateBudget, missed);
    }
    ctx->lastDisplayTime = state->predictedDisplayTime;
}

// Every RATE_LOG_WINDOWS judged windows, a line on how the rate is holding
static void logBudget(XrCtx* ctx) {
    const RateBudget* b = &ctx->rateBudget;
    ctx->rateLogWindows++;
    ctx->rateLogFrameMs += b->frameMs;
    if (b->frameMs > ctx->rateLogWorstMs) {
        ctx->rateLogWorstMs = b->frameMs;
    }
    ctx->rateLogMissed += b->lastMissed;
    if (ctx->rateLogWindows < RATE_LOG_WINDOWS) {
        return;
    }
    char hz[16];
    LOGI("display %s Hz over %ld s: frame %.2f ms avg, %.2f worst window, period %.2f ms, "
         "%ld missed refreshes, last window GPU %.2f ms (%ld warps) CPU %.2f ms pace %.2f ms, "
         "3D %s", hzText(ctx->displayRate, hz, sizeof(hz)),
         (long)RATE_LOG_WINDOWS * (RATE_WINDOW_NS / 1000000000L),
         ctx->rateLogFrameMs / ctx->rateLogWindows, ctx->rateLogWorstMs,
         1000.0f / ctx->displayRate, ctx->rateLogMissed, b->gpuMs, b->lastGpuFrames, b->cpuMs,
         b->paceMs, ctx->rateWarpOn ? "on" : "off");
    ctx->rateLogWindows = 0;
    ctx->rateLogFrameMs = 0.0f;
    ctx->rateLogWorstMs = 0.0f;
    ctx->rateLogMissed = 0;
}

void displayFrameEnded(XrCtx* ctx) {
    long now = nowNs();
    if (ctx->frameBeganNs > 0) {
        rateBudgetCpu(&ctx->rateBudget, now - ctx->frameBeganNs);
        ctx->frameBeganNs = 0;
    }
    if (!ctx->rateSettled
            && rateSettled(0, ctx->sessionState == XR_SESSION_STATE_FOCUSED,
                           ctx->refreshRateSupported ? ctx->rateAsked : 0.0f,
                           ctx->rateConfirmed)) {
        char settledHz[16];
        float hz = ctx->displayRate > 0.0f ? ctx->displayRate
                : ctx->displayPeriodNs > 0 ? 1e9f / (float)ctx->displayPeriodNs : 0.0f;
        ctx->rateSettled = 1;
        LOGI("display rate settled on %s Hz, a change from here on goes on the toast",
             hzText(hz, settledHz, sizeof(settledHz)));
    }
    if (!ctx->refreshRateSupported || ctx->rateAsked <= 0.0f) {
        return;
    }
    char hz[16], asked[16];

    // The 3D switched, which moves the warp's ceiling
    if (warpRunning(ctx) != ctx->rateWarpOn) {
        applyRate(ctx, warpRunning(ctx) ? "3D on" : "3D off");
        rateBudgetStart(&ctx->rateBudget, now, RATE_SETTLE_WINDOWS);
    }

    // Out of focus the system has the display, and the budget would measure a
    // rate nobody chose. Focus coming back picks it all up again.
    if (ctx->sessionState != XR_SESSION_STATE_FOCUSED) {
        rateBudgetStart(&ctx->rateBudget, now, RATE_SETTLE_WINDOWS);
        return;
    }

    // A request the runtime never acted on, which is what a pinned rate looks
    // like, gets the same one more try as a move
    if (!ctx->rateConfirmed && (now - ctx->rateAskedNs) / 1000000L > RATE_CONFIRM_MS) {
        ctx->rateConfirmed = 1;
        float current = 0.0f;
        if (XR_SUCCEEDED(ctx->pfnGetDisplayRefreshRate(ctx->session, &current))
                && current > 0.0f) {
            ctx->displayRate = current;
        }
        if (!sameRate(ctx->displayRate, ctx->rateAsked) && ctx->rateMovedNs == 0) {
            ctx->rateMovedNs = now;
            LOGEV("display still on %s Hz %d s after asking for %s Hz",
                  hzText(ctx->displayRate, hz, sizeof(hz)), RATE_CONFIRM_MS / 1000,
                  hzText(ctx->rateAsked, asked, sizeof(asked)));
        }
    }

    if (ctx->rateMovedNs != 0 && (now - ctx->rateMovedNs) / 1000000L > RATE_REASK_MS) {
        ctx->rateMovedNs = 0;
        if (!sameRate(ctx->displayRate, ctx->rateAsked)) {
            if (ctx->rateReasks < RATE_REASK_MAX) {
                ctx->rateReasks++;
                LOGEV("display rate: asking for %s Hz again, display on %s Hz (%d of %d)",
                      hzText(ctx->rateAsked, asked, sizeof(asked)),
                      hzText(ctx->displayRate, hz, sizeof(hz)), ctx->rateReasks, RATE_REASK_MAX);
                askRate(ctx, ctx->rateAsked);
            }
            else {
                LOGEV("display rate: leaving the display on %s Hz after asking for %s Hz %d times",
                      hzText(ctx->displayRate, hz, sizeof(hz)),
                      hzText(ctx->rateAsked, asked, sizeof(asked)), RATE_REASK_MAX + 1);
            }
        }
    }

    // And only judged with the display on the rate asked for
    if (!sameRate(ctx->displayRate, ctx->rateAsked)) {
        rateBudgetStart(&ctx->rateBudget, now, RATE_SETTLE_WINDOWS);
        return;
    }
    int verdict = rateBudgetTick(&ctx->rateBudget, now, ctx->displayRate);
    if (verdict == RATE_WINDOW_FILLING || verdict == RATE_WINDOW_SETTLING) {
        return;
    }
    logBudget(ctx);
    if (verdict != RATE_WINDOW_OVER || !ctx->rateWarpOn || ctx->refreshKnob > 0) {
        return;
    }

    const RateBudget* b = &ctx->rateBudget;
    float next = rateChoose((float)ctx->streamFps, ctx->displayRates, ctx->displayRateCount, 1,
                            ctx->rateAsked, b->frameMs);
    long overS = (long)RATE_OVER_WINDOWS * (RATE_WINDOW_NS / 1000000000L);
    if (next > 0.0f && next < ctx->rateAsked - RATE_TOLERANCE) {
        char to[16];
        LOGEV("display rate: %s Hz frame time %.2f ms (GPU %.2f, CPU %.2f, pace %.2f with %ld "
              "of %ld refreshes missed) over its %.2f ms period for %ld s with the 3D on, "
              "stepping down to %s Hz",
              hzText(ctx->rateAsked, asked, sizeof(asked)), b->frameMs, b->gpuMs, b->cpuMs,
              b->paceMs, b->lastMissed, b->lastFrames + b->lastMissed,
              1000.0f / ctx->rateAsked, overS, hzText(next, to, sizeof(to)));
        ctx->warpRateHeld = next;
        askRate(ctx, next);
    }
    else if (!sameRate(ctx->rateFloorSaid, ctx->rateAsked)) {
        // Said once a rate: the floor, or a stream already under it
        ctx->rateFloorSaid = ctx->rateAsked;
        LOGEV("display rate: %s Hz frame time %.2f ms over its %.2f ms period for %ld s with "
              "the 3D on, as low as the budget goes",
              hzText(ctx->rateAsked, asked, sizeof(asked)), b->frameMs,
              1000.0f / ctx->rateAsked, overS);
    }
}

void setRefreshKnob(XrCtx* ctx, int hz) {
    ctx->refreshKnob = hz;
    LOGEV("refresh knob %d Hz%s", hz, hz == 0 ? ", automatic" : "");
    if (ctx->sessionRunning) {
        applyRate(ctx, "knob changed");
    }
}

static const char* perfLevelName(XrPerfSettingsLevelEXT level) {
    switch (level) {
        case XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT: return "power savings";
        case XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT: return "sustained low";
        case XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT: return "sustained high";
        case XR_PERF_SETTINGS_LEVEL_BOOST_EXT: return "boost";
        default: return "unknown";
    }
}

static const char* perfResultName(XrResult res) {
    return XR_SUCCEEDED(res) ? "ok" : "refused";
}

void setPerfLevel(XrCtx* ctx, int level) {
    int was = ctx->perfLevel;
    ctx->perfLevel = level;
    if (!ctx->perfSettingsSupported || ctx->session == XR_NULL_HANDLE) {
        return;
    }
    if (level == PERF_LEVEL_NONE) {
        if (was != PERF_LEVEL_NONE) {
            LOGEV("performance levels: nothing more asked for, the last ask stands until "
                  "the session ends");
        }
        else {
            LOGEV("performance levels: none asked for, the runtime chooses");
        }
        return;
    }
    XrPerfSettingsLevelEXT want = level >= PERF_LEVEL_BOOST
            ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    XrResult cpu = ctx->pfnPerfSettingsSetPerformanceLevel(ctx->session,
                                                           XR_PERF_SETTINGS_DOMAIN_CPU_EXT, want);
    XrResult gpu = ctx->pfnPerfSettingsSetPerformanceLevel(ctx->session,
                                                           XR_PERF_SETTINGS_DOMAIN_GPU_EXT, want);
    LOGEV("performance levels asked for: cpu %s (%s), gpu %s (%s)", perfLevelName(want),
          perfResultName(cpu), perfLevelName(want), perfResultName(gpu));
    if (XR_FAILED(cpu) || XR_FAILED(gpu)) {
        LOGW("performance level results: cpu %d, gpu %d", cpu, gpu);
    }
}

// Once the session exists, which the levels belong to
void startPerfLevels(XrCtx* ctx) {
    setPerfLevel(ctx, ctx->perfLevel);
}

static const char* perfDomainName(XrPerfSettingsDomainEXT domain) {
    return domain == XR_PERF_SETTINGS_DOMAIN_CPU_EXT ? "cpu"
            : domain == XR_PERF_SETTINGS_DOMAIN_GPU_EXT ? "gpu" : "unknown domain";
}

static const char* perfSubDomainName(XrPerfSettingsSubDomainEXT sub) {
    switch (sub) {
        case XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT: return "compositing";
        case XR_PERF_SETTINGS_SUB_DOMAIN_RENDERING_EXT: return "rendering";
        case XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT: return "thermal";
        default: return "unknown";
    }
}

static const char* perfNoticeName(XrPerfSettingsNotificationLevelEXT level) {
    switch (level) {
        case XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT: return "normal";
        case XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT: return "warning";
        case XR_PERF_SETTINGS_NOTIF_LEVEL_IMPAIRED_EXT: return "impaired";
        default: return "unknown";
    }
}

// The runtime throttling or letting go again, which is the only warning a
// thermal drop ever gives
void perfNotice(XrCtx* ctx, const XrEventDataPerfSettingsEXT* notice) {
    (void)ctx;
    LOGEV("performance notice: %s %s, %s to %s", perfDomainName(notice->domain),
          perfSubDomainName(notice->subDomain), perfNoticeName(notice->fromLevel),
          perfNoticeName(notice->toLevel));
}

// The rate the display is on for the stats: the runtime's word where it has
// the extension, else the period it paces the frame loop at
JNIEXPORT jfloat JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetDisplayRate(JNIEnv* env, jobject thiz,
                                                                 jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return 0.0f;
    }
    if (ctx->refreshRateSupported && ctx->displayRate > 0.0f) {
        return ctx->displayRate;
    }
    return ctx->displayPeriodNs > 0 ? (float)(1e9 / (double)ctx->displayPeriodNs) : 0.0f;
}

// Every rate the display offers, empty on a runtime that does not say
JNIEXPORT jfloatArray JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetOfferedRates(JNIEnv* env, jobject thiz,
                                                                  jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    jsize count = ctx != NULL && ctx->refreshRateSupported ? (jsize)ctx->displayRateCount : 0;
    jfloatArray out = (*env)->NewFloatArray(env, count);
    if (out != NULL && count > 0) {
        (*env)->SetFloatArrayRegion(env, out, 0, count, ctx->displayRates);
    }
    return out;
}

// What the session last asked for, 0 when it has not asked
JNIEXPORT jfloat JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetAskedRate(JNIEnv* env, jobject thiz,
                                                               jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return ctx != NULL ? ctx->rateAsked : 0.0f;
}
