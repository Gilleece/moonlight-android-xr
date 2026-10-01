// The end of the frame: every composition layer the session shows,
// assembled in draw order and handed to the compositor.
#include "xr_renderer.h"

// Worst case reachable is a tab with six rows open: the glow, both eyes,
// stats, the cog button, the panel, six thumbs, ray and cursor, which is 14.
// A room adds its own layer, but in one the screen tab gives way to the Room
// tab and the move pill goes: three rings, three thumbs and the strip of
// percents beside them come to 16 with the room, the glow and the stats all
// up, which is the Pico's limit and no further. The display tab keeps its
// rows in a room: its seven rings and the glow level thumb over the rest come
// to 17 at its fullest, one past the Pico's sixteen, so a frame over the
// runtime's limit sheds the hover ring (see nativeEndFrame). The 3D tab, with
// the ring on its preset, the ring on its switch, the hover ring and its two
// thumbs, comes to 15 in a room. The panel is modal, and since the frame a
// modal opens now sheds the bar furniture too, the two can no longer land in
// one frame together.
// The bar itself, with the pill, all five buttons including the 3D switch and
// the padlock up over the glow, both eyes, the stats, ray and cursor, comes to
// 13, and to 13 in a room, where the pill gives way to the room's own layer.
// Switching the 3D off only ever takes a layer away: both eyes are then one.
// The keyboard sheds the same furniture and adds only its panel and one
// ring, so it comes to 9. The exit prompt sheds it too and adds its own
// sheet and the button that opened it, so it comes to less again. Sized
// well past that anyway: an overflow here is a smashed stack, and the
// margin costs five pointers.
#define FRAME_MAX_LAYERS 20

// Every composition layer a frame can carry, on nativeEndFrame's stack for as
// long as xrEndFrame needs them
typedef struct {
    XrCompositionLayerProjection room;
    XrCompositionLayerProjectionView roomViews[ROOM_EYES];
    XrCompositionLayerQuad glow;
    XrCompositionLayerQuad video[2];
    XrCompositionLayerCylinderKHR cylinder[2];
    XrCompositionLayerQuad overlay;
    XrCompositionLayerQuad handle;
    XrCompositionLayerQuad envButton;
    XrCompositionLayerQuad cogButton;
    XrCompositionLayerQuad kbButton;
    XrCompositionLayerQuad exitButton;
    XrCompositionLayerQuad stereoButton;
    XrCompositionLayerQuad exitPrompt;
    XrCompositionLayerQuad lock;
    XrCompositionLayerQuad picker;
    XrCompositionLayerQuad outline[2];
    XrCompositionLayerQuad cogPanel;
    // One per option row for what is chosen, plus one for the hover
    XrCompositionLayerQuad cogMark[COG_OPTION_COUNT + 1];
    XrCompositionLayerQuad cogReadout;
    // One per row of whichever tab has the most. The display tab's glow level
    // track is its seventh row, so it is the one that sets the size.
    XrCompositionLayerQuad cogThumb[COG_DISPLAY_SLIDER_ROW + 1 > COG_SLIDER_COUNT
                                    ? COG_DISPLAY_SLIDER_ROW + 1 : COG_SLIDER_COUNT];
    XrCompositionLayerQuad kbPanel;
    XrCompositionLayerQuad kbMark;
    XrCompositionLayerQuad beam;
    XrCompositionLayerQuad dot;
    XrCompositionLayerSettingsFB settings;
    // NULL when sharpening and supersampling are both off or unsupported,
    // which leaves every chain untouched
    const void* settingsChain;
    const XrCompositionLayerBaseHeader* order[FRAME_MAX_LAYERS];
    uint32_t count;
} FrameLayers;

// What every layer builder reads about this frame, worked out once
typedef struct {
    XrSpace space;
    XrPosef screenPose;
    float screenWidth;
    float screenHeight;
    float aspect;
    float curve;
    int screenCurved;
    int stereo;
    int roomOn;
    int headLocked;
    int eyeSwap;
    // The bar and the two buttons beside it share a hover area, so reaching
    // for one keeps the others on screen rather than swapping them. A modal
    // takes it away on the very frame it opens: the hover is still on the
    // button that was pressed, so the furniture and the panel would both go
    // up for one frame, and that stack overflowed the runtime's layer limit
    // and cost the whole frame with a -24 on device.
    int barArea;
} FrameView;

// Takes a layer back out of the frame, keeping the order of the rest
static void dropLayer(FrameLayers* layers, const void* layer) {
    for (uint32_t i = 0; i < layers->count; i++) {
        if (layers->order[i] == (const XrCompositionLayerBaseHeader*)layer) {
            memmove(&layers->order[i], &layers->order[i + 1],
                    (layers->count - i - 1) * sizeof(layers->order[0]));
            layers->count--;
            return;
        }
    }
}

// Adds a layer to the frame. One past the array would be a smashed stack, so a
// frame that gets there drops the layer and says so once: the layer that went
// missing points at whatever grew.
static void pushLayer(XrCtx* ctx, FrameLayers* layers, const void* layer) {
    if (layers->count >= FRAME_MAX_LAYERS) {
        if (!ctx->layerDropWarned) {
            ctx->layerDropWarned = 1;
            LOGE("frame needs more than %d composition layers, dropping the rest",
                 FRAME_MAX_LAYERS);
        }
        return;
    }
    layers->order[layers->count++] = (const XrCompositionLayerBaseHeader*)layer;
}

// A quad in the given space showing the whole of one swapchain image
static void quadLayer(XrCompositionLayerQuad* quad, const void* next,
                      XrCompositionLayerFlags flags, XrSwapchain chain, int texW, int texH,
                      XrSpace space, XrPosef pose, float width, float height) {
    memset(quad, 0, sizeof(*quad));
    quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad->next = next;
    quad->layerFlags = flags;
    quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad->subImage.swapchain = chain;
    quad->subImage.imageRect.offset.x = 0;
    quad->subImage.imageRect.offset.y = 0;
    quad->subImage.imageRect.extent.width = texW;
    quad->subImage.imageRect.extent.height = texH;
    quad->subImage.imageArrayIndex = 0;
    quad->space = space;
    quad->pose = pose;
    quad->size.width = width;
    quad->size.height = height;
}

