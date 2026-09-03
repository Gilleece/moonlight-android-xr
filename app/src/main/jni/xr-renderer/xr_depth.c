// The depth model's staging: the frame is drawn small and read back for
// the model on the frame loop, and the depth thread normalises the result
// and uploads it as the map the warp samples.
#include "xr_renderer.h"
#include "xr_depthmap.h"
#include "xr_shaders.h"

// GL side of the depth model path: the downscale target the frame is
// rendered into, and the staging buffers it is read back through
int initDepthModel(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;

    if (!linkProgram(&ctx->downscaleProgram, DOWNSCALE_FRAGMENT_SRC, "downscale")) {
        return 0;
    }
    ctx->downscaleTexMatrixUniform = glGetUniformLocation(ctx->downscaleProgram, "u_texmatrix");
    glUseProgram(ctx->downscaleProgram);
    glUniform1i(glGetUniformLocation(ctx->downscaleProgram, "u_texture"), 0);

    glGenTextures(1, &ctx->downscaleTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->downscaleTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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

    // The depth thread gets its own context in the same share group, so it
    // can upload into the back depth texture while the frame loop draws
    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx->depthContext = eglCreateContext(ctx->eglDisplay, ctx->eglConfig, ctx->eglContext,
                                         contextAttribs);
    if (ctx->depthContext == EGL_NO_CONTEXT) {
        LOGE("depth thread context creation failed: %d", eglGetError());
        return 0;
    }
    const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ctx->depthPbuffer = eglCreatePbufferSurface(ctx->eglDisplay, ctx->eglConfig, pbufferAttribs);
    if (ctx->depthPbuffer == EGL_NO_SURFACE) {
        LOGE("depth thread pbuffer creation failed: %d", eglGetError());
        return 0;
    }

    // Slot 0 is what fillSyntheticDepth and depthReadIndex both start on, so
    // the first real upload has to land somewhere else
    ctx->depthWriteIndex = 1;
    atomic_init(&ctx->depthStagedIndex, 0);

    // Storage only: each capture reads back into one of these and the depth
    // thread maps it later, so nothing is ever uploaded into them
    glGenBuffers(2, ctx->depthPbos);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depthPbos[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)n * n * 4, NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    ctx->modelInput = malloc((size_t)n * n * 3 * sizeof(float));
    ctx->modelOutput = malloc((size_t)n * n * sizeof(float));
    ctx->depthUploadBuf = malloc((size_t)n * n * 4);
    ctx->depthEma = malloc((size_t)n * n * sizeof(float));
    ctx->depthLow = malloc((size_t)n * n * sizeof(float));
    ctx->depthScratch = malloc((size_t)n * n * sizeof(float));
    ctx->depthColSums = malloc((size_t)n * sizeof(float));
    if (ctx->modelInput == NULL || ctx->modelOutput == NULL ||
            ctx->depthUploadBuf == NULL || ctx->depthEma == NULL ||
            ctx->depthLow == NULL || ctx->depthScratch == NULL ||
            ctx->depthColSums == NULL) {
        LOGE("depth staging buffer allocation failed");
        return 0;
    }

    LOGI("depth model staging ready at %dx%d", n, n);
    return 1;
}

JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetModelInput(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->modelInput == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelInput,
                                       (jlong)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * 3 * sizeof(float));
}

JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetModelOutput(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->modelOutput == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelOutput,
                                       (jlong)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * sizeof(float));
}

