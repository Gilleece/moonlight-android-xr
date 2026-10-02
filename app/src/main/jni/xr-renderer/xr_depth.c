// The depth model's staging: the frame is drawn small and read back for
// the model on the frame loop, the stage thread turns the readback into the
// model input, the depth thread runs the model in Java, and the stage thread
// normalises the result and uploads it as the map the warp samples. Each
// capture travels in one of DEPTH_PAIRS pairs of staging, so the stages of
// one map overlap the model's run on another.
#include "xr_renderer.h"
#include "xr_depthmap.h"
#include "xr_shaders.h"

// GL side of the depth model path: the downscale target the frame is
// rendered into, and the staging buffers it is read back through
int initDepthModel(XrCtx* ctx) {
    // nativeInit sets the size before any GL init runs, so a zero here means
    // that order broke
    if (ctx->depthTexW <= 0 || ctx->depthTexH <= 0) {
        LOGE("depth size not set before the depth model init");
        return 0;
    }
    const int w = ctx->depthTexW;
    const int h = ctx->depthTexH;
    const size_t count = (size_t)w * h;

    if (!linkProgram(&ctx->downscaleProgram, DOWNSCALE_FRAGMENT_SRC, "downscale")) {
        return 0;
    }
    ctx->downscaleTexMatrixUniform = glGetUniformLocation(ctx->downscaleProgram, "u_texmatrix");
    glUseProgram(ctx->downscaleProgram);
    glUniform1i(glGetUniformLocation(ctx->downscaleProgram, "u_texture"), 0);
    // One destination texel on each axis, so the box filter spans one texel
    // of the map at any size. Fixed for the session, so set once here.
    glUniform2f(glGetUniformLocation(ctx->downscaleProgram, "u_texel"),
                1.0f / (float)w, 1.0f / (float)h);

    glGenTextures(1, &ctx->downscaleTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->downscaleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &ctx->downscaleFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->downscaleFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->downscaleTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("downscale framebuffer incomplete: 0x%x", status);
        return 0;
    }

    // The depth thread gets its own context in the same share group for the
    // model, and the stage thread another, so it can map the readbacks and
    // upload into the back depth texture while the frame loop draws and the
    // model runs
    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ctx->depthContext = eglCreateContext(ctx->eglDisplay, ctx->eglConfig, ctx->eglContext,
                                         contextAttribs);
    if (ctx->depthContext == EGL_NO_CONTEXT) {
        LOGE("depth thread context creation failed: %d", eglGetError());
        return 0;
    }
    ctx->depthPbuffer = eglCreatePbufferSurface(ctx->eglDisplay, ctx->eglConfig, pbufferAttribs);
    if (ctx->depthPbuffer == EGL_NO_SURFACE) {
        LOGE("depth thread pbuffer creation failed: %d", eglGetError());
        return 0;
    }
    ctx->depthStageContext = eglCreateContext(ctx->eglDisplay, ctx->eglConfig, ctx->eglContext,
                                              contextAttribs);
    if (ctx->depthStageContext == EGL_NO_CONTEXT) {
        LOGE("depth stage context creation failed: %d", eglGetError());
        return 0;
    }
    ctx->depthStagePbuffer = eglCreatePbufferSurface(ctx->eglDisplay, ctx->eglConfig,
                                                     pbufferAttribs);
    if (ctx->depthStagePbuffer == EGL_NO_SURFACE) {
        LOGE("depth stage pbuffer creation failed: %d", eglGetError());
        return 0;
    }

    // Slot 0 is what fillSyntheticDepth and depthReadIndex both start on, so
    // the first real upload has to land somewhere else
    ctx->depthWriteIndex = 1;
    atomic_init(&ctx->depthStagedIndex, 0);
    atomic_init(&ctx->depthLastPair, 0);

    // Storage only: each capture reads back into its pair's and the stage
    // thread maps it later, so nothing is ever uploaded into them
    glGenBuffers(DEPTH_PAIRS, ctx->depthPbos);
    for (int i = 0; i < DEPTH_PAIRS; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depthPbos[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(count * 4), NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    int pairsOk = 1;
    for (int i = 0; i < DEPTH_PAIRS; i++) {
        ctx->modelInput[i] = malloc(count * 3 * sizeof(float));
        ctx->modelOutput[i] = malloc(count * sizeof(float));
        pairsOk = pairsOk && ctx->modelInput[i] != NULL && ctx->modelOutput[i] != NULL;
    }
    ctx->depthUploadBuf = malloc(count * 4);
    ctx->depthNorm = malloc(count * sizeof(float));
    ctx->depthTau = malloc(count * sizeof(float));
    ctx->depthLow = malloc(count * sizeof(float));
    ctx->depthScratch = malloc(count * sizeof(float));
    ctx->depthColSums = malloc((size_t)w * sizeof(float));
    if (!pairsOk || ctx->depthUploadBuf == NULL || ctx->depthNorm == NULL ||
            ctx->depthTau == NULL || ctx->depthLow == NULL ||
            ctx->depthScratch == NULL || ctx->depthColSums == NULL) {
        LOGE("depth staging buffer allocation failed");
        return 0;
    }

    LOGI("depth model staging ready at %dx%d", w, h);
    return 1;
}

static int pairOk(jint pair) {
    return pair >= 0 && pair < DEPTH_PAIRS;
}

JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetModelInput(JNIEnv* env, jobject thiz,
                                                                jlong handle, jint pair) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || !pairOk(pair) || ctx->modelInput[pair] == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelInput[pair],
                                       (jlong)ctx->depthTexW * ctx->depthTexH * 3 * sizeof(float));
}

JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetModelOutput(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jint pair) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || !pairOk(pair) || ctx->modelOutput[pair] == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelOutput[pair],
                                       (jlong)ctx->depthTexW * ctx->depthTexH * sizeof(float));
}

// Draws the current frame into the downscale target and asks for it back into
// the pair's pixel buffer. Nothing waits here: a readback straight into memory
// drains the whole GPU queue, which at 90 Hz is most of a frame gone. The
// stage thread waits on the fence left behind here and maps the buffer into
// the pair's model input, in nativeFinishDepthCapture, on a thread with no
// frame deadline to miss. Java only hands over a pair nothing else is using.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeCaptureDepthInput(JNIEnv* env, jobject thiz,
                                                                    jlong handle,
                                                                    jfloatArray texMatrixArr,
                                                                    jint pair) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->depthPbos[0] == 0 || !pairOk(pair)) {
        return 0;
    }
    const int w = ctx->depthTexW;
    const int h = ctx->depthTexH;
    int64_t startNs = nowNs();

    float texMatrix[16];
    (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->downscaleFbo);
    glViewport(0, 0, w, h);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->downscaleProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->downscaleTexMatrixUniform, 1, GL_FALSE, texMatrix);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depthPbos[pair]);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (ctx->captureFences[pair] != NULL) {
        // Only reachable if Java handed out a pair still in use, and a fence
        // nothing ever waited on would otherwise leak here
        glDeleteSync(ctx->captureFences[pair]);
    }

    // Fence first, flush second, and the order is the whole point: a flush
    // only pushes out what is already in this context's queue, so flushing
    // before the fence exists leaves the fence itself sitting unflushed. The
    // stage thread waits on it from another context and cannot flush this
    // one on our behalf, so it would block until this thread happened to
    // flush for some unrelated reason.
    ctx->captureFences[pair] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    return nowNs() - startNs;
}

// The scene cut detector, on the capture just copied into the pair's model
// input. Captures are finished in the order they were taken, so the detector
// steps through them in order, and what it finds stays with the pair until
// the map made from it goes up.
static void depthCutCheck(XrCtx* ctx, int pair, int w, int h) {
    int level = ctx->depthCutLevel;
    if (level == 0) {
        depthCutClear(&ctx->depthCut);
        return;
    }

    DepthThumb thumb;
    depthThumbMake(&thumb, ctx->modelInput[pair], w, h);
    // What the detector is about to judge, taken before the step moves it on
    int trace = level >= 2;
    float td = -1.0f, th = -1.0f, td2 = -1.0f, th2 = -1.0f, tcorr = 0.0f;
    float tout = 0.0f, tback = 0.0f;
    if (trace && ctx->depthCut.haveLast) {
        const DepthCut* c = &ctx->depthCut;
        td = depthThumbDiff(&thumb, &c->last);
        th = depthThumbHistDiff(&thumb, &c->last);
        tcorr = depthThumbCorr(&thumb, &c->last);
        tout = td / fmaxf(c->lastDiff, 1e-4f);
        if (c->haveOlder) {
            td2 = depthThumbDiff(&thumb, &c->older);
            th2 = depthThumbHistDiff(&thumb, &c->older);
            tback = td2 / fmaxf(td, 1e-4f);
        }
    }
    float diff, hist;
    int cut = depthCutStep(&ctx->depthCut, &thumb, &diff, &hist);
    ctx->depthCutChecks++;
    if (trace) {
        // One fixed line per capture, for a parser: the count of captures
        // checked, d and h against the last capture, d2 and h2 against the one
        // two back (-1 where there is none), the grid's correlation with the
        // last one's, out as d over the last step's d, back as d2 over d, and
        // what this capture fired. A confirmation's gates were measured on
        // the jump, so read them on the line before it.
        LOGI("depth cutv %ld d %.4f h %.4f d2 %.4f h2 %.4f corr %.3f out %.2f back %.2f "
             "jump %d cut %d", ctx->depthCutChecks, td, th, td2, th2, tcorr, tout, tback,
             (cut & DEPTH_CUT_JUMP) != 0, (cut & DEPTH_CUT_CONFIRMED) != 0);
    }
    depthResetsSet(&ctx->depthResets, pair, depthCutResets(cut));
    if (!(cut & DEPTH_CUT_CONFIRMED)) {
        return;
    }

    int64_t now = nowNs();
    if (ctx->depthCutLogNs != 0 && now - ctx->depthCutLogNs < DEPTH_CUT_LOG_NS) {
        ctx->depthCutUnlogged++;
        return;
    }
    if (ctx->depthCutUnlogged > 0) {
        LOGEV("depth cut: diff %.3f hist %.3f corr %.2f, %d more not logged", diff, hist,
              ctx->depthCut.corr, ctx->depthCutUnlogged);
    }
    else {
        LOGEV("depth cut: diff %.3f hist %.3f corr %.2f", diff, hist, ctx->depthCut.corr);
    }
    ctx->depthCutLogNs = now;
    ctx->depthCutUnlogged = 0;
}