// The pose a local offset from a base pose lands at, facing the same way
static XrPosef poseOffset(XrPosef base, Vec3 local) {
    Vec3 offset = quatRotate(base.orientation, local);
    XrPosef pose;
    pose.orientation = base.orientation;
    pose.position.x = base.position.x + offset.x;
    pose.position.y = base.position.y + offset.y;
    pose.position.z = base.position.z + offset.z;
    return pose;
}

// A line of warp timings every STATS_LOG_INTERVAL_FRAMES frames, then the counters start over
static void logWarpStats(XrCtx* ctx) {
    if (ctx->statFrames == STATS_LOG_INTERVAL_FRAMES) {
        // The room is timed separately, so it is reported separately: kept
        // of harvested, since only a fraction of its queries come back with
        // anything usable in them. Nothing is said when it is not on.
        char roomLine[64];
        roomLine[0] = '\0';
        if (ctx->roomGpuSamples > 0) {
            snprintf(roomLine, sizeof(roomLine), ", room avg %.2f ms (%ld of %ld)",
                     ctx->roomGpuTotalNs / (double)ctx->roomGpuSamples / 1e6,
                     ctx->roomGpuSamples, ctx->roomGpuSamples + ctx->roomGpuDropped);
        }
        else if (ctx->roomGpuDropped > 0) {
            snprintf(roomLine, sizeof(roomLine), ", room timer starved (%ld dropped)",
                     ctx->roomGpuDropped);
        }
        // Submit is the wall clock around the draw calls, which is only
        // how long the driver took to queue them. GPU is the real cost.
        if (ctx->gpuSamples > 0) {
            LOGI("XR warp: %ld frames, GPU avg %.2f ms, GPU max %.2f ms, submit avg %.2f ms, dropped %ld%s",
                 ctx->statFrames, ctx->gpuTotalNs / (double)ctx->gpuSamples / 1e6,
                 ctx->gpuMaxNs / 1e6,
                 ctx->statTotalNs / (double)ctx->statFrames / 1e6,
                 ctx->gpuDropped, roomLine);
        }
        else {
            // The raw value says which way the driver failed: zeros and
            // wrapped negatives are different diseases
            LOGI("XR warp: %ld frames, submit avg %.2f ms, max %.2f ms (no GPU timer, dropped %ld, last raw %llu)%s",
                 ctx->statFrames, ctx->statTotalNs / (double)ctx->statFrames / 1e6,
                 ctx->statMaxNs / 1e6, ctx->gpuDropped,
                 (unsigned long long)ctx->gpuLastDroppedNs, roomLine);
        }
        ctx->statFrames = 0;
        ctx->statTotalNs = 0;
        ctx->statMaxNs = 0;
        ctx->gpuTotalNs = 0;
        ctx->gpuMaxNs = 0;
        ctx->gpuSamples = 0;
        ctx->gpuDropped = 0;
        ctx->roomGpuTotalNs = 0;
        ctx->roomGpuSamples = 0;
        ctx->roomGpuDropped = 0;
    }
}

// Compositor sharpening and supersampling, both done in its sampling pass, so
// neither costs the app anything. Supersampling is the compositor filtering
// harder where a layer is drawn smaller than its texture, which a 4K picture
// on a headset usually is. The extension allows one bit of each in the same
// word. One struct serves every layer that wants it, and NULL when both are
// off or the runtime lacks the extension leaves every chain untouched.
static void setLayerSettings(XrCtx* ctx, FrameLayers* layers) {
    // Said whenever either moves, so a session log shows which half of an
    // A/B each stretch was, and once at the start
    int logged = ctx->sharpenMode * 3 + ctx->supersampleMode + 1;
    if (logged != ctx->layerFlagsLogged) {
        ctx->layerFlagsLogged = logged;
        LOGEV("layer flags: sharpen %d supersample %d%s", ctx->sharpenMode,
              ctx->supersampleMode, ctx->layerSettingsSupported ? "" : " (not offered, ignored)");
    }
    layers->settingsChain = NULL;
    if (!ctx->layerSettingsSupported) {
        return;
    }
    XrCompositionLayerSettingsFlagsFB flags = 0;
    if (ctx->sharpenMode == 1) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
    }
    else if (ctx->sharpenMode == 2) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
    }
    if (ctx->supersampleMode == 1) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
    }
    else if (ctx->supersampleMode == 2) {
        flags |= XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB;
    }
    if (flags != 0) {
        memset(&layers->settings, 0, sizeof(layers->settings));
        layers->settings.type = XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB;
        layers->settings.layerFlags = flags;
        layers->settingsChain = &layers->settings;
    }
}

// The 3d room, drawn per eye into the one projection layer
static void addRoomLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The environment, when it is a room. Passthrough wants the real room
    // instead, so the two never go up together.
    if (view->roomOn && ctx->roomRendered && ctx->roomViewsValid && !ctx->passthrough) {
        XrCompositionLayerProjection* room = &layers->room;
        memset(room, 0, sizeof(*room));
        memset(layers->roomViews, 0, sizeof(layers->roomViews));
        room->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        room->layerFlags = 0;
        // World locked, even when the screen is head locked, or the room would
        // swing about with the viewer
        room->space = ctx->localSpace;
        room->viewCount = ROOM_EYES;
        room->views = layers->roomViews;
        for (int eye = 0; eye < ROOM_EYES; eye++) {
            XrCompositionLayerProjectionView* projView = &layers->roomViews[eye];
            projView->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            // The poses the image was actually drawn from, so the compositor
            // reprojects it rather than being told a pose it does not match
            projView->pose = ctx->roomViews[eye].pose;
            projView->fov = ctx->roomViews[eye].fov;
            projView->subImage.swapchain = ctx->roomSwapchain;
            projView->subImage.imageRect.offset.x = eye * ctx->roomEyeWidth;
            projView->subImage.imageRect.offset.y = 0;
            projView->subImage.imageRect.extent.width = ctx->roomEyeWidth;
            projView->subImage.imageRect.extent.height = ctx->roomEyeHeight;
            projView->subImage.imageArrayIndex = 0;
        }
        pushLayer(ctx, layers, room);
    }
}

