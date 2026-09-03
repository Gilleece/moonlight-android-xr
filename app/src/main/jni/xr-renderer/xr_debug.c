// Tuning knobs read over setprop and the frame capture they can request,
// so a headset session can be A/B tested and its warp inputs taken off
// the device for work on a desktop. Debug builds only: polling the property
// store costs syscalls on the frame loop, and a knob left set from one
// session would quietly override the panel in the next, so a release build
// compiles the polling out and only keeps the capture writers.
#include "xr_renderer.h"

#ifdef XR_DEBUG_KNOBS

// Integer valued tuning property, left alone if unset or unparseable
static void propScaled(const char* name, float* target, float scale, long maxRaw) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end != value && v >= 0 && v <= maxRaw) {
        *target = v * scale;
    }
}

static void propPercent(const char* name, float* target) {
    propScaled(name, target, 0.01f, 100);
}

void propFlag(const char* name, int* target) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    *target = value[0] != '0';
}

// Small integer mode property, left alone if unset or unparseable
static void propInt(const char* name, int* target, long maxRaw) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end != value && v >= 0 && v <= maxRaw) {
        *target = (int)v;
    }
}

// Fires once each time the property is set to a value it has not seen. The
// value becomes the filename tag, so setprop 1, 2, 3 gives three captures.
void pollCaptureRequest(XrCtx* ctx) {
    if (++ctx->capturePollCounter < CAPTURE_POLL_FRAMES) {
        return;
    }
    ctx->capturePollCounter = 0;

    propPercent(PROP_DEPTH_ALPHA, &ctx->depthAlpha);
    propPercent(PROP_RANGE_ALPHA, &ctx->rangeAlpha);
    propPercent(PROP_UPSAMPLE_SIGMA, &ctx->upsampleSigmaR);
    propPercent(PROP_DEPTH_SHARP, &ctx->depthSharp);
    propFlag(PROP_OVERLAY, &ctx->overlayVisible);
    propFlag(PROP_UPSAMPLE, &ctx->upsampleEnabled);
    propFlag(PROP_OCCLUSION, &ctx->occlusionEnabled);
    propPercent(PROP_CONVERGENCE, &ctx->convergence);
    // Same tenths of a percent of frame width the preference uses
    propScaled(PROP_SEPARATION, &ctx->separationOverride, 0.001f, 50);
    // Tenths of a metre, the same units the preferences use
    propScaled(PROP_DISTANCE, &ctx->distanceOverride, 0.1f, 80);
    propScaled(PROP_SCREEN, &ctx->screenOverride, 0.1f, 120);
    propPercent(PROP_DEPTH_GLOBAL, &ctx->depthGlobal);
    propScaled(PROP_DEPTH_LOCAL, &ctx->depthLocal, 0.01f, 400);
    // Tenths of a Hz, and half units of speed sensitivity
    propScaled(PROP_POINTER_CUTOFF, &ctx->pointerMinCutoff, 0.1f, 200);
    propScaled(PROP_POINTER_BETA, &ctx->pointerBeta, 0.5f, 100);
    propScaled(PROP_AIM_CUTOFF, &ctx->aimMinCutoff, 0.1f, 200);
    propScaled(PROP_AIM_BETA, &ctx->aimBeta, 0.5f, 100);
    // Millimetres
    propScaled(PROP_BEAM_WIDTH, &ctx->beamWidth, 0.001f, 100);
    // Tenths of a second
    propScaled(PROP_POINTER_WAKE, &ctx->pointerWake, 0.1f, 100);
    propScaled(PROP_POINTER_SLEEP, &ctx->pointerSleep, 0.1f, 600);
    // Metres. Zero is the infinite sphere the layer starts out as.
    propScaled(PROP_ENV_RADIUS, &ctx->envRadius, 1.0f, 200);
    // 0 off, 1 normal, 2 quality
    propInt(PROP_SHARPEN, &ctx->sharpenMode, 2);
    // 0 forces the glow off, 1 to 100 forces it on at that intensity, and
    // unset leaves the panel in charge. Same trap as the rest of these: one
    // left set from an earlier session quietly overrides the panel.
    propInt(PROP_AMBILIGHT, &ctx->ambiOverride, 100);
    propPercent(PROP_AMBI_SMOOTH, &ctx->ambiSmooth);
    // 0 samples the whole frame, black bars and all, and 1 or unset crops the
    // sample to the picture inside them. Same trap again: one left at 0 from an
    // earlier session quietly turns the detection off.
    propFlag(PROP_LETTERBOX, &ctx->ambiBarDetect);
    // 0 forces the room off, 1 forces the minimal room, 2 the psx cinema, and
    // unset leaves the picker in charge
    propInt(PROP_ROOM, &ctx->roomOverride, 2);
    // Percent, both of them, and 0 hands the value back to the room. The scale
    // reaches the cinema only, and moving it rebuilds the geometry, so it is
    // not a knob to sit on a slider.
    propScaled(PROP_ROOM_SCALE, &ctx->roomScaleOverride, 0.01f, 400);
    propScaled(PROP_ROOM_DIM, &ctx->roomDimOverride, 0.01f, 200);

    if (ctx->captureDir[0] == '\0') {
        return;
    }

    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(CAPTURE_PROP, value) <= 0 || value[0] == '\0') {
        return;
    }
    if (strcmp(value, ctx->lastCaptureTag) == 0) {
        return;
    }
    strncpy(ctx->lastCaptureTag, value, sizeof(ctx->lastCaptureTag) - 1);
    strncpy(ctx->captureTag, value, sizeof(ctx->captureTag) - 1);
    ctx->captureRequested = 1;
    LOGI("capture: request %s", ctx->captureTag);
}

#else

// A release build reads no properties at all. Every knob keeps the value the
// panel or the preferences gave it, and a capture can only be taken from a
// debug build.
void propFlag(const char* name, int* target) {
    (void)name;
    (void)target;
}

void pollCaptureRequest(XrCtx* ctx) {
    (void)ctx;
}

#endif

void writeCapture(XrCtx* ctx, const char* what, const void* data, size_t bytes) {
    if (data == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/cap_%s_%s.raw", ctx->captureDir, ctx->captureTag, what);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        LOGE("capture: cannot write %s", path);
        return;
    }
    size_t written = fwrite(data, 1, bytes, f);
    fclose(f);
    LOGI("capture: %s %zu bytes", path, written);
}

// Reads back the depth texture the warp actually sampled this frame, so the
// captured warp can be reproduced exactly rather than approximately. Depth is
// the alpha channel, the rgb alongside it is the guide.
void writeCaptureDepthTexture(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;
    unsigned char* rgba = malloc((size_t)n * n * 4);
    unsigned char* red = malloc((size_t)n * n);
    if (rgba == NULL || red == NULL) {
        free(rgba);
        free(red);
        return;
    }

    waitForDepthSlot(ctx);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->depthTextures[ctx->depthReadIndex], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        for (int i = 0; i < n * n; i++) {
            red[i] = rgba[i * 4 + 3];
        }
        writeCapture(ctx, "depthtex", red, (size_t)n * n);
        writeCapture(ctx, "guidetex", rgba, (size_t)n * n * 4);
    }
    else {
        LOGW("capture: depth texture not readable");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    free(rgba);
    free(red);
}
