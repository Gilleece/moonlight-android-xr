// The swapchains the art lives in and the uploads that fill them, from the
// pointer and handle art drawn here to the panels and photos handed over
// from Java.
#include "xr_renderer.h"

/**
 * Makes the swapchain one piece of art lives in and fetches its images. Fails
 * closed: on any error the handle is left null, so the layer that would show
 * the art stays out of the frame rather than pointing at nothing.
 */
int createArtSwapchain(XrCtx* ctx, int width, int height, const char* what,
                       XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images,
                       uint32_t* count) {
    *chain = XR_NULL_HANDLE;
    *images = NULL;
    *count = 0;

    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = ctx->swapchainFormat;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    XrSwapchain created = XR_NULL_HANDLE;
    if (!checkXr(xrCreateSwapchain(ctx->session, &info, &created), what)) {
        return 0;
    }

    uint32_t n = 0;
    if (!checkXr(xrEnumerateSwapchainImages(created, 0, &n, NULL), what) || n == 0) {
        xrDestroySwapchain(created);
        return 0;
    }
    XrSwapchainImageOpenGLESKHR* fetched = calloc(n, sizeof(XrSwapchainImageOpenGLESKHR));
    if (fetched == NULL) {
        LOGE("%s: no memory for %u swapchain images", what, n);
        xrDestroySwapchain(created);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        fetched[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    if (!checkXr(xrEnumerateSwapchainImages(created, n, &n,
                                            (XrSwapchainImageBaseHeader*)fetched), what)) {
        free(fetched);
        xrDestroySwapchain(created);
        return 0;
    }

    *chain = created;
    *images = fetched;
    *count = n;
    return 1;
}

// The other half, safe on a chain that was never made
void destroyArtSwapchain(XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images) {
    if (*chain != XR_NULL_HANDLE) {
        xrDestroySwapchain(*chain);
        *chain = XR_NULL_HANDLE;
    }
    free(*images);
    *images = NULL;
}

// Every swapchain the furniture and the panels are shown from. Only the
// pointer's is required: anything else that fails just leaves its layer out.
int createPointerSwapchain(XrCtx* ctx) {
    if (!createArtSwapchain(ctx, PTR_TEX_W, PTR_TEX_H, "create pointer swapchain",
                            &ctx->pointerSwapchain, &ctx->pointerImages,
                            &ctx->pointerImageCount)) {
        return 0;
    }

    // Handles get a swapchain each rather than a corner of the atlas, so there
    // is no image rect origin convention to guess at
    createArtSwapchain(ctx, BAR_TEX_W, BAR_TEX_H, "create bar swapchain",
                       &ctx->barSwapchain, &ctx->barImages, &ctx->barImageCount);

    createArtSwapchain(ctx, PICKER_TEX_W, PICKER_TEX_H, "create picker swapchain",
                       &ctx->pickerSwapchain, &ctx->pickerImages, &ctx->pickerImageCount);

    for (int tab = 0; tab < COG_ART_COUNT; tab++) {
        createArtSwapchain(ctx, COG_TEX_W, COG_TEX_H, "create cog panel swapchain",
                           &ctx->cogPanelSwapchains[tab], &ctx->cogPanelImages[tab],
                           &ctx->cogPanelImageCounts[tab]);
    }

    createArtSwapchain(ctx, COG_THUMB_TEX, COG_THUMB_TEX, "create cog thumb swapchain",
                       &ctx->cogThumbSwapchain, &ctx->cogThumbImages, &ctx->cogThumbImageCount);

    for (int state = 0; state < KB_STATE_COUNT; state++) {
        createArtSwapchain(ctx, KB_TEX_W, KB_TEX_H, "create keyboard swapchain",
                           &ctx->kbPanelSwapchains[state], &ctx->kbPanelImages[state],
                           &ctx->kbPanelImageCounts[state]);
    }

    for (int sheet = 0; sheet < EXIT_ART_COUNT; sheet++) {
        createArtSwapchain(ctx, EXIT_TEX_W, EXIT_TEX_H, "create exit prompt swapchain",
                           &ctx->exitPromptSwapchains[sheet], &ctx->exitPromptImages[sheet],
                           &ctx->exitPromptImageCounts[sheet]);
    }

    createArtSwapchain(ctx, BUTTON_TEX, BUTTON_TEX, "create keyboard button swapchain",
                       &ctx->kbButtonSwapchain, &ctx->kbButtonImages, &ctx->kbButtonImageCount);
    createArtSwapchain(ctx, BUTTON_TEX, BUTTON_TEX, "create cog button swapchain",
                       &ctx->cogButtonSwapchain, &ctx->cogButtonImages, &ctx->cogButtonImageCount);
    createArtSwapchain(ctx, BUTTON_TEX, BUTTON_TEX, "create env button swapchain",
                       &ctx->envButtonSwapchain, &ctx->envButtonImages, &ctx->envButtonImageCount);
    createArtSwapchain(ctx, BUTTON_TEX, BUTTON_TEX, "create exit button swapchain",
                       &ctx->exitButtonSwapchain, &ctx->exitButtonImages,
                       &ctx->exitButtonImageCount);

    // Two padlocks rather than one, since a quad layer has no way to swap
    // its own texture and open and shut have to read differently
    createArtSwapchain(ctx, LOCK_TEX, LOCK_TEX, "create lock swapchain",
                       &ctx->lockSwapchain, &ctx->lockImages, &ctx->lockImageCount);
    createArtSwapchain(ctx, LOCK_TEX, LOCK_TEX, "create unlock swapchain",
                       &ctx->unlockSwapchain, &ctx->unlockImages, &ctx->unlockImageCount);

    createArtSwapchain(ctx, OUTLINE_TEX, OUTLINE_TEX, "create outline swapchain",
                       &ctx->outlineSwapchain, &ctx->outlineImages, &ctx->outlineImageCount);

    // The one chain here that is redrawn every frame rather than filled once,
    // since it is made out of whatever the picture is showing
    createArtSwapchain(ctx, GLOW_TEX, GLOW_TEX, "create glow swapchain",
                       &ctx->glowSwapchain, &ctx->glowImages, &ctx->glowImageCount);

    createArtSwapchain(ctx, CORNER_TEX_W, CORNER_TEX_H, "create corner swapchain",
                       &ctx->cornerSwapchain, &ctx->cornerImages, &ctx->cornerImageCount);

    return 1;
}

// Uploads one CPU buffer into a swapchain and hands the image straight back
static int uploadArt(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                     const unsigned char* px, int width, int height) {
    if (chain == XR_NULL_HANDLE) {
        return 0;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(chain, &acquire, &index), "acquire art image")) {
        return 0;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(chain, &wait);

    glBindTexture(GL_TEXTURE_2D, images[index].image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(chain, &release);
    return 1;
}

// Rows arrive bottom up, so a photo uploaded as it comes would put the sky
// underfoot
static int uploadFlipped(XrCtx* ctx, XrSwapchain chain, XrSwapchainImageOpenGLESKHR* images,
                         const unsigned char* px, int width, int height) {
    size_t stride = (size_t)width * 4;
    unsigned char* flipped = malloc(stride * height);
    if (flipped == NULL) {
        return 0;
    }
    for (int y = 0; y < height; y++) {
        memcpy(flipped + stride * y, px + stride * (height - 1 - y), stride);
    }
    int ok = uploadArt(ctx, chain, images, flipped, width, height);
    free(flipped);
    return ok;
}

// Soft edged coverage for a distance from a shape, in pixels
static float edgeAlpha(float distance, float halfStroke) {
    float a = (halfStroke - distance) / 1.5f + 0.5f;
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

static void buildHandleArt(XrCtx* ctx) {
    unsigned char* bar = calloc(BAR_TEX_W * BAR_TEX_H * 4, 1);
    unsigned char* corner = calloc(CORNER_TEX_W * CORNER_TEX_H * 4, 1);
    if (bar == NULL || corner == NULL) {
        free(bar);
        free(corner);
        return;
    }

    // A rounded bar, symmetric, so the row order does not matter here
    float barR = BAR_TEX_H * 0.5f;
    for (int y = 0; y < BAR_TEX_H; y++) {
        for (int x = 0; x < BAR_TEX_W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float cx = px;
            if (cx < barR) cx = barR;
            if (cx > BAR_TEX_W - barR) cx = BAR_TEX_W - barR;
            float dx = px - cx, dy = py - barR;
            float d = sqrtf(dx * dx + dy * dy);
            unsigned char* p = bar + ((y * BAR_TEX_W) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, barR - 1.0f) * 235.0f);
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    // A rounded bracket whose outer corner sits at the middle of the tile, with
    // the two runs going right and down from it, so centring the quad on a
    // corner of the screen wraps that corner. Rows are written bottom up: a
    // buffer uploaded the normal way arrives vertically flipped.
    const float mid = CORNER_TEX_W * 0.5f;
    const float arcR = 10.0f;
    const float stroke = 3.0f;
    for (int y = 0; y < CORNER_TEX_H; y++) {
        for (int x = 0; x < CORNER_TEX_W; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float d;
            if (px < mid + arcR && py < mid + arcR) {
                float ax = px - (mid + arcR), ay = py - (mid + arcR);
                d = fabsf(sqrtf(ax * ax + ay * ay) - arcR);
            }
            else if (px >= mid + arcR) {
                d = fabsf(py - mid);
            }
            else {
                d = fabsf(px - mid);
            }
            unsigned char* p = corner + (((CORNER_TEX_H - 1 - y) * CORNER_TEX_W) + x) * 4;
            unsigned char a = (unsigned char)(edgeAlpha(d, stroke) * 235.0f);
            p[0] = p[1] = p[2] = a;
            p[3] = a;
        }
    }

    unsigned char* outline = calloc(OUTLINE_TEX * OUTLINE_TEX * 4, 1);
    if (outline != NULL) {
        // Rounded rectangle border, used to mark the hovered and the selected
        // cell in the picker
        const float radius = 16.0f;
        const float border = 2.5f;
        const float half = OUTLINE_TEX * 0.5f;
        for (int y = 0; y < OUTLINE_TEX; y++) {
            for (int x = 0; x < OUTLINE_TEX; x++) {
                // Signed distance to a rounded rectangle, so the ring is just
                // the pixels whose distance is under the border width
                float qx = fabsf(x + 0.5f - half) - (half - radius);
                float qy = fabsf(y + 0.5f - half) - (half - radius);
                float mx = qx > 0.0f ? qx : 0.0f;
                float my = qy > 0.0f ? qy : 0.0f;
                float outside = sqrtf(mx * mx + my * my);
                float inside = (qx > qy ? qx : qy);
                if (inside > 0.0f) {
                    inside = 0.0f;
                }
                float dist = fabsf(outside + inside);

                unsigned char a = (unsigned char)(edgeAlpha(dist, border) * 255.0f);
                unsigned char* p = outline + ((y * OUTLINE_TEX) + x) * 4;
                p[0] = p[1] = p[2] = a;
                p[3] = a;
            }
        }
    }

    // The dot a settings slider is dragged by. Round and centred, so like the
    // bar it does not care which way up it is uploaded.
    unsigned char* thumb = calloc(COG_THUMB_TEX * COG_THUMB_TEX * 4, 1);
    if (thumb != NULL) {
        const float thumbMid = COG_THUMB_TEX * 0.5f;
        const float thumbR = COG_THUMB_TEX * 0.42f;
        for (int y = 0; y < COG_THUMB_TEX; y++) {
            for (int x = 0; x < COG_THUMB_TEX; x++) {
                float dx = x + 0.5f - thumbMid, dy = y + 0.5f - thumbMid;
                float d = sqrtf(dx * dx + dy * dy);
                unsigned char* p = thumb + ((y * COG_THUMB_TEX) + x) * 4;
                unsigned char a = (unsigned char)(edgeAlpha(d, thumbR) * 235.0f);
                p[0] = p[1] = p[2] = a;
                p[3] = a;
            }
        }
    }

    int ok = uploadArt(ctx, ctx->barSwapchain, ctx->barImages, bar, BAR_TEX_W, BAR_TEX_H);
    if (thumb != NULL) {
        ctx->cogThumbReady = uploadArt(ctx, ctx->cogThumbSwapchain, ctx->cogThumbImages,
                                       thumb, COG_THUMB_TEX, COG_THUMB_TEX);
        free(thumb);
    }
    if (outline != NULL) {
        ctx->outlineReady = uploadArt(ctx, ctx->outlineSwapchain, ctx->outlineImages,
                                      outline, OUTLINE_TEX, OUTLINE_TEX);
        free(outline);
    }
    ok &= uploadArt(ctx, ctx->cornerSwapchain, ctx->cornerImages, corner,
                    CORNER_TEX_W, CORNER_TEX_H);
    ctx->handleArtReady = ok;

    free(bar);
    free(corner);
}

// Has to run on the frame loop with the session going. Waiting on a swapchain
// image at init time blocks until the runtime is ready to hand one over, which
// on a session that has not begun is never, and the whole session hangs behind
// it with the shell stuck on its loading screen.
int uploadPointerArt(XrCtx* ctx) {
    unsigned char* px = calloc(PTR_TEX_W * PTR_TEX_H * 4, 1);
    if (px == NULL) {
        return 0;
    }

    const float half = PTR_TEX_W * 0.5f;
    for (int y = 0; y < PTR_BEAM_H; y++) {
        // Fades at both ends. Which end of the texture meets the hand depends
        // on how the runtime orients the image, and symmetric art does not care
        float along = (y + 0.5f) / PTR_BEAM_H;
        float edge = along < 0.5f ? along : 1.0f - along;
        float lengthFade = edge < 0.12f ? edge / 0.12f : 1.0f;
        for (int x = 0; x < PTR_TEX_W; x++) {
            float r = fabsf((x + 0.5f) - half) / half;
            float t = r * 3.2f;
            float a = expf(-t * t) * lengthFade;
            unsigned char* p = px + ((y * PTR_TEX_W) + x) * 4;
            unsigned char lit = (unsigned char)(a * 255.0f + 0.5f);
            p[0] = lit;
            p[1] = lit;
            p[2] = lit;
            p[3] = lit;
        }
    }

    for (int y = 0; y < PTR_DOT_H; y++) {
        for (int x = 0; x < PTR_TEX_W; x++) {
            float dx = ((x + 0.5f) - half) / half;
            float dy = ((y + 0.5f) - PTR_DOT_H * 0.5f) / (PTR_DOT_H * 0.5f);
            float r = sqrtf(dx * dx + dy * dy);
            // Solid core with a soft edge, and a darker rim so it stays
            // visible against a bright picture
            float a = r < 0.45f ? 1.0f : (r < 0.75f ? (0.75f - r) / 0.30f : 0.0f);
            float shade = r < 0.35f ? 1.0f : 0.25f;
            unsigned char* p = px + (((PTR_BEAM_H + y) * PTR_TEX_W) + x) * 4;
            unsigned char lit = (unsigned char)(a * 255.0f * shade + 0.5f);
            p[0] = lit;
            p[1] = lit;
            p[2] = lit;
            p[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (checkXr(xrAcquireSwapchainImage(ctx->pointerSwapchain, &acquire, &index),
                "acquire pointer image")) {
        XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(ctx->pointerSwapchain, &wait);

        glBindTexture(GL_TEXTURE_2D, ctx->pointerImages[index].image);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PTR_TEX_W, PTR_TEX_H,
                        GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindTexture(GL_TEXTURE_2D, 0);

        XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(ctx->pointerSwapchain, &release);
        // Drawn once and submitted from then on, the art never changes
        ctx->pointerArtReady = 1;
    }

    free(px);
    if (ctx->pointerArtReady) {
        buildHandleArt(ctx);
    }
    return ctx->pointerArtReady;
}

// One sheet of art drawn in Java, a direct buffer of RGBA rows running top
// down. A sheet that never arrived leaves the swapchain and its ready flag as
// they were, so a panel that failed to draw is simply not shown.
static void uploadSheet(JNIEnv* env, XrCtx* ctx, jobject buffer, XrSwapchain chain,
                        XrSwapchainImageOpenGLESKHR* images, int width, int height,
                        int* ready) {
    if (buffer == NULL) {
        return;
    }
    const unsigned char* px = (*env)->GetDirectBufferAddress(env, buffer);
    if (px != NULL) {
        *ready = uploadFlipped(ctx, chain, images, px, width, height);
    }
}

// The thumbnail grid and the button that opens it, both drawn as Bitmaps in
// Java. Same frame loop rule as the rest of the art. Flipped on the way in,
// since a Bitmap runs top down and a texture does not.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadPicker(JNIEnv* env, jobject thiz,
                                                               jlong handle, jobject grid,
                                                               jobject button) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    uploadSheet(env, ctx, grid, ctx->pickerSwapchain, ctx->pickerImages,
                PICKER_TEX_W, PICKER_TEX_H, &ctx->pickerReady);
    uploadSheet(env, ctx, button, ctx->envButtonSwapchain, ctx->envButtonImages,
                BUTTON_TEX, BUTTON_TEX, &ctx->envButtonReady);
    LOGI("picker art %s, button %s", ctx->pickerReady ? "ready" : "missing",
         ctx->envButtonReady ? "ready" : "missing");
}

// The settings panel and the cog that opens it, drawn in Java for the same
// reason the grid is: the labels are text. Every sheet arrives together and is
// uploaded once, so changing tab later touches nothing. The last one is the
// screen tab as it reads inside a room.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadCog(JNIEnv* env, jobject thiz,
                                                            jlong handle, jobject screenTab,
                                                            jobject displayTab, jobject tab3d,
                                                            jobject roomTab, jobject button) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    jobject tabs[COG_ART_COUNT] = { screenTab, displayTab, tab3d, roomTab };
    for (int tab = 0; tab < COG_ART_COUNT; tab++) {
        uploadSheet(env, ctx, tabs[tab], ctx->cogPanelSwapchains[tab], ctx->cogPanelImages[tab],
                    COG_TEX_W, COG_TEX_H, &ctx->cogPanelReady[tab]);
    }
    uploadSheet(env, ctx, button, ctx->cogButtonSwapchain, ctx->cogButtonImages,
                BUTTON_TEX, BUTTON_TEX, &ctx->cogButtonReady);
    LOGI("cog tabs %s, %s and %s, room screen %s, button %s",
         ctx->cogPanelReady[COG_TAB_SCREEN] ? "ready" : "missing",
         ctx->cogPanelReady[COG_TAB_DISPLAY] ? "ready" : "missing",
         ctx->cogPanelReady[COG_TAB_3D] ? "ready" : "missing",
         ctx->cogPanelReady[COG_ART_ROOM_SCREEN] ? "ready" : "missing",
         ctx->cogButtonReady ? "ready" : "missing");
}

// The keyboard: a sheet of art per state, the button that opens it, and the
// layout itself. Drawing and layout both live in Java so they cannot disagree,
// and this side keeps only the rectangles and the codes behind them.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadKeyboard(JNIEnv* env, jobject thiz,
                                                                  jlong handle, jobject lower,
                                                                  jobject upper, jobject symbols,
                                                                  jobject buttonIcon,
                                                                  jfloatArray keyRects,
                                                                  jintArray codesLower,
                                                                  jintArray codesUpper,
                                                                  jintArray codesSymbols) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }

    jobject sheets[KB_STATE_COUNT] = { lower, upper, symbols };
    for (int state = 0; state < KB_STATE_COUNT; state++) {
        uploadSheet(env, ctx, sheets[state], ctx->kbPanelSwapchains[state],
                    ctx->kbPanelImages[state], KB_TEX_W, KB_TEX_H, &ctx->kbPanelReady[state]);
    }
    uploadSheet(env, ctx, buttonIcon, ctx->kbButtonSwapchain, ctx->kbButtonImages,
                BUTTON_TEX, BUTTON_TEX, &ctx->kbButtonReady);

    jintArray tables[KB_STATE_COUNT] = { codesLower, codesUpper, codesSymbols };
    if (keyRects != NULL && codesLower != NULL && codesUpper != NULL && codesSymbols != NULL) {
        int count = (*env)->GetArrayLength(env, keyRects) / 4;
        for (int state = 0; state < KB_STATE_COUNT; state++) {
            int codes = (*env)->GetArrayLength(env, tables[state]);
            if (codes < count) {
                count = codes;
            }
        }
        if (count > KB_MAX_KEYS) {
            LOGW("keyboard layout has %d keys, keeping the first %d", count, KB_MAX_KEYS);
            count = KB_MAX_KEYS;
        }
        (*env)->GetFloatArrayRegion(env, keyRects, 0, count * 4, ctx->kbKeyRects);
        for (int state = 0; state < KB_STATE_COUNT; state++) {
            (*env)->GetIntArrayRegion(env, tables[state], 0, count, ctx->kbCodes[state]);
        }
        ctx->kbKeyCount = count;
    }

    LOGI("keyboard art %s, %s and %s, button %s, %d keys",
         ctx->kbPanelReady[KB_STATE_LOWER] ? "ready" : "missing",
         ctx->kbPanelReady[KB_STATE_UPPER] ? "ready" : "missing",
         ctx->kbPanelReady[KB_STATE_SYMBOLS] ? "ready" : "missing",
         ctx->kbButtonReady ? "ready" : "missing", ctx->kbKeyCount);
}

// The exit button and the prompt behind it. One sheet per lit button, handed
// over in zone order, so which one is showing is a swapchain handle rather than
// an upload.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadExit(JNIEnv* env, jobject thiz,
                                                              jlong handle, jobject button,
                                                              jobject promptPlain,
                                                              jobject promptExitHot,
                                                              jobject promptCancelHot) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    uploadSheet(env, ctx, button, ctx->exitButtonSwapchain, ctx->exitButtonImages,
                BUTTON_TEX, BUTTON_TEX, &ctx->exitButtonReady);

    jobject sheets[EXIT_ART_COUNT] = { promptPlain, promptExitHot, promptCancelHot };
    for (int sheet = 0; sheet < EXIT_ART_COUNT; sheet++) {
        uploadSheet(env, ctx, sheets[sheet], ctx->exitPromptSwapchains[sheet],
                    ctx->exitPromptImages[sheet], EXIT_TEX_W, EXIT_TEX_H,
                    &ctx->exitPromptReady[sheet]);
    }

    LOGI("exit button %s, prompt %s, %s and %s",
         ctx->exitButtonReady ? "ready" : "missing",
         ctx->exitPromptReady[EXIT_ZONE_NONE] ? "ready" : "missing",
         ctx->exitPromptReady[EXIT_ZONE_EXIT] ? "ready" : "missing",
         ctx->exitPromptReady[EXIT_ZONE_CANCEL] ? "ready" : "missing");
}