// The ambilight glow behind the picture
static void addGlowLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The glow, over the environment and under the picture. Deliberately not
    // sharpened: being soft is the whole of the effect.
    int glowOn;
    float glowLevel;
    ambiEffective(ctx, &glowOn, &glowLevel);
    if (glowOn && ctx->glowRendered && ctx->everRendered && ctx->shouldRender) {
        // Local +z is behind the picture, the same direction the cylinder puts
        // its axis. Far enough back that the two never z fight, near enough
        // that the glow reads as coming off the screen.
        Vec3 behindLocal = { 0.0f, 0.0f, GLOW_BEHIND_M };
        quadLayer(&layers->glow, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->glowSwapchain, GLOW_TEX, GLOW_TEX, view->space,
                  poseOffset(view->screenPose, behindLocal),
                  view->screenWidth * GLOW_SCALE, view->screenHeight * GLOW_SCALE);
        pushLayer(ctx, layers, &layers->glow);
    }
}

// The picture, one layer per eye, on a cylinder when it is curved. With the 3D
// switched off the left half alone is drawn, flat, and goes to both eyes as
// one layer. What was last drawn decides, not the switch, so a press that
// lands between two frames never shows an eye the draw has not caught up with.
static void addVideoLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    int viewCount = view->stereo && ctx->drawnEyes == 2 ? 2 : 1;
    for (int eye = 0; eye < viewCount; eye++) {
        XrSwapchainSubImage subImage;
        subImage.swapchain = ctx->swapchain;
        // The swap toggle reroutes which half each eye sees. Any stereo
        // inversion bug found later is then depth or warp, not routing
        int half = viewCount == 2 && view->eyeSwap ? (1 - eye) : eye;
        int x = view->stereo ? half * ctx->videoWidth : 0;
        int w = ctx->videoWidth;
        // Both eyes share one chain, side by side, and the compositor's filter
        // reads a texel past the rectangle it is given, which at the seam is
        // the other eye's picture. Both ends come in by a texel, so the two
        // eyes lose the same columns and the crop brings no disparity with it.
        // The flat picture keeps the same crop, so switching moves nothing.
        if (view->stereo && ctx->seamInset && w > 2 * SEAM_INSET_TEXELS) {
            x += SEAM_INSET_TEXELS;
            w -= 2 * SEAM_INSET_TEXELS;
        }
        subImage.imageRect.offset.x = x;
        subImage.imageRect.offset.y = 0;
        subImage.imageRect.extent.width = w;
        subImage.imageRect.extent.height = ctx->videoHeight;
        subImage.imageArrayIndex = 0;

        XrEyeVisibility visibility = viewCount == 1 ? XR_EYE_VISIBILITY_BOTH :
                (eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);

        if (view->curve > 0.01f && ctx->cylinderSupported) {
            XrCompositionLayerCylinderKHR* cyl = &layers->cylinder[eye];
            memset(cyl, 0, sizeof(*cyl));
            cyl->type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
            cyl->next = layers->settingsChain;
            // Radius runs from 4x distance (slightly curved) down to the
            // distance itself (wrapped around the viewer) as curvature rises
            float radius = ctx->screenRadius;
            cyl->eyeVisibility = visibility;
            cyl->subImage = subImage;
            cyl->space = view->space;
            // The layer pose is the axis, which sits a radius behind the
            // surface the placement tracks
            Vec3 axisLocal = { 0.0f, 0.0f, radius };
            cyl->pose = poseOffset(view->screenPose, axisLocal);
            cyl->radius = radius;
            cyl->centralAngle = view->screenWidth / radius;
            cyl->aspectRatio = 1.0f / view->aspect;
            pushLayer(ctx, layers, cyl);
        }
        else {
            XrCompositionLayerQuad* quad = &layers->video[eye];
            quadLayer(quad, layers->settingsChain, 0, ctx->swapchain, ctx->videoWidth,
                      ctx->videoHeight, view->space, view->screenPose, view->screenWidth,
                      view->screenHeight);
            quad->eyeVisibility = visibility;
            quad->subImage = subImage;
            pushLayer(ctx, layers, quad);
        }
    }
}

// The stats overlay in the corner of the picture
static void addOverlayLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // Stats sit in the top left corner of the screen, same space and
    // distance, both eyes, so they read at screen depth with no disparity.
    // Visibility is the gate rather than the content: switching them off
    // leaves the last text sitting in the swapchain, since nothing comes
    // along to overwrite it.
    if (ctx->overlayHasContent && ctx->overlayVisible
            && ctx->overlaySwapchain != XR_NULL_HANDLE) {
        float overlayW = view->screenWidth * 0.30f;
        float overlayH = overlayW * (float)OVERLAY_HEIGHT / (float)OVERLAY_WIDTH;
        float margin = view->screenWidth * 0.02f;

        // Pinned to the top left of the screen in the screen's own frame,
        // so it follows wherever the screen has been moved to
        Vec3 statsLocal = { -view->screenWidth * 0.5f + overlayW * 0.5f + margin,
                            view->screenHeight * 0.5f - overlayH * 0.5f - margin,
                            // A little in front so the two never z fight
                            0.01f };
        quadLayer(&layers->overlay, layers->settingsChain,
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, ctx->overlaySwapchain,
                  OVERLAY_WIDTH, OVERLAY_HEIGHT, view->space,
                  poseOffset(view->screenPose, statsLocal), overlayW, overlayH);
        pushLayer(ctx, layers, &layers->overlay);
    }
}

