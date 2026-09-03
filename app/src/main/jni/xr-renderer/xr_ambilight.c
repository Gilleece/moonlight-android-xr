// The ambilight: the frame boiled down to a tiny colour texture, the
// letterbox detection that keeps black bars out of it, and the glow quad
// drawn from it behind the screen.
#include "xr_renderer.h"
#include "xr_shaders.h"

// GL side of the ambilight: the colour texture the frame is sampled into and
// the program that spreads it over the glow quad. Set up whatever the depth
// settings are, since the glow needs no depth of any kind.
int initAmbilight(XrCtx* ctx) {
    if (!linkProgram(&ctx->ambiProgram, AMBI_FRAGMENT_SRC, "frame colour sample")) {
        return 0;
    }
    ctx->ambiTexMatrixUniform = glGetUniformLocation(ctx->ambiProgram, "u_texmatrix");
    ctx->ambiCropUniform = glGetUniformLocation(ctx->ambiProgram, "u_crop");
    glUseProgram(ctx->ambiProgram);
    glUniform1i(glGetUniformLocation(ctx->ambiProgram, "u_texture"), 0);

    glGenTextures(1, &ctx->ambiTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // The glow reads past both ends of this on purpose, and the clamp is what
    // carries the frame's edge colours out to the rim of the quad
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->ambiFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->ambiTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("ambilight framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // The letterbox detector's own target. Same size and format, never sampled
    // by anything: it exists to be drawn once in a while and read back.
    glGenTextures(1, &ctx->ambiDetectTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiDetectTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->ambiDetectFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiDetectFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->ambiDetectTexture, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("letterbox framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // Where its readback lands, to be looked at a frame later rather than
    // waited for
    glGenBuffers(1, &ctx->ambiDetectPbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->ambiDetectPbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, AMBI_SAMPLE_TEX * AMBI_SAMPLE_TEX * 4, NULL,
                 GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (!linkProgram(&ctx->glowProgram, GLOW_FRAGMENT_SRC, "glow")) {
        return 0;
    }
    ctx->glowIntensityUniform = glGetUniformLocation(ctx->glowProgram, "u_intensity");
    glUseProgram(ctx->glowProgram);
    glUniform1i(glGetUniformLocation(ctx->glowProgram, "u_texture"), 0);

    // Nothing attached yet: the target is whichever swapchain image the frame
    // acquires
    glGenFramebuffers(1, &ctx->glowFbo);

    LOGI("ambilight ready, sampling %dx%d into a %dx%d glow",
         AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, GLOW_TEX, GLOW_TEX);
    return 1;
}

// What the glow is doing this frame. The panel owns it, with the debug
// property over the top of it the way the separation override works.
void ambiEffective(XrCtx* ctx, int* on, float* level) {
    int enabled = ctx->ambilightOn;
    float value = ctx->ambiIntensity;
    if (ctx->ambiOverride >= 0) {
        enabled = ctx->ambiOverride > 0;
        value = ctx->ambiOverride / 100.0f;
    }
    *on = enabled;
    *level = value;
}

// The whole frame, which is what the sample pass used before there was a crop
// and what the detector always draws
static const float AMBI_CROP_FULL[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

// One edge of the hysteresis. The crop grows only once a larger bar has held
// for AMBI_BAR_APPLY ticks, and then only to the smallest of them, so a dark
// scene that reads as bar for a moment moves nothing. It shrinks after
// AMBI_BAR_RELEASE ticks, to the largest of those, which is the same idea the
// other way: the least the evidence supports.
static void ambiBarEdge(XrCtx* ctx, int edge, int measured) {
    if (measured > ctx->ambiBarApplied[edge]) {
        ctx->ambiBarShrinkTicks[edge] = 0;
        if (ctx->ambiBarGrowTicks[edge] == 0 || measured < ctx->ambiBarGrowMin[edge]) {
            ctx->ambiBarGrowMin[edge] = measured;
        }
        ctx->ambiBarGrowTicks[edge]++;
        if (ctx->ambiBarGrowTicks[edge] >= AMBI_BAR_APPLY) {
            ctx->ambiBarApplied[edge] = ctx->ambiBarGrowMin[edge];
            ctx->ambiBarGrowTicks[edge] = 0;
        }
        return;
    }
    if (measured < ctx->ambiBarApplied[edge]) {
        ctx->ambiBarGrowTicks[edge] = 0;
        if (ctx->ambiBarShrinkTicks[edge] == 0 || measured > ctx->ambiBarShrinkMax[edge]) {
            ctx->ambiBarShrinkMax[edge] = measured;
        }
        ctx->ambiBarShrinkTicks[edge]++;
        if (ctx->ambiBarShrinkTicks[edge] >= AMBI_BAR_RELEASE) {
            ctx->ambiBarApplied[edge] = ctx->ambiBarShrinkMax[edge];
            ctx->ambiBarShrinkTicks[edge] = 0;
        }
        return;
    }
    ctx->ambiBarGrowTicks[edge] = 0;
    ctx->ambiBarShrinkTicks[edge] = 0;
}

// Counts black bars in a readback of the sample texture and folds the result
// into the applied crop. Rows and columns are named by the readback's own
// order: row 0 is the v = 0 end of the quad, which is not necessarily the
// picture's top, since the SurfaceTexture transform may flip on the way in.
// Nothing here or downstream cares, because the crop is applied in the same
// space it was measured in.
static void detectAmbiBars(XrCtx* ctx, const unsigned char* rgba) {
    const int n = AMBI_SAMPLE_TEX;
    float rowSum[AMBI_SAMPLE_TEX];
    float colSum[AMBI_SAMPLE_TEX];
    for (int i = 0; i < n; i++) {
        rowSum[i] = 0.0f;
        colSum[i] = 0.0f;
    }
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const unsigned char* p = rgba + ((size_t)y * n + x) * 4;
            // The brightest channel rather than a weighted luma: a saturated
            // blue at the edge of the picture is not a bar, and the weights
            // would nearly call it one
            int m = p[0] > p[1] ? p[0] : p[1];
            if (p[2] > m) {
                m = p[2];
            }
            float l = m * (1.0f / 255.0f);
            rowSum[y] += l;
            colSum[x] += l;
        }
    }

    int rowBar[AMBI_SAMPLE_TEX];
    int colBar[AMBI_SAMPLE_TEX];
    int rowsBar = 0;
    int colsBar = 0;
    for (int i = 0; i < n; i++) {
        rowBar[i] = rowSum[i] * (1.0f / n) < AMBI_BAR_LUMA;
        colBar[i] = colSum[i] * (1.0f / n) < AMBI_BAR_LUMA;
        rowsBar += rowBar[i];
        colsBar += colBar[i];
    }

    // A frame that reads as bar end to end is a fade or a stall, not a
    // letterbox. Cropping it would pick a content region out of nothing and
    // then have to walk back out of it when the picture returns. Each axis
    // gets the rule on its own, which also covers the frame being black.
    int measured[AMBI_EDGES];
    memset(measured, 0, sizeof(measured));
    if (rowsBar < n) {
        while (measured[AMBI_EDGE_BOTTOM] < n && rowBar[measured[AMBI_EDGE_BOTTOM]]) {
            measured[AMBI_EDGE_BOTTOM]++;
        }
        while (measured[AMBI_EDGE_TOP] < n && rowBar[n - 1 - measured[AMBI_EDGE_TOP]]) {
            measured[AMBI_EDGE_TOP]++;
        }
    }
    if (colsBar < n) {
        while (measured[AMBI_EDGE_LEFT] < n && colBar[measured[AMBI_EDGE_LEFT]]) {
            measured[AMBI_EDGE_LEFT]++;
        }
        while (measured[AMBI_EDGE_RIGHT] < n && colBar[n - 1 - measured[AMBI_EDGE_RIGHT]]) {
            measured[AMBI_EDGE_RIGHT]++;
        }
    }

    int changed = 0;
    for (int e = 0; e < AMBI_EDGES; e++) {
        if (measured[e] > AMBI_BAR_MAX) {
            measured[e] = AMBI_BAR_MAX;
        }
        int before = ctx->ambiBarApplied[e];
        ambiBarEdge(ctx, e, measured[e]);
        if (ctx->ambiBarApplied[e] != before) {
            changed = 1;
        }
    }
    if (!changed) {
        return;
    }

    int left = ctx->ambiBarApplied[AMBI_EDGE_LEFT];
    int right = ctx->ambiBarApplied[AMBI_EDGE_RIGHT];
    int bottom = ctx->ambiBarApplied[AMBI_EDGE_BOTTOM];
    int top = ctx->ambiBarApplied[AMBI_EDGE_TOP];
    ctx->ambiCrop[0] = left / (float)n;
    ctx->ambiCrop[1] = bottom / (float)n;
    ctx->ambiCrop[2] = (n - left - right) / (float)n;
    ctx->ambiCrop[3] = (n - bottom - top) / (float)n;
    // Top and bottom here are the v = 1 and v = 0 ends of the readback, not
    // the picture's own, for the reason in the comment above
    LOGI("ambilight bars: top %d bottom %d left %d right %d", top, bottom, left, right);
}

// Draws the frame uncropped into the detector's target and asks for it back
// into the pixel buffer, where finishAmbiBarDetect reads it a frame later
// rather than draining the GPU queue for it here. Sets up all of its own
// state: it runs well after the sample pass, from a point where the video draw
// has left the program, viewport and texture units on something else entirely.
void runAmbiBarDetect(XrCtx* ctx, const float* texMatrix) {
    const int n = AMBI_SAMPLE_TEX;
    if (ctx->ambiDetectPbo == 0) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiDetectFbo);
    glViewport(0, 0, n, n);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->ambiProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->ambiTexMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform4fv(ctx->ambiCropUniform, 1, AMBI_CROP_FULL);
    // The current frame whole. Both the crop, which would hide the bars being
    // looked for, and the smoothing, which would drag old ones in for ten
    // frames after a cut, are off for this one draw.
    glDisable(GL_BLEND);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->ambiDetectPbo);
    glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ctx->ambiDetectPending = 1;
}

// Counts the bars in the readback asked for last time. By now the draw behind
// it has long finished, so the map costs a copy and nothing more.
void finishAmbiBarDetect(XrCtx* ctx) {
    const int n = AMBI_SAMPLE_TEX;
    if (!ctx->ambiDetectPending) {
        return;
    }
    ctx->ambiDetectPending = 0;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->ambiDetectPbo);
    const unsigned char* rgba = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, n * n * 4,
                                                 GL_MAP_READ_BIT);
    if (rgba != NULL) {
        detectAmbiBars(ctx, rgba);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

// Boils the frame down to the tiny colour texture. Kept self contained and
// free of anything glow shaped, since it is the frame's colours rather than
// the glow that anything else would want.
void runFrameColorSample(XrCtx* ctx, const float* texMatrix) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiFbo);
    glViewport(0, 0, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->ambiProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->ambiTexMatrixUniform, 1, GL_FALSE, texMatrix);
    // Whatever the detector last settled on, so the glow and the room's light
    // come from the picture rather than from the bars around it. With the
    // detection off the crop is not applied, but the state it found is kept,
    // so turning it back on picks up where it was.
    glUniform4fv(ctx->ambiCropUniform, 1,
                 ctx->ambiBarDetect ? ctx->ambiCrop : AMBI_CROP_FULL);

    // Mixed into what is already there rather than replacing it. A cut to a
    // different scene would otherwise strobe the whole glow in one frame,
    // where this takes about ten to get there. The first frame has nothing to
    // mix with, so it lands whole.
    if (ctx->ambiSeeded) {
        glEnable(GL_BLEND);
        glBlendColor(ctx->ambiSmooth, ctx->ambiSmooth, ctx->ambiSmooth, ctx->ambiSmooth);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    }

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
    ctx->ambiSeeded = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Spreads those colours over the glow quad, into an image the compositor then
// places behind the screen
void runGlowRender(XrCtx* ctx) {
    if (ctx->glowSwapchain == XR_NULL_HANDLE) {
        return;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->glowSwapchain, &acquire, &index),
                 "acquire glow image")) {
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->glowSwapchain, &wait);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->glowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->glowImages[index].image, 0);
    glViewport(0, 0, GLOW_TEX, GLOW_TEX);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    int on;
    float level;
    ambiEffective(ctx, &on, &level);

    glUseProgram(ctx->glowProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    glUniform1f(ctx->glowIntensityUniform, level);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->glowSwapchain, &release);
    ctx->glowRendered = 1;
}
