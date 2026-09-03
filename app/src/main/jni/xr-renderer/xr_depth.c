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

    ctx->readbackBuf = malloc((size_t)n * n * 4);
    ctx->modelInput = malloc((size_t)n * n * 3 * sizeof(float));
    ctx->modelOutput = malloc((size_t)n * n * sizeof(float));
    ctx->depthUploadBuf = malloc((size_t)n * n * 4);
    ctx->depthEma = malloc((size_t)n * n * sizeof(float));
    ctx->depthLow = malloc((size_t)n * n * sizeof(float));
    ctx->depthScratch = malloc((size_t)n * n * sizeof(float));
    ctx->depthColSums = malloc((size_t)n * sizeof(float));
    if (ctx->readbackBuf == NULL || ctx->modelInput == NULL || ctx->modelOutput == NULL ||
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
    if (ctx->modelInput == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelInput,
                                       (jlong)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * 3 * sizeof(float));
}

JNIEXPORT jobject JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetModelOutput(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx->modelOutput == NULL) {
        return NULL;
    }
    return (*env)->NewDirectByteBuffer(env, ctx->modelOutput,
                                       (jlong)DEPTH_TEX_SIZE * DEPTH_TEX_SIZE * sizeof(float));
}

// Draws the current frame into the downscale target and reads it back into
// the model input buffer. Rows are flipped on the way: GL hands back the
// bottom row first and the model wants the image the right way up, since
// monocular depth leans heavily on which way is down.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeCaptureDepthInput(JNIEnv* env, jobject thiz,
                                                                    jlong handle,
                                                                    jfloatArray texMatrixArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
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

    glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, ctx->readbackBuf);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    for (int y = 0; y < n; y++) {
        const unsigned char* src = ctx->readbackBuf + (size_t)(n - 1 - y) * n * 4;
        float* dst = ctx->modelInput + (size_t)y * n * 3;
        for (int x = 0; x < n; x++) {
            dst[x * 3 + 0] = src[x * 4 + 0] * (1.0f / 255.0f);
            dst[x * 3 + 1] = src[x * 4 + 1] * (1.0f / 255.0f);
            dst[x * 3 + 2] = src[x * 4 + 2] * (1.0f / 255.0f);
        }
    }

    return nowNs() - startNs;
}

// Binds the depth thread's context. Called once from that thread before it
// touches GL or creates the delegate.
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeBindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (!eglMakeCurrent(ctx->eglDisplay, ctx->depthPbuffer, ctx->depthPbuffer, ctx->depthContext)) {
        LOGE("depth thread eglMakeCurrent failed: %d", eglGetError());
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUnbindDepthContext(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
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
// Runs on the depth thread, writing whichever texture the frame loop is not
// sampling, then publishing it. The finish is what makes the upload visible
// to the other context, and costs nothing here since this thread has no
// deadline.
JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadDepth(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
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

    int writeIndex = 1 - ctx->depthReadIndex;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->depthTextures[writeIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, ctx->depthUploadBuf);
    glFinish();
    ctx->depthReadIndex = writeIndex;

    return nowNs() - startNs;
}