// The move bar or the resize corner, whichever the ray is over
static void addHandleLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // Move bar and resize corner, shown only while the ray is over them.
    // Both live in the screen's own frame, so they travel with it. The bar
    // never goes up in a room, where the wall holds the picture, though the
    // buttons beside it still come up on the same hover; the corners go up
    // in a room that lets its picture be resized, and nowhere else in one.
    int isBar = view->barArea && !view->roomOn;
    int isCorner = !view->barArea && ctx->hoverKind == HOVER_CORNER && cornerSide(ctx) > 0.0f;
    if (ctx->handleArtReady && (isBar || isCorner)) {
        Vec3 local;
        float sizeW, sizeH;
        float roll = 0.0f;

        if (isBar) {
            sizeW = view->screenWidth * BAR_WIDTH_FRAC;
            sizeH = view->screenWidth * BAR_HEIGHT_FRAC;
            local.x = 0.0f;
            local.y = -(view->screenHeight * 0.5f + view->screenWidth * BAR_GAP_FRAC
                        + sizeH * 0.5f);
        }
        else {
            sizeW = sizeH = cornerSide(ctx);
            int right = ctx->hoverCorner == 1 || ctx->hoverCorner == 3;
            int bottom = ctx->hoverCorner >= 2;
            // Half a bracket outside the corner in both axes, so its inner
            // tip touches the corner and none of it covers the picture
            local.x = (right ? 0.5f : -0.5f) * (view->screenWidth + sizeW);
            local.y = (bottom ? -0.5f : 0.5f) * (view->screenHeight + sizeH);
            // The art is a top left bracket, so the other three are the
            // same picture rolled about the screen normal
            if (ctx->hoverCorner == 1) roll = -1.5707963f;
            else if (ctx->hoverCorner == 2) roll = 1.5707963f;
            else if (ctx->hoverCorner == 3) roll = 3.1415927f;
        }
        // Just off the surface so it never z fights the picture
        local.z = 0.005f;
        // On a curved screen the corners come a long way toward the
        // viewer, so both the place and the facing follow the surface
        float yaw = 0.0f;
        curveLocal(&local, ctx->screenRadius, view->screenCurved, &yaw);

        XrQuaternionf rollQ = { 0.0f, 0.0f, sinf(roll * 0.5f), cosf(roll * 0.5f) };
        Vec3 yawAxis = { 0.0f, 1.0f, 0.0f };
        XrQuaternionf turnQ = quatMul(axisAngleQuat(yawAxis, yaw), rollQ);

        quadLayer(&layers->handle, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  isBar ? ctx->barSwapchain : ctx->cornerSwapchain,
                  isBar ? BAR_TEX_W : CORNER_TEX_W, isBar ? BAR_TEX_H : CORNER_TEX_H,
                  view->space, poseOffset(view->screenPose, local), sizeW, sizeH);
        layers->handle.pose.orientation = quatNorm(quatMul(view->screenPose.orientation, turnQ));
        pushLayer(ctx, layers, &layers->handle);
    }
}

// One of the buttons beside the move bar, wherever its placement puts it. On
// the furniture's frame, which in a room is the stand in rather than the wall.
static void addBarButton(XrCtx* ctx, const FrameView* view, FrameLayers* layers,
                         XrCompositionLayerQuad* slot, XrSwapchain chain,
                         void (*placement)(XrCtx*, float, Vec3*, float*), int hot) {
    Vec3 local;
    float side;
    placement(ctx, furnitureHeight(ctx), &local, &side);
    // Grows a little when the ray is on it, which is the only feedback
    // a quad layer can give without a second texture
    float scale = hot ? 1.18f : 1.0f;
    quadLayer(slot, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, chain,
              BUTTON_TEX, BUTTON_TEX, view->space, poseOffset(furniturePose(ctx), local),
              side * scale, side * scale);
    pushLayer(ctx, layers, slot);
}

// The environment, settings, keyboard and exit buttons beside the move bar
static void addBarButtonLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The button that opens the environment grid, left of the move bar.
    // Stays up while the grid is open so it reads as the thing that
    // opened it.
    if (ctx->envButtonReady && (view->barArea || ctx->pickerOpen)) {
        addBarButton(ctx, view, layers, &layers->envButton, ctx->envButtonSwapchain,
                     envButtonPlacement, ctx->envButtonHot);
    }

    // The cog that opens the settings panel, right of the move bar. Same
    // rules as the environment button on the other side.
    if (ctx->cogButtonReady && (view->barArea || ctx->cogOpen)) {
        addBarButton(ctx, view, layers, &layers->cogButton, ctx->cogButtonSwapchain,
                     cogButtonPlacement, ctx->cogButtonHot || ctx->cogOpen);
    }

    // The keyboard button, one place further out. Unlike the cog it goes
    // away while its panel is up, since the panel covers the bar anyway and
    // the hide key is what puts it away.
    if (ctx->kbButtonReady && view->barArea) {
        addBarButton(ctx, view, layers, &layers->kbButton, ctx->kbButtonSwapchain,
                     kbButtonPlacement, ctx->kbButtonHot);
    }

    // The button that ends the stream, furthest out on the left. Stays up
    // while its prompt is open, like the environment button does, so it
    // reads as the thing that asked the question.
    if (ctx->exitButtonReady && (view->barArea || ctx->exitConfirmOpen)) {
        addBarButton(ctx, view, layers, &layers->exitButton, ctx->exitButtonSwapchain,
                     exitButtonPlacement, ctx->exitButtonHot || ctx->exitConfirmOpen);
    }

    // The 3D switch, furthest out on the right, showing which way it is set.
    // Never made in a session without stereo, so never ready in one.
    if (ctx->stereoButtonReady && view->barArea) {
        addBarButton(ctx, view, layers, &layers->stereoButton,
                     ctx->stereoButtonSwapchains[ctx->stereoLive ? 1 : 0],
                     stereoButtonPlacement, ctx->stereoButtonHot);
    }
}