// Waits for a pair's readback to land, then copies it out of the pixel buffer
// into the pair's model input. Rows are flipped on the way: GL hands back the
// bottom row first and the model wants the image the right way up, since
// monocular depth leans heavily on which way is down. Runs on the stage thread
// as soon as the capture is queued, while the model runs the other pair, and
// the scene cut detector looks at it there. Returns the time it took, or -1
// when the buffer could not be mapped and there is nothing to run the model
// on.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeFinishDepthCapture(JNIEnv* env, jobject thiz,
                                                                     jlong handle, jint pair) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->depthPbos[0] == 0 || !pairOk(pair)) {
        return -1;
    }
    const int w = ctx->depthTexW;
    const int h = ctx->depthTexH;
    int64_t startNs = nowNs();
    const int slot = pair;
    // Nothing found yet, so a capture that cannot be mapped carries nothing
    depthResetsSet(&ctx->depthResets, pair, 0);

    GLsync fence = ctx->captureFences[slot];
    if (fence != NULL) {
        // Checked rather than discarded: on a timeout the map below still
        // hands back a buffer, so the frame would be normalised from whatever
        // the transfer had managed and the depth map would go subtly wrong
        // with nothing in the log to say why. Half a second is long past
        // anything a readback this small can take, so this firing means the
        // fence never landed rather than that the GPU was busy.
        if (glClientWaitSync(fence, 0, CAPTURE_FENCE_TIMEOUT_NS) == GL_TIMEOUT_EXPIRED) {
            LOGW("depth capture: readback fence timed out, frame may be torn");
        }
        glDeleteSync(fence);
        ctx->captureFences[slot] = NULL;
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depthPbos[slot]);
    const unsigned char* pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                   (GLsizeiptr)w * h * 4, GL_MAP_READ_BIT);
    if (pixels == NULL) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        LOGW("depth readback could not be mapped: 0x%x", glGetError());
        return -1;
    }

    for (int y = 0; y < h; y++) {
        const unsigned char* src = pixels + (size_t)(h - 1 - y) * w * 4;
        float* dst = ctx->modelInput[pair] + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            dst[x * 3 + 0] = src[x * 4 + 0] * (1.0f / 255.0f);
            dst[x * 3 + 1] = src[x * 4 + 1] * (1.0f / 255.0f);
            dst[x * 3 + 2] = src[x * 4 + 2] * (1.0f / 255.0f);
        }
    }

    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    depthCutCheck(ctx, pair, w, h);
    return nowNs() - startNs;
}