// The two padlocks, shut and open. Both or neither, since one on its own
// would leave the button blank in half its states.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadLock(JNIEnv* env, jobject thiz,
                                                             jlong handle, jobject shut,
                                                             jobject open) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || shut == NULL || open == NULL) {
        return;
    }
    const char* shutPx = (*env)->GetDirectBufferAddress(env, shut);
    const char* openPx = (*env)->GetDirectBufferAddress(env, open);
    if (shutPx == NULL || openPx == NULL) {
        return;
    }
    ctx->lockArtReady = uploadFlipped(ctx, ctx->lockSwapchain, ctx->lockImages,
                                      (const unsigned char*)shutPx, LOCK_TEX, LOCK_TEX)
            && uploadFlipped(ctx, ctx->unlockSwapchain, ctx->unlockImages,
                             (const unsigned char*)openPx, LOCK_TEX, LOCK_TEX);
    LOGI("lock art %s", ctx->lockArtReady ? "ready" : "missing");
}

// Which cell the picker is showing as chosen, so it survives a restart
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetEnvironment(JNIEnv* env, jobject thiz,
                                                                 jlong handle, jint choice,
                                                                 jboolean backgroundOn) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    ctx->pickerChoice = choice;
    ctx->backgroundEnabled = backgroundOn;
    if (choice == ENV_CELL_MINIMAL_ROOM) {
        ctx->roomStyle = ROOM_STYLE_MINIMAL;
    }
    else if (choice == ENV_CELL_PSX_CINEMA) {
        ctx->roomStyle = ROOM_STYLE_PSX;
    }
    else {
        ctx->roomStyle = 0;
    }
    if (choice != ctx->loggedChoice) {
        ctx->loggedChoice = choice;
        LOGEV("environment %d, room %d", choice, roomEffective(ctx));
    }
}