// The prompt the exit button opens
static void addExitPromptLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The prompt itself, on the pose frozen when it opened. Which sheet is
    // up is which of its buttons the ray is on, so lighting one costs a
    // handle rather than an upload. Sharpened like the grid, since what it
    // carries is text.
    if (ctx->exitConfirmOpen && ctx->exitPromptReady[ctx->exitHoverZone]) {
        quadLayer(&layers->exitPrompt, layers->settingsChain,
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->exitPromptSwapchains[ctx->exitHoverZone], EXIT_TEX_W, EXIT_TEX_H,
                  view->space, ctx->exitPose, ctx->exitW, ctx->exitH);
        pushLayer(ctx, layers, &layers->exitPrompt);
    }
}

// The padlock on the left edge
static void addLockLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The padlock. Comes and goes like the rest of the furniture rather
    // than sitting there permanently, so it costs nothing to look at while
    // playing. Reaching for the bar shows it too, since that is where
    // people go looking when they want to change something, and so does the
    // ring finger gesture turning it, for a moment. A setting can hide it
    // altogether, which leaves the gesture as the way to the lock.
    int flashing = ctx->lockFlashNs != 0 && nowNs() - ctx->lockFlashNs < LOCK_FLASH_NS;
    if (ctx->handsEnabled && ctx->lockIconShown && ctx->lockArtReady
            && (ctx->hoverKind == HOVER_LOCK || view->barArea || flashing)) {
        Vec3 local;
        float side;
        float lockYaw = 0.0f;
        lockButtonPlacement(ctx, &local, &side);
        // Hangs off the left edge, which on a curved screen is well in
        // front of the flat plane the placement is measured in. A room's
        // picture is never curved, and its furniture is on the stand in.
        curveLocal(&local, ctx->screenRadius, view->screenCurved, &lockYaw);
        Vec3 yawAxis = { 0.0f, 1.0f, 0.0f };
        float lockScale = ctx->lockHot ? 1.18f : 1.0f;
        XrPosef frame = furniturePose(ctx);

        quadLayer(&layers->lock, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->handsLocked ? ctx->lockSwapchain : ctx->unlockSwapchain,
                  LOCK_TEX, LOCK_TEX, view->space, poseOffset(frame, local),
                  side * lockScale, side * lockScale);
        layers->lock.pose.orientation = quatNorm(quatMul(frame.orientation,
                                                         axisAngleQuat(yawAxis, lockYaw)));
        pushLayer(ctx, layers, &layers->lock);
    }
}

// The environment grid and the rings on its hovered and chosen cells
static void addPickerLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The environment grid, floating in front of the screen, with the
    // hovered and the chosen cell ringed
    if (ctx->pickerOpen && ctx->pickerReady) {
        float pickW, pickH;
        XrPosef pickPose = pickerPose(ctx, &pickW, &pickH);

        quadLayer(&layers->picker, layers->settingsChain,
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, ctx->pickerSwapchain,
                  PICKER_TEX_W, PICKER_TEX_H, view->space, pickPose, pickW, pickH);
        pushLayer(ctx, layers, &layers->picker);

        if (ctx->outlineReady) {
            float cellW = pickW / (float)PICKER_COLS;
            float cellH = pickH * (float)PICKER_CELL_PX / (float)PICKER_TEX_H;
            // Hover rings the cell, the choice sits inside it, so both
            // read at once when the ray is over what is already selected
            int marks[2] = { ctx->pickerHover, ctx->pickerChoice };
            float scales[2] = { 1.0f, 0.84f };

            for (int m = 0; m < 2; m++) {
                int cell = marks[m];
                // A blank tile is not a cell, so nothing is ringed on one
                if (cell < 0 || cell >= ctx->pickerCells) {
                    continue;
                }
                int col = cell % PICKER_COLS;
                int row = cell / PICKER_COLS;
                // Down the texture past this band's header to the middle
                // of its row of cells
                float centreV = (row * PICKER_BAND_PX + PICKER_HEADER_PX
                                 + PICKER_CELL_PX * 0.5f) / (float)PICKER_TEX_H;
                Vec3 local;
                local.x = ((col + 0.5f) / PICKER_COLS - 0.5f) * pickW;
                local.y = (0.5f - centreV) * pickH;
                local.z = 0.004f;

                XrCompositionLayerQuad* mark = &layers->outline[m];
                quadLayer(mark, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                          ctx->outlineSwapchain, OUTLINE_TEX, OUTLINE_TEX, view->space,
                          poseOffset(pickPose, local), cellW * scales[m], cellH * scales[m]);
                pushLayer(ctx, layers, mark);
            }
        }
    }
}

// A ring over one cell of a row on the settings panel, the same trick the
// picker uses to mark cells without an upload
static void addCogRing(XrCtx* ctx, const FrameView* view, FrameLayers* layers,
                       XrCompositionLayerQuad* mark, int row, int cell, int cells, float scale) {
    float span = (COG_TRACK_R - COG_TRACK_L) / cells;
    Vec3 local;
    local.x = (COG_TRACK_L + (cell + 0.5f) * span - 0.5f) * ctx->cogW;
    local.y = (0.5f - (COG_ROW_V0 + row * COG_ROW_STEP)) * ctx->cogH;
    local.z = 0.004f;
    quadLayer(mark, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              ctx->outlineSwapchain, OUTLINE_TEX, OUTLINE_TEX, view->space,
              poseOffset(ctx->cogPose, local), span * ctx->cogW * scale,
              2.0f * COG_CELL_HALF * ctx->cogH * scale);
    pushLayer(ctx, layers, mark);
}