// Draws the current frame into the downscale target and asks for it back into
// a pixel buffer. Nothing waits here: a readback straight into memory drains
// the whole GPU queue, which at 90 Hz is most of a frame gone. The depth
// thread waits on the fence left behind here and maps the buffer right before
// the model needs it, in nativeFinishDepthCapture, which is a thread with no
// frame deadline to miss.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeCaptureDepthInput(JNIEnv* env, jobject thiz,
                                                                    jlong handle,
                                                                    jfloatArray texMatrixArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->depthPbos[0] == 0) {
        return 0;
    }
    const int n = DEPTH_TEX_SIZE;
    long startNs = nowNs();

    float texMatrix[16];
    (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->downscaleFbo);
    glViewport(0, 0, n, n);
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

    // The handoff guard on the Java side means the depth thread is always done
    // with the other slot by the time a new capture gets here, but ping
    // ponging costs nothing and leaves room if that guard ever loosens
    int slot = 1 - ctx->captureIndex;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depthPbos[slot]);
    glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (ctx->captureFences[slot] != NULL) {
        // Only reachable if that guard was bypassed, and a fence nothing ever
        // waited on would otherwise leak here
        glDeleteSync(ctx->captureFences[slot]);
    }

    // Fence first, flush second, and the order is the whole point: a flush
    // only pushes out what is already in this context's queue, so flushing
    // before the fence exists leaves the fence itself sitting unflushed. The
    // depth thread waits on it from another context and cannot flush this
    // one on our behalf, so it would block until this thread happened to
    // flush for some unrelated reason.
    ctx->captureFences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    ctx->captureIndex = slot;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    return nowNs() - startNs;
}

// Waits for the last capture's readback to land, then copies it out of the
// pixel buffer into the model input. Rows are flipped on the way: GL hands
// back the bottom row first and the model wants the image the right way up,
// since monocular depth leans heavily on which way is down. Runs on the depth
// thread right before the model reads the input, so nothing else has to keep
// the two in step. Returns the time it took, or -1 when the buffer could not
// be mapped and there is nothing to run the model on.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeFinishDepthCapture(JNIEnv* env, jobject thiz,
                                                                     jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->depthPbos[0] == 0) {
        return -1;
    }
    const int n = DEPTH_TEX_SIZE;
    long startNs = nowNs();
    int slot = ctx->captureIndex;

    GLsync fence = ctx->captureFences[slot];
    if (fence != NULL) {
        // Checked rather than discarded: on a timeout the map below still
        // hands back a buffer, so the frame would be normalised from whatever
        // the transfer had managed and the depth map would go subtly wrong
        // with nothing in the log to say why. Half a second is long past
        // anything a 256x256 readback can take, so this firing means the fence
        // never landed rather than that the GPU was busy.
        if (glClientWaitSync(fence, 0, CAPTURE_FENCE_TIMEOUT_NS) == GL_TIMEOUT_EXPIRED) {
            LOGW("depth capture: readback fence timed out, frame may be torn");
        }
        glDeleteSync(fence);
        ctx->captureFences[slot] = NULL;
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depthPbos[slot]);
    const unsigned char* pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                   (GLsizeiptr)n * n * 4, GL_MAP_READ_BIT);
    if (pixels == NULL) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        LOGW("depth readback could not be mapped: 0x%x", glGetError());
        return -1;
    }

    for (int y = 0; y < n; y++) {
        const unsigned char* src = pixels + (size_t)(n - 1 - y) * n * 4;
        float* dst = ctx->modelInput + (size_t)y * n * 3;
        for (int x = 0; x < n; x++) {
            dst[x * 3 + 0] = src[x * 4 + 0] * (1.0f / 255.0f);
            dst[x * 3 + 1] = src[x * 4 + 1] * (1.0f / 255.0f);
            dst[x * 3 + 2] = src[x * 4 + 2] * (1.0f / 255.0f);
        }
    }

    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return nowNs() - startNs;
}

// Binds the depth thread's context. Called once from that thread before it
// touches GL or creates the delegate.
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeBindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return JNI_FALSE;
    }
    if (!eglMakeCurrent(ctx->eglDisplay, ctx->depthPbuffer, ctx->depthPbuffer, ctx->depthContext)) {
        LOGE("depth thread eglMakeCurrent failed: %d", eglGetError());
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUnbindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    eglMakeCurrent(ctx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx->depthPbuffer != EGL_NO_SURFACE) {
        eglDestroySurface(ctx->eglDisplay, ctx->depthPbuffer);
        ctx->depthPbuffer = EGL_NO_SURFACE;
    }
    if (ctx->depthContext != EGL_NO_CONTEXT) {
        eglDestroyContext(ctx->eglDisplay, ctx->depthContext);
        ctx->depthContext = EGL_NO_CONTEXT;
    }
    eglReleaseThread();
}

