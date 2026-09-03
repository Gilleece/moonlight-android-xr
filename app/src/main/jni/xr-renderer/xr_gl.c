// GL setup and the passes that turn a decoded frame into the stereo pair
// the compositor shows: depth upsample, offset search, warp, and the GPU
// timer that says what they cost.
#include "xr_renderer.h"
#include "xr_shaders.h"

PFNGENQUERIESEXT pfnGenQueries;
PFNBEGINQUERYEXT pfnBeginQuery;
PFNENDQUERYEXT pfnEndQuery;
PFNGETQUERYOBJECTUIVEXT pfnGetQueryObjectuiv;
PFNGETQUERYOBJECTUI64VEXT pfnGetQueryObjectui64v;
PFNDELETEQUERIESEXT pfnDeleteQueries;

// Same fullscreen strip as the 2d GL path, x y u v
const float VERTEX_DATA[16] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Builds the hardcoded depth map for the stereo test path. Depth convention:
// 0 far, 1 near, 0.5 sits exactly on the screen plane (zero disparity)
static int fillSyntheticDepth(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;
    // RGBA throughout: depth in alpha, guide colour in rgb. The synthetic
    // patterns have no guide, so it stays neutral and the upsample falls back
    // to a plain blur on them.
    unsigned char* buf = malloc((size_t)n * n * 4);
    if (buf == NULL) {
        LOGE("no memory for the depth texture");
        return 0;
    }

    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            float fx = x / (float)(n - 1);
            float fy = y / (float)(n - 1);
            float d;
            switch (ctx->stereoMode) {
                case DEPTH_MODE_RAMP:
                    d = fx;
                    break;
                case DEPTH_MODE_BLOB: {
                    float dx = fx - 0.5f;
                    float dy = fy - 0.5f;
                    float sigma = 0.15f;
                    d = 0.35f + 0.5f * expf(-(dx * dx + dy * dy) / (2.0f * sigma * sigma));
                    break;
                }
                case DEPTH_MODE_SHIFTTEST:
                    // Constant near depth so the whole bar shifts uniformly
                    d = 0.85f;
                    break;
                case DEPTH_MODE_FLAT:
                default:
                    d = 0.5f;
                    break;
            }
            if (d < 0.0f) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            unsigned char* px = buf + ((size_t)y * n + x) * 4;
            px[0] = px[1] = px[2] = 128;
            px[3] = (unsigned char)(d * 255.0f + 0.5f);
        }
    }

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    free(buf);
    return 1;
}

int linkProgram(GLuint* out, const char* fragmentSrc, const char* what) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOGE("%s program link failed: %s", what, log);
        return 0;
    }
    *out = program;
    return 1;
}