// The settings panel, the rings on its rows of cells, the thumbs on its
// sliders, and on the Room tab the percents beside them
static void addCogLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The settings panel, at the pose it was opened with. The tab is a
    // choice of swapchain, all were filled at startup, and a room has its
    // own sheets. Sharpened: it carries text.
    int art = cogArt(ctx);
    int face = cogFace(ctx);
    if (ctx->cogOpen && ctx->cogPanelReady[art]) {
        quadLayer(&layers->cogPanel, layers->settingsChain,
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->cogPanelSwapchains[art], COG_TEX_W, COG_TEX_H, view->space,
                  ctx->cogPose, ctx->cogW, ctx->cogH);
        pushLayer(ctx, layers, &layers->cogPanel);

        // The cells are drawn into the texture, so what is chosen and what is
        // under the ray are rings over them. One per row for the choice, then
        // a wider one for the hover.
        if (face == COG_TAB_DISPLAY && ctx->outlineReady) {
            for (int m = 0; m <= COG_OPTION_COUNT; m++) {
                int hoverMark = m == COG_OPTION_COUNT;
                int option = hoverMark ? ctx->cogHoverSlider : m;
                int cell = hoverMark ? ctx->cogHoverCell
                        : cogOptionValue(ctx, m, view->headLocked);
                if (option < 0 || option >= COG_OPTION_COUNT || cell < 0) {
                    continue;
                }
                addCogRing(ctx, view, layers, &layers->cogMark[m], option, cell,
                           cogOptionCells(option), hoverMark ? 1.12f : 1.0f);
            }
        }
        else if ((face == COG_FACE_ROOM || face == COG_TAB_3D) && ctx->outlineReady) {
            // The Room and 3D tabs mix cells with tracks. Each live row of
            // cells rings the one in force, where there is one: the presets
            // ring none once the depth track is dragged off all three.
            int marks = 0;
            int rowCount = cogTabRowCount(face);
            for (int row = 0; row < rowCount && marks < COG_OPTION_COUNT; row++) {
                if (cogRowIsTrack(face, row) || !cogRowLive(ctx, face, row)) {
                    continue;
                }
                int cell = cogCellInForce(ctx, face, row);
                if (cell >= 0) {
                    addCogRing(ctx, view, layers, &layers->cogMark[marks++], row, cell,
                               cogRowCells(face, row), 1.0f);
                }
            }
            int hoverRow = ctx->cogHoverSlider;
            if (hoverRow >= 0 && !cogRowIsTrack(face, hoverRow) && ctx->cogHoverCell >= 0) {
                addCogRing(ctx, view, layers, &layers->cogMark[COG_OPTION_COUNT], hoverRow,
                           ctx->cogHoverCell, cogRowCells(face, hoverRow), 1.12f);
            }
        }

        // The Room tab's percents, once the strip says what the rows do now.
        // A strip still showing another room's values, or a value a drag has
        // just moved past, stays down until Java has drawn it again.
        if (face == COG_FACE_ROOM && ctx->cogReadoutReady) {
            int readouts[READOUT_VALUES];
            cogReadouts(ctx, readouts);
            if (memcmp(readouts, ctx->cogReadoutDrawn, sizeof(readouts)) == 0) {
                float stripW = (float)COG_READOUT_TEX_W / (float)COG_TEX_W;
                float stripH = (float)COG_READOUT_TEX_H / (float)COG_TEX_H;
                Vec3 local;
                local.x = (COG_READOUT_L + stripW * 0.5f - 0.5f) * ctx->cogW;
                local.y = (0.5f - (COG_READOUT_T + stripH * 0.5f)) * ctx->cogH;
                local.z = 0.003f;
                quadLayer(&layers->cogReadout, layers->settingsChain,
                          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                          ctx->cogReadoutSwapchain, COG_READOUT_TEX_W, COG_READOUT_TEX_H,
                          view->space, poseOffset(ctx->cogPose, local), stripW * ctx->cogW,
                          stripH * ctx->cogH);
                pushLayer(ctx, layers, &layers->cogReadout);
            }
        }

        if (ctx->cogThumbReady) {
            float thumbSize = ctx->cogH * 0.085f;
            int rowCount = cogTabRowCount(face);
            for (int s = 0; s < rowCount; s++) {
                // No thumb on a row that cannot be dragged, or on a row of
                // cells, which has rings over it instead
                if (!cogRowLive(ctx, face, s) || !cogRowIsTrack(face, s)) {
                    continue;
                }
                float t = cogSliderValue(ctx, face, s);
                Vec3 local;
                local.x = (COG_TRACK_L + t * (COG_TRACK_R - COG_TRACK_L) - 0.5f) * ctx->cogW;
                local.y = (0.5f - (COG_ROW_V0 + s * COG_ROW_STEP)) * ctx->cogH;
                local.z = 0.004f;
                // Grows under the ray, the same feedback the buttons give
                float grow = (ctx->cogHoverSlider == s || ctx->cogDragSlider == s)
                        ? 1.25f : 1.0f;

                XrCompositionLayerQuad* thumb = &layers->cogThumb[s];
                quadLayer(thumb, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                          ctx->cogThumbSwapchain, COG_THUMB_TEX, COG_THUMB_TEX, view->space,
                          poseOffset(ctx->cogPose, local), thumbSize * grow, thumbSize * grow);
                pushLayer(ctx, layers, thumb);
            }
        }
    }
}