// The 360 photo, uploaded once from the frame loop. Same rule as the rest of
// the art: a swapchain image cannot be waited on before the session runs.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadBackground(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint width, jint height) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || width <= 0 || height <= 0) {
        return;
    }
    if (!ctx->equirectSupported) {
        LOGW("no equirect layer support, skipping the background");
        return;
    }

    const unsigned char* px = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (px == NULL) {
        return;
    }

    // Switching environment reuses the swapchain, since every one of them is
    // the same size. Only a different size needs a new one.
    if (ctx->backgroundSwapchain != XR_NULL_HANDLE
            && (ctx->backgroundWidth != width || ctx->backgroundHeight != height)) {
        destroyArtSwapchain(&ctx->backgroundSwapchain, &ctx->backgroundImages);
        ctx->backgroundReady = 0;
    }

    if (ctx->backgroundSwapchain == XR_NULL_HANDLE) {
        if (!createArtSwapchain(ctx, width, height, "create background swapchain",
                                &ctx->backgroundSwapchain, &ctx->backgroundImages,
                                &ctx->backgroundImageCount)) {
            return;
        }
        ctx->backgroundWidth = width;
        ctx->backgroundHeight = height;
    }

    ctx->backgroundReady = uploadFlipped(ctx, ctx->backgroundSwapchain, ctx->backgroundImages,
                                         px, width, height);
    LOGI("background %dx%d %s", width, height, ctx->backgroundReady ? "ready" : "failed");
}

// Pixels come from a Bitmap the stats are drawn into on the Java side, which
// is the only place Android will lay out text. Runs on the frame loop thread
// so the GL context is current, and only when the text actually changed.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadOverlay(JNIEnv* env, jobject thiz,
                                                                jlong handle, jobject buffer,
                                                                jint width, jint height) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || ctx->overlaySwapchain == XR_NULL_HANDLE) {
        return;
    }
    void* pixels = (*env)->GetDirectBufferAddress(env, buffer);
    if (pixels == NULL || width != OVERLAY_WIDTH || height != OVERLAY_HEIGHT) {
        return;
    }

    if (uploadArt(ctx, ctx->overlaySwapchain, ctx->overlayImages, pixels, width, height)) {
        ctx->overlayHasContent = 1;
    }
}