// Quarter resolution is enough: at 1920x1080 the measured edge width was the
// same 5 px, so the extra four times the pixels bought nothing.
static int initUpsample(XrCtx* ctx) {
    ctx->upsampleWidth = ctx->videoWidth / 4;
    ctx->upsampleHeight = ctx->videoHeight / 4;

    if (!linkProgram(&ctx->upsampleProgram, UPSAMPLE_FRAGMENT_SRC, "upsample")) {
        return 0;
    }
    ctx->upsampleTexMatrixUniform = glGetUniformLocation(ctx->upsampleProgram, "u_texmatrix");
    ctx->upsampleSigmaUniform = glGetUniformLocation(ctx->upsampleProgram, "u_sigmaR");
    ctx->upsampleSharpUniform = glGetUniformLocation(ctx->upsampleProgram, "u_sharp");
    glUseProgram(ctx->upsampleProgram);
    glUniform1i(glGetUniformLocation(ctx->upsampleProgram, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(ctx->upsampleProgram, "u_depth"), 1);

    glGenTextures(1, &ctx->upsampleTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->upsampleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx->upsampleWidth, ctx->upsampleHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->upsampleFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->upsampleTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("upsample framebuffer incomplete: 0x%x", status);
        return 0;
    }

    if (!linkProgram(&ctx->offsetProgram, OFFSET_FRAGMENT_SRC, "offset")) {
        return 0;
    }
    ctx->offsetDispUniform = glGetUniformLocation(ctx->offsetProgram, "u_dispTexels");
    ctx->offsetConvUniform = glGetUniformLocation(ctx->offsetProgram, "u_convergence");
    glUseProgram(ctx->offsetProgram);
    glUniform1i(glGetUniformLocation(ctx->offsetProgram, "u_depth"), 1);

    glGenTextures(1, &ctx->offsetTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->offsetTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx->upsampleWidth, ctx->upsampleHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->offsetFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->offsetFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->offsetTexture, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("offset framebuffer incomplete: 0x%x", status);
        return 0;
    }

    LOGI("depth upsample and offset search ready at %dx%d",
         ctx->upsampleWidth, ctx->upsampleHeight);
    return 1;
}

int initGl(XrCtx* ctx) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }

    ctx->program = glCreateProgram();
    glAttachShader(ctx->program, vs);
    glAttachShader(ctx->program, fs);
    glBindAttribLocation(ctx->program, 0, "a_position");
    glBindAttribLocation(ctx->program, 1, "a_texcoord");
    glLinkProgram(ctx->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(ctx->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(ctx->program, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        return 0;
    }
    ctx->texMatrixUniform = glGetUniformLocation(ctx->program, "u_texmatrix");
    ctx->disparityUniform = glGetUniformLocation(ctx->program, "u_disparity");
    ctx->tintUniform = glGetUniformLocation(ctx->program, "u_tint");
    ctx->barTestUniform = glGetUniformLocation(ctx->program, "u_barTest");
    ctx->occlusionUniform = glGetUniformLocation(ctx->program, "u_occlusion");
    ctx->eyeIndexUniform = glGetUniformLocation(ctx->program, "u_eyeIndex");
    ctx->convergenceUniform = glGetUniformLocation(ctx->program, "u_convergence");
    ctx->dispTexelsUniform = glGetUniformLocation(ctx->program, "u_dispTexels");
    ctx->lowResWidthUniform = glGetUniformLocation(ctx->program, "u_lowResWidth");
    ctx->frameWidthUniform = glGetUniformLocation(ctx->program, "u_frameWidth");

    // Sampler units are fixed: color on 0, depth on 1
    glUseProgram(ctx->program);
    glUniform1i(glGetUniformLocation(ctx->program, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(ctx->program, "u_depth"), 1);
    glUniform1i(glGetUniformLocation(ctx->program, "u_offsets"), 2);
    glUniform1f(glGetUniformLocation(ctx->program, "u_showDepth"),
                ctx->depthDebug ? 1.0f : 0.0f);

    glGenTextures(2, ctx->depthTextures);
    if (!fillSyntheticDepth(ctx)) {
        return 0;
    }

    glGenTextures(1, &ctx->oesTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->fbo);

    const char* glExts = (const char*)glGetString(GL_EXTENSIONS);
    ctx->srgbWriteControl = glExts != NULL && strstr(glExts, "GL_EXT_sRGB_write_control") != NULL;

    if (glExts != NULL && strstr(glExts, "GL_EXT_disjoint_timer_query") != NULL) {
        pfnGenQueries = (PFNGENQUERIESEXT)eglGetProcAddress("glGenQueriesEXT");
        pfnBeginQuery = (PFNBEGINQUERYEXT)eglGetProcAddress("glBeginQueryEXT");
        pfnEndQuery = (PFNENDQUERYEXT)eglGetProcAddress("glEndQueryEXT");
        pfnGetQueryObjectuiv = (PFNGETQUERYOBJECTUIVEXT)eglGetProcAddress("glGetQueryObjectuivEXT");
        pfnGetQueryObjectui64v =
                (PFNGETQUERYOBJECTUI64VEXT)eglGetProcAddress("glGetQueryObjectui64vEXT");
        pfnDeleteQueries = (PFNDELETEQUERIESEXT)eglGetProcAddress("glDeleteQueriesEXT");
        if (pfnGenQueries != NULL && pfnBeginQuery != NULL && pfnEndQuery != NULL &&
                pfnGetQueryObjectuiv != NULL && pfnGetQueryObjectui64v != NULL &&
                pfnDeleteQueries != NULL) {
            pfnGenQueries(2, ctx->timerQueries);
            pfnGenQueries(2, ctx->roomTimerQueries);
            ctx->timerSupported = 1;
        }
    }
    if (!ctx->timerSupported) {
        LOGW("GL_EXT_disjoint_timer_query missing, GPU times unavailable");
    }

    // The video frames are already gamma encoded. With an sRGB swapchain the
    // GPU would encode again on write, so turn that off. Without the
    // extension colors will look washed out and we would need a shader fix.
    if (ctx->swapchainFormat == GL_SRGB8_ALPHA8 && !ctx->srgbWriteControl) {
        LOGW("GL_EXT_sRGB_write_control not available, expect wrong gamma");
    }

    if (!initAmbilight(ctx)) {
        return 0;
    }

    if (ctx->stereoMode == DEPTH_MODE_MODEL) {
        if (!initDepthModel(ctx) || !initUpsample(ctx)) {
            return 0;
        }
    }

    return 1;
}

// Runs every video frame rather than only when new depth lands, which also
// re-snaps a depth map that is a few frames old onto the colour edges of the
// frame it is actually warping.
static void runUpsample(XrCtx* ctx, const float* texMatrix) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
    glViewport(0, 0, ctx->upsampleWidth, ctx->upsampleHeight);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->upsampleProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[ctx->depthReadIndex]);
    glUniformMatrix4fv(ctx->upsampleTexMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform1f(ctx->upsampleSigmaUniform, ctx->upsampleSigmaR);
    glUniform1f(ctx->upsampleSharpUniform, ctx->depthSharp);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Both eyes in one pass, since they search the same depth reads
static void runOffsetSearch(XrCtx* ctx, float separation) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->offsetFbo);
    glViewport(0, 0, ctx->upsampleWidth, ctx->upsampleHeight);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->offsetProgram);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->upsampleTexture);
    glUniform1f(ctx->offsetDispUniform, separation * ctx->upsampleWidth);
    glUniform1f(ctx->offsetConvUniform, ctx->convergence);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation) {
    int upsampling = ctx->stereoMode == DEPTH_MODE_MODEL && ctx->upsampleEnabled;
    int occluding = upsampling && ctx->occlusionEnabled && separation > 0.0f;

    // Capture frames do readbacks and file writes inside what would be the
    // query window, which both ruins the number and, on this driver, leaves a
    // query that never becomes available. Skip timing them.
    int timing = ctx->timerSupported && !ctx->captureRequested;

    // Before the query opens, since creating the room's swapchain and buffers
    // inside the window wedges every sample after it. Only the CPU and buffer
    // work: the room's own draw is at the end of this function.
    int roomOn = roomEffective(ctx) > 0;
    if (roomOn) {
        prepareRoom(ctx);
    }

    if (timing && !ctx->timerPending[ctx->timerSlot]) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->timerQueries[ctx->timerSlot]);
    }

    if (upsampling) {
        runUpsample(ctx, texMatrix);
    }
    if (occluding) {
        runOffsetSearch(ctx, separation);
    }

    // Inside the query window with the rest of them, so what the glow costs
    // shows up in the same GPU number
    int glowOn;
    float glowLevel;
    ambiEffective(ctx, &glowOn, &glowLevel);
    // The room is lit from the same colours the glow is made of, so the sample
    // is taken for either of them. It happens here rather than with the room's
    // draw, which now follows the video, so the room is lit from this frame's
    // colour rather than the last one's.
    int sampled = glowOn || roomOn;
    if (sampled) {
        runFrameColorSample(ctx, texMatrix);
    }
    if (glowOn) {
        runGlowRender(ctx);
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->swapchain, &acquireInfo, &imageIndex), "acquire image")) {
        // The query opened above must not be left standing on this early out,
        // or the next frame's begin would nest inside it
        if (timing && !ctx->timerPending[ctx->timerSlot]) {
            pfnEndQuery(GL_TIME_ELAPSED_EXT);
            ctx->timerPending[ctx->timerSlot] = 1;
            ctx->timerPendingFrames[ctx->timerSlot] = 0;
            ctx->timerSlot = 1 - ctx->timerSlot;
        }
        return;
    }
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->swapchain, &waitInfo);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->swapchainImages[imageIndex].image, 0);

    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glActiveTexture(GL_TEXTURE1);
    // Either the raw 256x256 map or the edge aware upsample of it. Both carry
    // depth in alpha, so the warp shader does not care which it got.
    glBindTexture(GL_TEXTURE_2D, upsampling ? ctx->upsampleTexture
                                            : ctx->depthTextures[ctx->depthReadIndex]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx->offsetTexture);
    glUniformMatrix4fv(ctx->texMatrixUniform, 1, GL_FALSE, texMatrix);
    glUniform1f(ctx->occlusionUniform, occluding ? 1.0f : 0.0f);
    glUniform1f(ctx->convergenceUniform, ctx->convergence);
    glUniform1f(ctx->dispTexelsUniform, separation * ctx->upsampleWidth);
    glUniform1f(ctx->lowResWidthUniform, (float)ctx->upsampleWidth);
    glUniform1f(ctx->frameWidthUniform, (float)ctx->videoWidth);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);

    // Mono is a single full width draw with zero disparity. Stereo draws the
    // left eye into the left half and the right eye into the right half,
    // with opposite disparity signs
    int eyes = ctx->stereoMode != DEPTH_MODE_OFF ? 2 : 1;

    // The unwarped frame, drawn first so the real eye passes overwrite it and
    // the submitted frame is unaffected. Readback and file writes stall the
    // frame loop for a while, which is fine for a one off debug capture.
    unsigned char* captureBuf = NULL;
    size_t captureBytes = (size_t)ctx->videoWidth * ctx->videoHeight * 4;
    if (ctx->captureRequested) {
        captureBuf = malloc(captureBytes);
        if (captureBuf != NULL) {
            glViewport(0, 0, ctx->videoWidth, ctx->videoHeight);
            glUniform1f(ctx->disparityUniform, 0.0f);
            glUniform1f(ctx->barTestUniform, 0.0f);
            glUniform3f(ctx->tintUniform, 1.0f, 1.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glReadPixels(0, 0, ctx->videoWidth, ctx->videoHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                         captureBuf);
            writeCapture(ctx, "source", captureBuf, captureBytes);
        }
    }

    for (int eye = 0; eye < eyes; eye++) {
        glViewport(eye * ctx->videoWidth, 0, ctx->videoWidth, ctx->videoHeight);
        float disparity = 0.0f;
        if (eyes == 2 && ctx->stereoMode != DEPTH_MODE_EYETEST) {
            disparity = (eye == 0) ? separation : -separation;
        }
        glUniform1f(ctx->disparityUniform, disparity);
        glUniform1f(ctx->eyeIndexUniform, (float)eye);
        glUniform1f(ctx->barTestUniform, ctx->stereoMode == DEPTH_MODE_SHIFTTEST ? 1.0f : 0.0f);

        if (ctx->stereoMode == DEPTH_MODE_EYETEST) {
            // Half 0 red, half 1 blue
            if (eye == 0) {
                glUniform3f(ctx->tintUniform, 1.0f, 0.2f, 0.2f);
            }
            else {
                glUniform3f(ctx->tintUniform, 0.2f, 0.2f, 1.0f);
            }
        }
        else {
            glUniform3f(ctx->tintUniform, 1.0f, 1.0f, 1.0f);
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Measure where the bar actually landed in each half. Positive shift
    // means content moved right in that eye
    if (ctx->stereoMode == DEPTH_MODE_SHIFTTEST && ctx->barTestFramesLogged < 3) {
        int rowWidth = ctx->videoWidth * 2;
        unsigned char* row = malloc((size_t)rowWidth * 4);
        if (row != NULL) {
            glReadPixels(0, ctx->videoHeight / 2, rowWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
            for (int half = 0; half < 2; half++) {
                long sum = 0, count = 0;
                for (int x = 0; x < ctx->videoWidth; x++) {
                    if (row[(size_t)((half * ctx->videoWidth) + x) * 4] > 128) {
                        sum += x;
                        count++;
                    }
                }
                if (count > 0) {
                    double center = (double)sum / (double)count / (double)ctx->videoWidth;
                    LOGI("bar test: half %d (%s eye) bar center %.4f, shift %+.4f",
                         half, half == 0 ? "left" : "right", center, center - 0.5);
                }
                else {
                    LOGI("bar test: half %d no bar found", half);
                }
            }
            free(row);
        }
        ctx->barTestFramesLogged++;
    }

    if (ctx->captureRequested) {
        if (captureBuf != NULL) {
            for (int eye = 0; eye < eyes; eye++) {
                glReadPixels(eye * ctx->videoWidth, 0, ctx->videoWidth, ctx->videoHeight,
                             GL_RGBA, GL_UNSIGNED_BYTE, captureBuf);
                writeCapture(ctx, eye == 0 ? "left" : "right", captureBuf, captureBytes);
            }
            free(captureBuf);
        }
        writeCaptureDepthTexture(ctx);
        if (upsampling) {
            size_t count = (size_t)ctx->upsampleWidth * ctx->upsampleHeight;
            unsigned char* rgba = malloc(count * 4);
            unsigned char* alpha = malloc(count);
            if (rgba != NULL && alpha != NULL) {
                glBindFramebuffer(GL_FRAMEBUFFER, ctx->upsampleFbo);
                glReadPixels(0, 0, ctx->upsampleWidth, ctx->upsampleHeight, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba);
                for (size_t i = 0; i < count; i++) {
                    alpha[i] = rgba[i * 4 + 3];
                }
                writeCapture(ctx, "upsampled", alpha, count);
            }
            free(rgba);
            free(alpha);
        }
        // Best effort, the depth thread may be part way through refilling
        // these. The depth texture above is the exact one this frame sampled.
        writeCapture(ctx, "modelinput", ctx->modelInput,
                     (size_t)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * 3 * sizeof(float));
        writeCapture(ctx, "depthraw", ctx->modelOutput,
                     (size_t)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * sizeof(float));
        ctx->captureRequested = 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->swapchain, &releaseInfo);

    // Close this frame's query and collect whichever earlier one has landed.
    // Never blocks: an unfinished query is simply left for a later frame.
    if (timing) {
        if (!ctx->timerPending[ctx->timerSlot]) {
            pfnEndQuery(GL_TIME_ELAPSED_EXT);
            ctx->timerPending[ctx->timerSlot] = 1;
            ctx->timerPendingFrames[ctx->timerSlot] = 0;
            ctx->timerSlot = 1 - ctx->timerSlot;
        }
        int other = ctx->timerSlot;
        if (ctx->timerPending[other]) {
            GLuint ready = 0;
            pfnGetQueryObjectuiv(ctx->timerQueries[other], GL_QUERY_RESULT_AVAILABLE_EXT, &ready);
            if (ready) {
                GLuint64 elapsed = 0;
                pfnGetQueryObjectui64v(ctx->timerQueries[other], GL_QUERY_RESULT_EXT, &elapsed);
                ctx->timerPending[other] = 0;
                ctx->timerPendingFrames[other] = 0;
                // Only a plausible sample is kept: zero and anything past 50 ms
                // is the driver rather than a frame, and the wrapped negatives
                // land far past the cap. The disjoint flag is deliberately not
                // consulted: this driver raises it on every GPU clock change,
                // which the room render provokes constantly, and gating on it
                // starved the stats to nothing while the values stayed sane.
                if (elapsed > 0 && elapsed < 50000000ull) {
                    ctx->gpuTotalNs += (long)elapsed;
                    ctx->gpuSamples++;
                    ctx->overlayGpuTotalNs += (long)elapsed;
                    ctx->overlayGpuSamples++;
                    if ((long)elapsed > ctx->gpuMaxNs) {
                        ctx->gpuMaxNs = (long)elapsed;
                    }
                }
                else {
                    // Counted and kept so the stats line can say what the
                    // driver was actually handing back on a starved window
                    ctx->gpuDropped++;
                    ctx->gpuLastDroppedNs = elapsed;
                }
            }
            else if (++ctx->timerPendingFrames[other] > 90) {
                // Abandon it. Waiting forever costs every later measurement,
                // and one missed sample costs nothing.
                ctx->timerPending[other] = 0;
                ctx->timerPendingFrames[other] = 0;
                LOGW("XR warp: gave up on a GPU timer query that never landed");
            }
        }
    }

    // Letterbox detection, once every so many frames the colour sample ran, so
    // it stops with the sample rather than running on its own. Down here rather
    // than beside the sample on purpose: this reads the result back, and a
    // readback inside the query window can leave a query that never becomes
    // available on this driver, which is the same trap the capture path avoids
    // by not timing itself at all. The room's query has not opened yet either,
    // so the read sits between the two.
    if (sampled && ctx->ambiBarDetect) {
        // Last time's readback first, then this frame's request if it is due
        finishAmbiBarDetect(ctx);
        ctx->ambiBarCounter++;
        if (ctx->ambiBarCounter >= AMBI_BAR_PERIOD) {
            ctx->ambiBarCounter = 0;
            runAmbiBarDetect(ctx, texMatrix);
        }
    }

    // Last of all, after the video has been queued and the main query closed.
    // GL runs one queue in order, and under head motion the compositor preempts
    // this full resolution pass often enough to stretch it past 20 ms: anything
    // queued behind it waits, which is what the video was doing while this ran
    // first. Behind the warp a preempted room only holds up its own layer, and
    // the compositor covers that by reprojecting the image it already has.
    // The two timer pairs stay disjoint: the main one is ended just above, and
    // renderRoom opens its own once it has its swapchain image.
    if (roomOn) {
        renderRoom(ctx);
    }

    // The room's pair is opened just above, but collected down here, on every
    // frame rather than only the ones it drew: a query from the last frame it
    // rendered still has to be picked up after it goes away.
    if (timing) {
        int roomOther = ctx->roomTimerSlot;
        if (ctx->roomTimerPending[roomOther]) {
            GLuint ready = 0;
            pfnGetQueryObjectuiv(ctx->roomTimerQueries[roomOther], GL_QUERY_RESULT_AVAILABLE_EXT,
                                 &ready);
            if (ready) {
                GLuint64 elapsed = 0;
                pfnGetQueryObjectui64v(ctx->roomTimerQueries[roomOther], GL_QUERY_RESULT_EXT,
                                       &elapsed);
                ctx->roomTimerPending[roomOther] = 0;
                ctx->roomTimerPendingFrames[roomOther] = 0;
                // Same plausibility filter as the warp's, for the same reason
                if (elapsed > 0 && elapsed < 50000000ull) {
                    ctx->roomGpuTotalNs += (long)elapsed;
                    ctx->roomGpuSamples++;
                }
                else {
                    ctx->roomGpuDropped++;
                }
            }
            else if (++ctx->roomTimerPendingFrames[roomOther] > 90) {
                ctx->roomTimerPending[roomOther] = 0;
                ctx->roomTimerPendingFrames[roomOther] = 0;
                LOGW("room: gave up on a GPU timer query that never landed");
            }
        }
    }

    ctx->everRendered = 1;
}

// Average GPU time of the warp since this was last called, which is what the
// overlay wants. Returns 0 when the timer is unavailable.
JNIEXPORT jfloat JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetWarpGpuMs(JNIEnv* env, jobject thiz,
                                                                jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->overlayGpuSamples == 0) {
        return 0.0f;
    }
    float ms = (float)(ctx->overlayGpuTotalNs / (double)ctx->overlayGpuSamples / 1e6);
    ctx->overlayGpuTotalNs = 0;
    ctx->overlayGpuSamples = 0;
    return ms;
}