// The keyboard and the ring on the key under the ray
static void addKeyboardLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The keyboard, at the pose it was opened with, with the key under the
    // ray ringed. Sharpened, since it is all text. It stands down while a
    // modal is up rather than stacking under one: the two together would
    // crowd the runtime's layer ceiling, and the modal has the ray anyway.
    if (ctx->kbOpen && !ctx->pickerOpen && !ctx->cogOpen && !ctx->exitConfirmOpen
            && ctx->kbPanelReady[ctx->kbState]) {
        quadLayer(&layers->kbPanel, layers->settingsChain,
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->kbPanelSwapchains[ctx->kbState], KB_TEX_W, KB_TEX_H, view->space,
                  ctx->kbPose, ctx->kbW, ctx->kbH);
        pushLayer(ctx, layers, &layers->kbPanel);

        if (ctx->outlineReady && ctx->kbHoverKey >= 0
                && ctx->kbHoverKey < ctx->kbKeyCount) {
            const float* r = &ctx->kbKeyRects[ctx->kbHoverKey * 4];
            Vec3 local;
            local.x = ((r[0] + r[2]) * 0.5f - 0.5f) * ctx->kbW;
            local.y = (0.5f - (r[1] + r[3]) * 0.5f) * ctx->kbH;
            local.z = 0.004f;
            // Swells while the trigger is held, which is the only press
            // feedback a quad layer can give
            float grow = ctx->kbKeyDown ? 1.12f : 1.0f;

            quadLayer(&layers->kbMark, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->outlineSwapchain, OUTLINE_TEX, OUTLINE_TEX, view->space,
                      poseOffset(ctx->kbPose, local), (r[2] - r[0]) * ctx->kbW * grow,
                      (r[3] - r[1]) * ctx->kbH * grow);
            pushLayer(ctx, layers, &layers->kbMark);
        }
    }
}