static jboolean bindContext(XrCtx* ctx, EGLContext context, EGLSurface surface,
                            const char* who) {
    if (!eglMakeCurrent(ctx->eglDisplay, surface, surface, context)) {
        LOGE("%s eglMakeCurrent failed: %d", who, eglGetError());
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

static void unbindContext(XrCtx* ctx, EGLContext* context, EGLSurface* surface) {
    eglMakeCurrent(ctx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (*surface != EGL_NO_SURFACE) {
        eglDestroySurface(ctx->eglDisplay, *surface);
        *surface = EGL_NO_SURFACE;
    }
    if (*context != EGL_NO_CONTEXT) {
        eglDestroyContext(ctx->eglDisplay, *context);
        *context = EGL_NO_CONTEXT;
    }
    eglReleaseThread();
}

// Binds the depth thread's context. Called once from that thread before it
// touches GL or creates the delegate.
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeBindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return JNI_FALSE;
    }
    return bindContext(ctx, ctx->depthContext, ctx->depthPbuffer, "depth thread");
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUnbindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    unbindContext(ctx, &ctx->depthContext, &ctx->depthPbuffer);
}

// The model will make no map this session, so the splash stops waiting for
// one. Any thread.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeDepthGaveUp(JNIEnv* env, jobject thiz,
                                                              jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx != NULL) {
        atomic_store_explicit(&ctx->depthGaveUp, 1, memory_order_relaxed);
    }
}

// The same for the stage thread, called once from it before it touches GL
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeBindDepthStageContext(JNIEnv* env, jobject thiz,
                                                                        jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return JNI_FALSE;
    }
    return bindContext(ctx, ctx->depthStageContext, ctx->depthStagePbuffer, "depth stage");
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUnbindDepthStageContext(JNIEnv* env,
                                                                          jobject thiz,
                                                                          jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    unbindContext(ctx, &ctx->depthStageContext, &ctx->depthStagePbuffer);
}

// The model output averaged per texel over real time, alpha = 1 - exp(-dt /
// tau) with dt the time since the last map, so the settling time is the same
// whatever rate the model manages. It runs on the raw output, ahead of the
// range, since the range is smoothed over a history of its own and has to be
// found on the map that is drawn. Hands back the map to normalise, which is
// the model output itself when the time constant is 0.
static const float* depthTauMap(XrCtx* ctx, const float* output, int count, int64_t now) {
    int tauMs = ctx->depthTauMs;
    if (tauMs <= 0) {
        ctx->depthTauValid = 0;
        return output;
    }
    int seed = !ctx->depthTauValid;
    float alpha = seed ? 1.0f
                       : depthTauAlpha((float)(now - ctx->depthTauNs) / 1e9f,
                                       (float)tauMs / 1000.0f);
    depthTauBlend(ctx->depthTau, output, count, alpha, seed);
    ctx->depthTauValid = 1;
    ctx->depthTauNs = now;
    return ctx->depthTau;
}