// Normalizes the model output to 0..1 and uploads it as the depth map the
// warp samples. MiDaS emits relative inverse depth on an arbitrary scale, so
// the range has to be found per frame. Rows flip back here.
//
// Two separate temporal filters. The range is smoothed so the mapping does
// not jump when the scene changes, and the map itself is smoothed per texel
// so raw model flicker does not reach the eyes. The guide colour rides along
// in RGB so the upsampling pass gets the exact frame the depth came from.
//
// Runs on the depth thread, writing the next slot in a fixed rotation, never
// the one the frame loop is reading, then publishing it behind a fence. This
// used to finish instead, which stalled this thread until the GPU was idle
// and still did not promise the frame loop's context, a different one in the
// same share group, would see the result. A fence is something that context
// can wait on itself, at the point it samples the texture.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadDepth(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->modelOutput == NULL) {
        return 0;
    }
    const int n = DEPTH_TEX_SIZE;
    long startNs = nowNs();

    float lo, hi;
    robustRange(ctx->modelOutput, n * n, &lo, &hi);
    if (!ctx->rangeValid) {
        ctx->smoothLo = lo;
        ctx->smoothHi = hi;
        ctx->rangeValid = 1;
    }
    else {
        ctx->smoothLo += ctx->rangeAlpha * (lo - ctx->smoothLo);
        ctx->smoothHi += ctx->rangeAlpha * (hi - ctx->smoothHi);
    }
    float scale = 1.0f / (ctx->smoothHi - ctx->smoothLo);
    float alpha = ctx->depthAlpha;
    int seed = !ctx->depthEmaValid;

    for (int y = 0; y < n; y++) {
        const float* src = ctx->modelOutput + (size_t)(n - 1 - y) * n;
        float* ema = ctx->depthEma + (size_t)y * n;
        for (int x = 0; x < n; x++) {
            float v = (src[x] - ctx->smoothLo) * scale;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            ema[x] = seed ? v : ema[x] + alpha * (v - ema[x]);
        }
    }
    ctx->depthEmaValid = 1;

    float kg = ctx->depthGlobal;
    float kl = ctx->depthLocal;
    float conv = ctx->convergence;

    // The low pass is only needed to split the map into overall shape and
    // local detail, so skip it when the remap is doing nothing. It costs
    // about 10 ms on this thread, which is latency the depth map cannot
    // afford for an effect measured to be invisible.
    int remapping = kg < 0.995f || kg > 1.005f || kl < 0.995f || kl > 1.005f;
    if (remapping) {
        lowPass(ctx->depthEma, ctx->depthLow, ctx->depthScratch, ctx->depthColSums, n,
                DEPTH_LOWPASS_RADIUS);
    }

    for (int y = 0; y < n; y++) {
        const float* guide = ctx->modelInput + (size_t)(n - 1 - y) * n * 3;
        const float* ema = ctx->depthEma + (size_t)y * n;
        const float* low = ctx->depthLow + (size_t)y * n;
        unsigned char* dst = ctx->depthUploadBuf + (size_t)y * n * 4;
        for (int x = 0; x < n; x++) {
            float v = remapping ? conv + kg * (low[x] - conv) + kl * (ema[x] - low[x])
                                : ema[x];
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, ctx->depthUploadBuf);

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

    // Always the next slot in the rotation, never a function of where the
    // frame loop currently is, which is what keeps this from landing on a slot
    // it could still have a draw in flight against
    ctx->depthWriteIndex = (writeIndex + 1) % DEPTH_TEX_COUNT;

    return nowNs() - startNs;
}

// Waits, once, for the fence guarding the slot the frame loop is about to
// sample, then discards it. Once the wait is in this context's queue every
// later command is ordered behind the depth thread's upload by the queue
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