// The laser and the cursor at the end of it
static void addPointerLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // Laser and cursor, submitted last so they sit over the picture. Two
    // quad layers, so this costs no drawing at all: the art was uploaded
    // once and the compositor places it from these poses.
    if (ctx->beamVisible && !ctx->beamGaze && ctx->pointerArtReady) {
        Vec3 start = { ctx->beamStart.x, ctx->beamStart.y, ctx->beamStart.z };
        Vec3 end = { ctx->beamEnd.x, ctx->beamEnd.y, ctx->beamEnd.z };
        Vec3 head = { ctx->headPos.x, ctx->headPos.y, ctx->headPos.z };
        Vec3 along = vecSub(end, start);
        float length = sqrtf(along.x * along.x + along.y * along.y + along.z * along.z);

        Vec3 mid = { (start.x + end.x) * 0.5f, (start.y + end.y) * 0.5f,
                     (start.z + end.z) * 0.5f };
        Vec3 beamY = vecNorm(along);
        Vec3 toHead = vecNorm(vecSub(head, mid));
        Vec3 beamX = vecCross(beamY, toHead);
        float sideLen = sqrtf(beamX.x * beamX.x + beamX.y * beamX.y + beamX.z * beamX.z);

        // A quad has one orientation, so the ribbon is turned to face the
        // head. Aimed nearly along the line of sight there is no such
        // direction to find, and any perpendicular will do: the ribbon is
        // edge on either way. This used to give up instead, which is why
        // the ray vanished over the lower half of the screen.
        if (sideLen < 0.15f) {
            Vec3 up = { 0.0f, 1.0f, 0.0f };
            beamX = vecCross(beamY, up);
            sideLen = sqrtf(beamX.x * beamX.x + beamX.y * beamX.y + beamX.z * beamX.z);
            if (sideLen < 0.15f) {
                Vec3 side = { 1.0f, 0.0f, 0.0f };
                beamX = vecCross(beamY, side);
            }
        }

        if (length > 0.10f) {
            beamX = vecNorm(beamX);
            Vec3 beamZ = vecCross(beamX, beamY);
            XrPosef beamPose = { quatFromBasis(beamX, beamY, beamZ),
                                 { mid.x, mid.y, mid.z } };

            quadLayer(&layers->beam, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->pointerSwapchain, PTR_TEX_W, PTR_BEAM_H, view->space, beamPose,
                      ctx->beamWidth, length);
            pushLayer(ctx, layers, &layers->beam);
        }

        // Cursor sits just off the surface facing the viewer, which works
        // on the cylinder as well as the flat screen. Independent of the
        // ribbon: a gaze has a cursor and no ray, a ray aimed at nothing
        // has no cursor.
        if (!ctx->beamFree) {
            Vec3 dotZ = vecNorm(vecSub(head, end));
            Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
            Vec3 dotX = vecNorm(vecCross(worldUp, dotZ));
            Vec3 dotY = vecCross(dotZ, dotX);
            XrPosef dotPose = { quatFromBasis(dotX, dotY, dotZ),
                                { end.x + dotZ.x * 0.012f, end.y + dotZ.y * 0.012f,
                                  end.z + dotZ.z * 0.012f } };

            quadLayer(&layers->dot, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->pointerSwapchain, PTR_TEX_W, PTR_DOT_H, view->space, dotPose,
                      0.022f, 0.022f);
            // The dot is the strip under the beam in the swapchain they share
            layers->dot.subImage.imageRect.offset.y = PTR_BEAM_H;
            pushLayer(ctx, layers, &layers->dot);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeEndFrame(JNIEnv* env, jobject thiz, jlong handle,
                                                           jboolean newFrame, jfloatArray texMatrixArr,
                                                           jfloat distance, jfloat quadWidth,
                                                           jfloat curvature, jboolean headLocked,
                                                           jfloat separation, jboolean eyeSwap,
                                                           jboolean passthrough) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }

    ctx->passthrough = passthrough;
    ctx->prefCurvature = curvature;
    pollCaptureRequest(ctx);
    propFlag(PROP_PASSTHROUGH, &ctx->passthrough);
    // The panel first, then the debug property over the top of it, so a blind
    // A/B still wins whatever the panel was left on
    if (ctx->panelSeparation >= 0.0f) {
        separation = ctx->panelSeparation;
    }
    if (ctx->separationOverride >= 0.0f) {
        separation = ctx->separationOverride;
    }
    // What is really in force, for the panel's thumb to read back
    ctx->separationCurrent = separation;
    if (ctx->distanceOverride > 0.0f) {
        distance = ctx->distanceOverride;
    }
    if (ctx->screenOverride > 0.0f) {
        quadWidth = ctx->screenOverride;
    }

    // Back on and waiting flat: a map made since the switch has landed, or
    // the wait is over. Drawn again at once, like the switch itself, so a
    // picture standing still takes its depth without waiting for a new frame.
    if (ctx->stereoWaiting
            && (atomic_load_explicit(&ctx->depthStagedIndex, memory_order_acquire)
                    != ctx->stereoWaitIndex
                || nowNs() - ctx->stereoWaitNs > STEREO_WAIT_NS)) {
        ctx->stereoWaiting = 0;
        ctx->warpRedraw = 1;
        LOGI("3d warping again after %.0f ms", (nowNs() - ctx->stereoWaitNs) / 1e6);
    }

    // A switch of the 3D redraws the frame already latched, since the decoder
    // sends nothing while the picture stands still
    int redraw = ctx->warpRedraw && ctx->everRendered;
    if ((newFrame || redraw) && ctx->shouldRender) {
        long startNs = nowNs();

        float texMatrix[16];
        (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);
        renderVideoFrame(ctx, texMatrix, separation);

        long elapsed = nowNs() - startNs;
        ctx->statFrames++;
        ctx->statTotalNs += elapsed;
        if (elapsed > ctx->statMaxNs) ctx->statMaxNs = elapsed;
        logWarpStats(ctx);
    }

    FrameView view;
    view.aspect = (float)ctx->videoHeight / (float)ctx->videoWidth;
    view.stereo = ctx->stereoMode != DEPTH_MODE_OFF;
    int roomStyle = roomEffective(ctx);
    view.roomOn = roomStyle > 0;
    view.headLocked = headLocked;
    view.eyeSwap = eyeSwap;
    // A room hangs the picture on a wall, and a wall does not follow the head
    // about however the preference is set
    view.space = (headLocked && !view.roomOn) ? ctx->viewSpace : ctx->localSpace;

    if (!ctx->pointerArtReady && ctx->pointerSwapchain != XR_NULL_HANDLE && ctx->shouldRender) {
        uploadPointerArt(ctx);
    }

    // The panel's curve, if it has one, so a reseed keeps the curve in force
    // rather than snapping back to the preference
    view.curve = effectiveCurvature(ctx);
    int reseeded = updatePlacement(ctx, distance, quadWidth, view.curve);
    // Then the wall has the last word on where the picture is, and a picture
    // flat on a wall is flat
    applyRoomPlacement(ctx, roomStyle, view.aspect, reseeded);
    if (view.roomOn) {
        view.curve = 0.0f;
    }
    view.screenPose = ctx->screenPose;
    view.screenWidth = ctx->screenWidth;
    view.screenHeight = view.screenWidth * view.aspect;
    // The same test the picture's own layer makes below, so the furniture that
    // is pinned to the picture sits on whichever surface actually goes up
    view.screenCurved = view.curve > 0.01f && ctx->cylinderSupported;
    view.barArea = !ctx->pickerOpen && !ctx->cogOpen && !ctx->kbOpen
            && !ctx->exitConfirmOpen
            && (ctx->hoverKind == HOVER_BAR || ctx->hoverKind == HOVER_ENVBUTTON
                || ctx->hoverKind == HOVER_COGBUTTON
                || ctx->hoverKind == HOVER_KBBUTTON
                || ctx->hoverKind == HOVER_EXITBUTTON
                || ctx->hoverKind == HOVER_STEREOBUTTON);

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = ctx->predictedDisplayTime;
    // Some runtimes only bring the cameras up on a change of blend mode seen
    // after the session is focused, and asking for alpha blend from the very
    // first frame leaves them off for the whole session. Submitting the first
    // focused frame opaque gives every runtime the transition it wants. Later
    // switches from the picker are long past this point.
    int wantPassthrough = ctx->passthrough && ctx->alphaBlendSupported;
    endInfo.environmentBlendMode = (wantPassthrough && ctx->focusedFrames > 0)
            ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (ctx->sessionState == XR_SESSION_STATE_FOCUSED) {
        ctx->focusedFrames++;
        if (wantPassthrough && ctx->focusedFrames == 1) {
            LOGI("passthrough blend enabled after first focused frame");
            LOGEV("passthrough blend enabled after first focused frame");
        }
    }

    FrameLayers layers;
    layers.count = 0;
    setLayerSettings(ctx, &layers);

    addRoomLayer(ctx, &view, &layers);
    addGlowLayer(ctx, &view, &layers);
    if (ctx->everRendered && ctx->shouldRender) {
        addVideoLayers(ctx, &view, &layers);
        addOverlayLayer(ctx, &view, &layers);
        addHandleLayer(ctx, &view, &layers);
        addBarButtonLayers(ctx, &view, &layers);
        addExitPromptLayer(ctx, &view, &layers);
        addLockLayer(ctx, &view, &layers);
        addPickerLayers(ctx, &view, &layers);
        addCogLayers(ctx, &view, &layers);
        addKeyboardLayers(ctx, &view, &layers);
        addPointerLayers(ctx, &view, &layers);
    }

    // The display tab at its fullest, with a room, the glow, the stats and
    // the ray all up, is one layer past the Pico's sixteen, and a frame over
    // the limit is refused whole. Its hover ring is what goes, and the Room
    // and 3D tabs' are the same slot: the cursor already shows where the ray
    // is. The padlock shown for a moment after the ring finger gesture can
    // land on top of that, and goes next.
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.cogMark[COG_OPTION_COUNT]);
    }
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.lock);
    }

    // Said once and only once, since a frame that crowds the limit is usually
    // every frame after it. Nothing else is dropped: a missing layer is a
    // silent bug, where the count in the log points straight at the culprit.
    if (layers.count >= (uint32_t)ctx->maxLayerCount && !ctx->layerLimitWarned) {
        ctx->layerLimitWarned = 1;
        LOGW("submitted %u composition layers against a limit of %d",
             layers.count, ctx->maxLayerCount);
    }

    endInfo.layerCount = layers.count;
    endInfo.layers = layers.order;
    checkXr(xrEndFrame(ctx->session, &endInfo), "xrEndFrame");
    displayFrameEnded(ctx);
}