// Normalizes a pair's model output to 0..1 and uploads it as the depth map the
// warp samples. Both models emit relative inverse depth on an arbitrary
// scale, so the range has to be found per frame. Rows flip back here.
//
// Two separate temporal filters, each a one pole over real time. The model
// output is averaged per texel ahead of everything else so raw model flicker
// does not reach the eyes, and the range is smoothed on its own so the
// mapping does not jump when the scene changes. A scene cut found on this
// map's capture starts them again instead. The guide colour rides along in
// RGB so the upsampling pass gets the exact frame the depth came from.
//
// Runs on the stage thread while the model runs the other pair, and in
// capture order, so a map from a capture taken before a cut can never land
// after the map the cut starts the averages on. It writes the next slot in
// a fixed rotation, never the one the frame loop is reading, then publishes
// it behind a fence. This used to finish instead, which stalled the thread
// until the GPU was idle and still did not promise the frame loop's context,
// a different one in the same share group, would see the result. A fence is
// something that context can wait on itself, at the point it samples the
// texture.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadDepth(JNIEnv* env, jobject thiz,
                                                              jlong handle, jint pair) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || !pairOk(pair) || ctx->modelOutput[pair] == NULL) {
        return 0;
    }
    const int w = ctx->depthTexW;
    const int h = ctx->depthTexH;
    int64_t startNs = nowNs();

    // Whatever the cut check found on this map's capture, or on an earlier
    // one whose model run made no map
    int resets = depthResetsTake(&ctx->depthResets, pair);
    if (resets & DEPTH_RESET_TEXEL) {
        ctx->depthTauValid = 0;
    }
    if (resets & DEPTH_RESET_RANGE) {
        ctx->depthRange.valid = 0;
    }

    const float* raw = depthTauMap(ctx, ctx->modelOutput[pair], w * h, startNs);
    float lo, hi;
    robustRange(raw, w * h, &lo, &hi);
    depthRangeStep(&ctx->depthRange, lo, hi, (float)(startNs - ctx->rangeNs) / 1e9f,
                   (float)ctx->rangeTauMs / 1000.0f);
    ctx->rangeNs = startNs;
    const float rangeLo = ctx->depthRange.lo;
    const float scale = depthRangeScale(&ctx->depthRange);

    for (int y = 0; y < h; y++) {
        const float* src = raw + (size_t)(h - 1 - y) * w;
        float* norm = ctx->depthNorm + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            // A NaN, from the model or from a texel of the average nothing
            // finite has reached yet, would otherwise sail through the clamps
            // below
            float v = (depthRead(src[x], lo) - rangeLo) * scale;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            norm[x] = v;
        }
    }

    float kg = ctx->depthGlobal;
    float kl = ctx->depthLocal;
    float conv = ctx->convergence;

    // The low pass is only needed to split the map into overall shape and
    // local detail, so skip it when the remap is doing nothing. It costs
    // about 10 ms on this thread, which is latency the depth map cannot
    // afford for an effect measured to be invisible.
    int remapping = kg < 0.995f || kg > 1.005f || kl < 0.995f || kl > 1.005f;
    if (remapping) {
        // Scaled with the map on each axis, so the split covers the same part
        // of the picture at any size
        lowPass(ctx->depthNorm, ctx->depthLow, ctx->depthScratch, ctx->depthColSums, w, h,
                DEPTH_LOWPASS_RADIUS * w / DEPTH_TEX_SIZE_DEFAULT,
                DEPTH_LOWPASS_RADIUS * h / DEPTH_TEX_SIZE_DEFAULT);
    }

    for (int y = 0; y < h; y++) {
        const float* guide = ctx->modelInput[pair] + (size_t)(h - 1 - y) * w * 3;
        const float* norm = ctx->depthNorm + (size_t)y * w;
        const float* low = ctx->depthLow + (size_t)y * w;
        unsigned char* dst = ctx->depthUploadBuf + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            float v = remapping ? conv + kg * (low[x] - conv) + kl * (norm[x] - low[x])
                                : norm[x];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;

            dst[x * 4 + 0] = (unsigned char)(guide[x * 3 + 0] * 255.0f + 0.5f);
            dst[x * 4 + 1] = (unsigned char)(guide[x * 3 + 1] * 255.0f + 0.5f);
            dst[x * 4 + 2] = (unsigned char)(guide[x * 3 + 2] * 255.0f + 0.5f);
            dst[x * 4 + 3] = (unsigned char)(v * 255.0f + 0.5f);
        }
    }

    int writeIndex = ctx->depthWriteIndex;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[writeIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, ctx->depthUploadBuf);

    if (ctx->depthFences[writeIndex] != NULL) {
        // The frame loop normally takes a slot's fence the frame after it is
        // published, so a live one here means it never got that far. Not a
        // rare path: frames are only rendered while the runtime asks for
        // them, while captures follow every decoded frame, so a headset off
        // the head publishes slots nothing adopts for as long as it lasts.
        glDeleteSync(ctx->depthFences[writeIndex]);
    }

    // Fence before flush, for the same reason as in the capture: a fence left
    // unflushed in this queue is one the frame loop may never see satisfied
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    ctx->depthFences[writeIndex] = fence;

    // Index and fence go out together under one release store, so the frame
    // loop can never see the new index without the fence that belongs to it
    atomic_store_explicit(&ctx->depthStagedIndex, writeIndex, memory_order_release);
    atomic_store_explicit(&ctx->depthLastPair, pair, memory_order_relaxed);
    // Counted for the splash, which waits for the first
    atomic_fetch_add_explicit(&ctx->depthMapsStaged, 1, memory_order_relaxed);

    // Always the next slot in the rotation, never a function of where the
    // frame loop currently is, which is what keeps this from landing on a slot
    // it could still have a draw in flight against
    ctx->depthWriteIndex = (writeIndex + 1) % DEPTH_TEX_COUNT;

    return nowNs() - startNs;
}

// A capture that made no map, its readback unmappable or its model run
// failed, in its turn among the maps. Whatever its cut check found goes to
// the next map made.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeDropDepth(JNIEnv* env, jobject thiz,
                                                            jlong handle, jint pair) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || !pairOk(pair)) {
        return;
    }
    depthResetsDrop(&ctx->depthResets, pair);
}

// Waits, once, for the fence guarding the slot the frame loop is about to
// sample, then discards it. Once the wait is in this context's queue every
// later command is ordered behind the stage thread's upload by the queue
// itself, so a second site in the same frame, or the same slot next frame,
// has nothing left to wait for.
void waitForDepthSlot(XrCtx* ctx) {
    GLsync fence = ctx->depthFences[ctx->depthReadIndex];
    if (fence != NULL) {
        glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(fence);
        ctx->depthFences[ctx->depthReadIndex] = NULL;
    }
}
