// The end of the frame: every composition layer the session shows,
// assembled in draw order and handed to the compositor.
#include "xr_renderer.h"

// What a frame carries at its fullest, counted against the Pico's sixteen, the
// lowest limit of the headsets here (the Quests report 32, but the Quest 3
// refuses more than 16 all the same, so 16 is used everywhere). The settings
// panel is modal, and the frame a modal opens sheds the bar furniture, so the
// two never land in one frame together. With the panel open the clock strip
// over it is always up, and a step button under the ray takes the hover ring a
// cell would. The toast can land on any of these, one layer more.
//   Screen tab: the glow, both eyes, the stats, the cog button, the panel,
//     the clock, six thumbs, the hover ring, ray and cursor: 16. With the
//     screen locked to the head head aim's two rows are live, two thumbs more
//     and the strip of values beside them: 19, three past.
//   Room tab, in a room where it takes the screen tab's place: the room, the
//     glow, both eyes, the stats, the cog button, the panel, the clock, the
//     strip of percents, two rings on its rows of cells and the hover ring,
//     three thumbs, ray and cursor: 17, one past.
//   Display tab in a room: its choices are one strip rather than a ring a
//     row, so the room, the glow, both eyes, the stats, the cog button, the
//     panel, the clock, the marks, the hover ring, the glow level's thumb,
//     ray and cursor: 14.
//   3D tab in a room: the rings on its preset and its switch, the hover ring
//     and two thumbs over the same: 16.
//   Picture tab in a room: the room, the glow, both eyes, the stats, the cog
//     button, the panel, the clock, the strip of values, four thumbs, the
//     hover ring, ray and cursor: 16.
//   About tab in a room: no rows, so the room, the glow, both eyes, the
//     stats, the cog button, the panel, the clock, the ring on its button,
//     ray and cursor: 11.
//   The bar: the pill, all seven buttons (exit, gamepad, environment, cog,
//     keyboard, ray, 3D) and the padlock over the glow, both eyes, the stats,
//     ray and cursor: 15, and 15 in a room, where the pill gives way to the
//     room's own layer. 16 with the toast. With the ray switched off it is
//     one fewer, since the bar is not a panel and brings no beam back. With
//     the screen locked to the head, outside a room, head aim's button is an
//     eighth: 16, 17 with the toast, one past.
//   The controller models are a projection layer of their own, over the
//     picture and the panels and under the beam, one more on any of these
//     while a model shows: the bar to 16, 17 with the toast (17 and 18 with
//     head aim's button), the Display tab to 15 and the About tab to 12, and
//     the screen, 3D and Picture tabs to 17 and the Room tab to 18, which go
//     over, and the head locked screen tab to 20.
// So a frame over the runtime's limit sheds, in this order, the toast, the
// controller models, the hover ring, the cog button and the clock strip (see
// nativeEndFrame), which brings every case above to 16 or under with the
// toast up. The head locked screen tab at its fullest, models and toast
// included, is 21 and comes down to exactly 16; the head locked bar at its
// fullest is 18 and comes down to 16.
// Switching the 3D off only ever takes a layer away: both eyes are then one.
// The keyboard sheds the bar furniture and adds only its panel and one ring,
// so it comes to 9. The report sheet puts the settings panel away and brings
// the keyboard up under it, so it is the keyboard's 9 and its own sheet: 10.
// Each is one more with the controller models up.
// The exit prompt sheds the furniture too and adds its own sheet and the
// button that opened it, so it comes to less again. A panel fading out
// keeps the bar furniture down until it has gone, and one opening cuts any
// other's fade short, so a fade never stacks two of these. The launch splash
// is two layers and all the frame carries while it is fully up; as it fades
// the room, the glow, the eyes and the stats come up under it, five more,
// with no furniture and no toast, since neither input nor notices move until
// it has gone.
// Sized well past all that anyway: an overflow here is a smashed stack, and
// the margin costs a few pointers.
#define FRAME_MAX_LAYERS 24

// The display tab's marks go back to Java one value a row
_Static_assert(MARK_VALUES == COG_OPTION_COUNT, "a mark for every display tab row");

// Every composition layer a frame can carry, on nativeEndFrame's stack for as
// long as xrEndFrame needs them
typedef struct {
    XrCompositionLayerProjection room;
    XrCompositionLayerProjectionView roomViews[ROOM_EYES];
    XrCompositionLayerQuad glow;
    // The glow's shape round a curved picture, in the quad's place
    XrCompositionLayerCylinderKHR glowCylinder;
    XrCompositionLayerQuad video[2];
    XrCompositionLayerCylinderKHR cylinder[2];
    XrCompositionLayerQuad overlay;
    XrCompositionLayerQuad handle;
    XrCompositionLayerQuad envButton;
    XrCompositionLayerQuad cogButton;
    XrCompositionLayerQuad kbButton;
    XrCompositionLayerQuad exitButton;
    XrCompositionLayerQuad stereoButton;
    XrCompositionLayerQuad rayButton;
    XrCompositionLayerQuad aimButton;
    XrCompositionLayerQuad padButton;
    XrCompositionLayerQuad exitPrompt;
    XrCompositionLayerQuad report;
    XrCompositionLayerQuad lock;
    XrCompositionLayerQuad picker;
    XrCompositionLayerQuad outline[2];
    XrCompositionLayerQuad cogPanel;
    // One per option row for what is chosen, plus one for the hover
    XrCompositionLayerQuad cogMark[COG_OPTION_COUNT + 1];
    XrCompositionLayerQuad cogReadout;
    XrCompositionLayerQuad cogMarks;
    XrCompositionLayerQuad cogClock;
    // One per row of whichever tab has the most. The display tab's glow level
    // track is its last row, so it is the one that sets the size.
    XrCompositionLayerQuad cogThumb[COG_DISPLAY_SLIDER_ROW + 1 > COG_SCREEN_ROW_COUNT
                                    ? COG_DISPLAY_SLIDER_ROW + 1 : COG_SCREEN_ROW_COUNT];
    XrCompositionLayerQuad kbPanel;
    XrCompositionLayerQuad kbMark;
    XrCompositionLayerQuad beam;
    XrCompositionLayerQuad dot;
    // The controller models, over the picture and the panels and under the
    // beam and the dot
    XrCompositionLayerProjection models;
    XrCompositionLayerProjectionView modelViews[ROOM_EYES];
    XrCompositionLayerQuad toast;
    XrCompositionLayerQuad splashBlack;
    XrCompositionLayerQuad splashSheet;
    XrCompositionLayerSettingsFB settings;
    // NULL when sharpening and supersampling are both off or unsupported,
    // which leaves every chain untouched
    const void* settingsChain;
    // The colour scale on whatever is part way through a fade, one per thing
    // that fades, ahead of the settings chain on the layers that carry it and
    // on its own on the rest
    XrCompositionLayerColorScaleBiasKHR fades[FADE_SLOTS][2];
    const XrCompositionLayerBaseHeader* order[FRAME_MAX_LAYERS];
    uint32_t count;
} FrameLayers;

// What a layer chains on as it fades: the settings chain on the layers that
// carry it, behind a colour scale while the fade is part way. At rest, or on a
// runtime without the extension, the chain is what it always was. Every
// channel is scaled, since the art's alpha is premultiplied.
static const void* fadeNext(XrCtx* ctx, FrameLayers* layers, int slot, int sharp, float level) {
    const void* next = sharp ? layers->settingsChain : NULL;
    if (!ctx->colorScaleSupported || level >= 1.0f) {
        return next;
    }
    XrCompositionLayerColorScaleBiasKHR* scale = &layers->fades[slot][sharp ? 1 : 0];
    memset(scale, 0, sizeof(*scale));
    scale->type = XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR;
    scale->next = next;
    scale->colorScale.r = level;
    scale->colorScale.g = level;
    scale->colorScale.b = level;
    scale->colorScale.a = level;
    return scale;
}

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
        char roomLine[128];
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
        // The controller models' pass the same way, once a display frame
        // while one shows
        if (ctx->modelGpuSamples > 0) {
            size_t used = strlen(roomLine);
            snprintf(roomLine + used, sizeof(roomLine) - used,
                     ", models avg %.2f ms (%ld of %ld)",
                     ctx->modelGpuTotalNs / (double)ctx->modelGpuSamples / 1e6,
                     ctx->modelGpuSamples, ctx->modelGpuSamples + ctx->modelGpuDropped);
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
        ctx->modelGpuTotalNs = 0;
        ctx->modelGpuSamples = 0;
        ctx->modelGpuDropped = 0;
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
        // A curved picture's sides come round toward the viewer and would
        // cover a flat glow's, leaving it only above and below, so the glow
        // curves with it: the same axis, a little inside the same radius
        GlowCylinder shape;
        if (view->screenCurved
                && glowCylinderFor(view->screenWidth, view->screenHeight, ctx->screenRadius,
                                   &shape)) {
            XrCompositionLayerCylinderKHR* cyl = &layers->glowCylinder;
            memset(cyl, 0, sizeof(*cyl));
            cyl->type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
            cyl->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            cyl->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cyl->subImage.swapchain = ctx->glowSwapchain;
            cyl->subImage.imageRect.offset.x = shape.rectX;
            cyl->subImage.imageRect.offset.y = 0;
            cyl->subImage.imageRect.extent.width = shape.rectWidth;
            cyl->subImage.imageRect.extent.height = GLOW_TEX;
            cyl->subImage.imageArrayIndex = 0;
            cyl->space = view->space;
            Vec3 axisLocal = { 0.0f, 0.0f, ctx->screenRadius };
            cyl->pose = poseOffset(view->screenPose, axisLocal);
            cyl->radius = shape.radius;
            cyl->centralAngle = shape.centralAngle;
            cyl->aspectRatio = shape.aspectRatio;
            pushLayer(ctx, layers, cyl);
            return;
        }
        // Local +z is toward the viewer, the side the cylinder puts its axis,
        // so this sits just proud of the picture. The layer order is what
        // keeps it under the picture, not the depth.
        Vec3 proudLocal = { 0.0f, 0.0f, GLOW_PROUD_M };
        quadLayer(&layers->glow, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->glowSwapchain, GLOW_TEX, GLOW_TEX, view->space,
                  poseOffset(view->screenPose, proudLocal),
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
                         void (*placement)(XrCtx*, float, Vec3*, float*), int hot,
                         const void* next) {
    Vec3 local;
    float side;
    placement(ctx, furnitureHeight(ctx), &local, &side);
    // Grows a little when the ray is on it, which is the only feedback
    // a quad layer can give without a second texture
    float scale = hot ? 1.18f : 1.0f;
    quadLayer(slot, next, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, chain,
              BUTTON_TEX, BUTTON_TEX, view->space, poseOffset(furniturePose(ctx), local),
              side * scale, side * scale);
    pushLayer(ctx, layers, slot);
}

// Whether a button that stays up with its panel is up, and what it chains on:
// nothing extra while the bar or the panel holds it up, and the panel's fade
// while the panel is on its way out, so the two go together
static int panelButton(XrCtx* ctx, const FrameView* view, FrameLayers* layers, int panel,
                       int open, const void** next) {
    float level = ctx->panelFades[panel].level;
    *next = NULL;
    if (view->barArea || open) {
        return 1;
    }
    if (level > 0.0f) {
        *next = fadeNext(ctx, layers, panel, 0, level);
        return 1;
    }
    return 0;
}

// The environment, settings, keyboard and exit buttons beside the move bar
static void addBarButtonLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    const void* next = NULL;
    // The button that opens the environment grid, left of the move bar.
    // Stays up while the grid is open so it reads as the thing that
    // opened it.
    if (ctx->envButtonReady
            && panelButton(ctx, view, layers, FADE_PICKER, ctx->pickerOpen, &next)) {
        addBarButton(ctx, view, layers, &layers->envButton, ctx->envButtonSwapchain,
                     envButtonPlacement, ctx->envButtonHot, next);
    }

    // The cog that opens the settings panel, right of the move bar. Same
    // rules as the environment button on the other side.
    if (ctx->cogButtonReady && panelButton(ctx, view, layers, FADE_COG, ctx->cogOpen, &next)) {
        addBarButton(ctx, view, layers, &layers->cogButton, ctx->cogButtonSwapchain,
                     cogButtonPlacement, ctx->cogButtonHot || ctx->cogOpen, next);
    }

    // The keyboard button, one place further out. Unlike the cog it goes
    // away while its panel is up, since the panel covers the bar anyway and
    // the hide key is what puts it away.
    if (ctx->kbButtonReady && view->barArea) {
        addBarButton(ctx, view, layers, &layers->kbButton, ctx->kbButtonSwapchain,
                     kbButtonPlacement, ctx->kbButtonHot, NULL);
    }

    // The button that ends the stream, furthest out on the left. Stays up
    // while its prompt is open, like the environment button does, so it
    // reads as the thing that asked the question.
    if (ctx->exitButtonReady
            && panelButton(ctx, view, layers, FADE_EXIT, ctx->exitConfirmOpen, &next)) {
        addBarButton(ctx, view, layers, &layers->exitButton, ctx->exitButtonSwapchain,
                     exitButtonPlacement, ctx->exitButtonHot || ctx->exitConfirmOpen, next);
    }

    // Gamepad mode's switch, past the exit button, lit while the controllers
    // are the pad
    if (ctx->padButtonReady && view->barArea) {
        addBarButton(ctx, view, layers, &layers->padButton,
                     ctx->padButtonSwapchains[ctx->padMode ? 1 : 0],
                     padButtonPlacement, ctx->padButtonHot, NULL);
    }

    // Head aim's switch, past that, showing which way it is set. Only where
    // head aim can act, so it is never a button that does nothing.
    if (ctx->aimButtonReady && view->barArea && headAimCanAct(ctx)) {
        addBarButton(ctx, view, layers, &layers->aimButton,
                     ctx->aimButtonSwapchains[headAimSwitchOn(ctx->headAimSetting,
                                                              ctx->headAimFlipped)],
                     aimButtonPlacement, ctx->aimButtonHot, NULL);
    }

    // The ray's switch, past the keyboard, showing which way it is set
    if (ctx->rayButtonReady && view->barArea) {
        addBarButton(ctx, view, layers, &layers->rayButton,
                     ctx->rayButtonSwapchains[raySwitchOn(ctx->raySetting, ctx->rayFlipped)],
                     rayButtonPlacement, ctx->rayButtonHot, NULL);
    }

    // The 3D switch, furthest out on the right, showing which way it is set.
    // Never made in a session without stereo, so never ready in one.
    if (ctx->stereoButtonReady && view->barArea) {
        addBarButton(ctx, view, layers, &layers->stereoButton,
                     ctx->stereoButtonSwapchains[ctx->stereoLive ? 1 : 0],
                     stereoButtonPlacement, ctx->stereoButtonHot, NULL);
    }
}

// The prompt the exit button opens
static void addExitPromptLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The prompt itself, on the pose frozen when it opened. Which sheet is
    // up is which of its buttons the ray is on, so lighting one costs a
    // handle rather than an upload. Sharpened like the grid, since what it
    // carries is text.
    float level = ctx->panelFades[FADE_EXIT].level;
    if (level > 0.0f && ctx->exitPromptReady[ctx->exitHoverZone]) {
        quadLayer(&layers->exitPrompt, fadeNext(ctx, layers, FADE_EXIT, 1, level),
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->exitPromptSwapchains[ctx->exitHoverZone], EXIT_TEX_W, EXIT_TEX_H,
                  view->space, ctx->exitPose, ctx->exitW, ctx->exitH);
        pushLayer(ctx, layers, &layers->exitPrompt);
    }
}

// The report sheet, on the pose frozen when it opened, once Java has drawn it
// for this opening. Sharpened, since it is all text.
static void addReportLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    float level = ctx->panelFades[FADE_REPORT].level;
    if (level > 0.0f && ctx->reportReady) {
        quadLayer(&layers->report, fadeNext(ctx, layers, FADE_REPORT, 1, level),
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->reportSwapchain, REPORT_TEX_W, REPORT_TEX_H, view->space,
                  ctx->reportPose, ctx->reportW, ctx->reportH);
        pushLayer(ctx, layers, &layers->report);
    }
}

// The padlock on the left edge
static void addLockLayer(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The padlock. Comes and goes like the rest of the furniture rather
    // than sitting there permanently, so it costs nothing to look at while
    // playing. Reaching for the bar shows it too, since that is where
    // people go looking when they want to change something. A setting can
    // hide it altogether, which leaves the gesture as the way to the lock,
    // and the toast says when that turns it.
    if (ctx->handsEnabled && ctx->lockIconShown && ctx->lockArtReady
            && (ctx->hoverKind == HOVER_LOCK || view->barArea)) {
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
    float level = ctx->panelFades[FADE_PICKER].level;
    if (level > 0.0f && ctx->pickerReady) {
        float pickW, pickH;
        XrPosef pickPose = pickerPose(ctx, &pickW, &pickH);

        quadLayer(&layers->picker, fadeNext(ctx, layers, FADE_PICKER, 1, level),
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
                quadLayer(mark, fadeNext(ctx, layers, FADE_PICKER, 0, level),
                          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
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
                       XrCompositionLayerQuad* mark, int face, int row, int cell, int cells,
                       float scale) {
    float span = (COG_TRACK_R - COG_TRACK_L) / cells;
    Vec3 local;
    local.x = (COG_TRACK_L + (cell + 0.5f) * span - 0.5f) * ctx->cogW;
    local.y = (0.5f - cogRowV(face, row)) * ctx->cogH;
    local.z = 0.004f;
    quadLayer(mark, fadeNext(ctx, layers, FADE_COG, 0, ctx->panelFades[FADE_COG].level),
              XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              ctx->outlineSwapchain, OUTLINE_TEX, OUTLINE_TEX, view->space,
              poseOffset(ctx->cogPose, local), span * ctx->cogW * scale,
              2.0f * cogCellHalf(face) * ctx->cogH * scale);
    pushLayer(ctx, layers, mark);
}

// The settings panel, the rings on its rows of cells, the thumbs on its
// sliders, and on the Room, Picture and Screen tabs the values beside them
static void addCogLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The settings panel, at the pose it was opened with. The tab is a
    // choice of swapchain, all were filled at startup, and a room has its
    // own sheets. Sharpened: it carries text.
    int art = cogArt(ctx);
    int face = cogFace(ctx);
    float level = ctx->panelFades[FADE_COG].level;
    if (level > 0.0f && ctx->cogPanelReady[art]) {
        quadLayer(&layers->cogPanel, fadeNext(ctx, layers, FADE_COG, 1, level),
                  XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  ctx->cogPanelSwapchains[art], COG_TEX_W, COG_TEX_H, view->space,
                  ctx->cogPose, ctx->cogW, ctx->cogH);
        pushLayer(ctx, layers, &layers->cogPanel);

        // The time and the battery, on a strip just over the top edge, so a
        // long session has a clock somewhere without the stats up
        if (ctx->cogClockReady) {
            float stripW = (float)COG_CLOCK_TEX_W / (float)COG_TEX_W * ctx->cogW;
            float stripH = (float)COG_CLOCK_TEX_H / (float)COG_TEX_H * ctx->cogH;
            Vec3 local = { 0.0f, ctx->cogH * 0.5f + stripH * 0.75f, 0.0f };
            quadLayer(&layers->cogClock, fadeNext(ctx, layers, FADE_COG, 1, level),
                      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->cogClockSwapchain, COG_CLOCK_TEX_W, COG_CLOCK_TEX_H, view->space,
                      poseOffset(ctx->cogPose, local), stripW, stripH);
            pushLayer(ctx, layers, &layers->cogClock);
        }

        // The cells are drawn into the texture, so what is chosen and what is
        // under the ray are marks over them. On the display tab the choices
        // are one strip Java draws for every row at once, then a ring for the
        // hover.
        if (face == COG_TAB_DISPLAY) {
            if (ctx->cogMarksReady) {
                float stripW = (float)COG_MARKS_TEX_W / (float)COG_TEX_W;
                float stripH = (float)COG_MARKS_TEX_H / (float)COG_TEX_H;
                Vec3 local;
                local.x = (COG_MARKS_L + stripW * 0.5f - 0.5f) * ctx->cogW;
                local.y = (0.5f - (COG_MARKS_T + stripH * 0.5f)) * ctx->cogH;
                local.z = 0.003f;
                quadLayer(&layers->cogMarks, fadeNext(ctx, layers, FADE_COG, 0, level),
                          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                          ctx->cogMarksSwapchain, COG_MARKS_TEX_W, COG_MARKS_TEX_H, view->space,
                          poseOffset(ctx->cogPose, local), stripW * ctx->cogW,
                          stripH * ctx->cogH);
                pushLayer(ctx, layers, &layers->cogMarks);
            }
            int option = ctx->cogHoverSlider;
            if (ctx->outlineReady && option >= 0 && option < COG_OPTION_COUNT
                    && ctx->cogHoverCell >= 0) {
                addCogRing(ctx, view, layers, &layers->cogMark[COG_OPTION_COUNT], face, option,
                           ctx->cogHoverCell, cogOptionCells(option), 1.12f);
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
                    addCogRing(ctx, view, layers, &layers->cogMark[marks++], face, row, cell,
                               cogRowCells(face, row), 1.0f);
                }
            }
            int hoverRow = ctx->cogHoverSlider;
            if (hoverRow >= 0 && !cogRowIsTrack(face, hoverRow) && ctx->cogHoverCell >= 0) {
                addCogRing(ctx, view, layers, &layers->cogMark[COG_OPTION_COUNT], face,
                           hoverRow, ctx->cogHoverCell, cogRowCells(face, hoverRow), 1.12f);
            }
        }

        // The About tab's button gets the hover ring the cells do, in the
        // same slot
        if (face == COG_TAB_ABOUT && ctx->cogReportHot && ctx->outlineReady) {
            Vec3 local;
            local.x = ((COG_REPORT_L + COG_REPORT_R) * 0.5f - 0.5f) * ctx->cogW;
            local.y = (0.5f - (COG_REPORT_T + COG_REPORT_B) * 0.5f) * ctx->cogH;
            local.z = 0.004f;
            XrCompositionLayerQuad* mark = &layers->cogMark[COG_OPTION_COUNT];
            quadLayer(mark, fadeNext(ctx, layers, FADE_COG, 0, level),
                      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->outlineSwapchain, OUTLINE_TEX, OUTLINE_TEX, view->space,
                      poseOffset(ctx->cogPose, local),
                      (COG_REPORT_R - COG_REPORT_L) * ctx->cogW * 1.04f,
                      (COG_REPORT_B - COG_REPORT_T) * ctx->cogH * 1.12f);
            pushLayer(ctx, layers, mark);
        }

        // A step button under the ray gets the same hover ring a cell does,
        // in the slot the cells' hover uses, since the ray is on one or the
        // other
        int stepRow = ctx->cogHoverSlider;
        if (ctx->outlineReady && ctx->cogHoverStep != 0 && stepRow >= 0
                && cogRowIsTrack(face, stepRow)) {
            float centre = ctx->cogHoverStep > 0 ? COG_TRACK_R - COG_CHEVRON_W * 0.5f
                                                 : COG_TRACK_L + COG_CHEVRON_W * 0.5f;
            Vec3 local;
            local.x = (centre - 0.5f) * ctx->cogW;
            local.y = (0.5f - cogRowV(face, stepRow)) * ctx->cogH;
            local.z = 0.004f;
            XrCompositionLayerQuad* mark = &layers->cogMark[COG_OPTION_COUNT];
            quadLayer(mark, fadeNext(ctx, layers, FADE_COG, 0, level),
                      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->outlineSwapchain, OUTLINE_TEX, OUTLINE_TEX, view->space,
                      poseOffset(ctx->cogPose, local), COG_CHEVRON_W * ctx->cogW * 1.12f,
                      2.0f * cogCellHalf(face) * ctx->cogH * 1.12f);
            pushLayer(ctx, layers, mark);
        }

        // The Room tab's percents, the Picture tab's values or head aim's on
        // the Screen tab, once the strip says what the rows do now. A strip
        // still showing another tab's or another room's values, or a value a
        // drag has just moved past, stays down until Java has drawn it again.
        if ((face == COG_FACE_ROOM || face == COG_TAB_PICTURE || face == COG_TAB_SCREEN)
                && ctx->cogReadoutReady) {
            int readouts[READOUT_VALUES];
            cogReadouts(ctx, readouts);
            if (readouts[0] >= 0
                    && memcmp(readouts, ctx->cogReadoutDrawn, sizeof(readouts)) == 0) {
                float stripW = (float)COG_READOUT_TEX_W / (float)COG_TEX_W;
                float stripH = (float)COG_READOUT_TEX_H / (float)COG_TEX_H;
                Vec3 local;
                local.x = (COG_READOUT_L + stripW * 0.5f - 0.5f) * ctx->cogW;
                local.y = (0.5f - (COG_READOUT_T + stripH * 0.5f)) * ctx->cogH;
                local.z = 0.003f;
                quadLayer(&layers->cogReadout, fadeNext(ctx, layers, FADE_COG, 1, level),
                          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                          ctx->cogReadoutSwapchain, COG_READOUT_TEX_W, COG_READOUT_TEX_H,
                          view->space, poseOffset(ctx->cogPose, local), stripW * ctx->cogW,
                          stripH * ctx->cogH);
                pushLayer(ctx, layers, &layers->cogReadout);
            }
        }

        if (ctx->cogThumbReady) {
            float thumbSize = ctx->cogH * cogThumbSize(face);
            int rowCount = cogTabRowCount(face);
            for (int s = 0; s < rowCount; s++) {
                // No thumb on a row that cannot be dragged, or on a row of
                // cells, which has rings over it instead
                if (!cogRowLive(ctx, face, s) || !cogRowIsTrack(face, s)) {
                    continue;
                }
                float t = cogSliderValue(ctx, face, s);
                Vec3 local;
                local.x = (cogRunU(t) - 0.5f) * ctx->cogW;
                local.y = (0.5f - cogRowV(face, s)) * ctx->cogH;
                local.z = 0.004f;
                // Grows under the ray, the same feedback the buttons give,
                // though not while the ray is on a step button instead
                float grow = ((ctx->cogHoverSlider == s && ctx->cogHoverStep == 0)
                              || ctx->cogDragSlider == s) ? 1.25f : 1.0f;

                XrCompositionLayerQuad* thumb = &layers->cogThumb[s];
                quadLayer(thumb, fadeNext(ctx, layers, FADE_COG, 0, level),
                          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
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
    // The report sheet is the exception, being what it types into.
    float level = ctx->panelFades[FADE_KB].level;
    if (level > 0.0f && ctx->kbPanelReady[ctx->kbState]) {
        quadLayer(&layers->kbPanel, fadeNext(ctx, layers, FADE_KB, 1, level),
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

            quadLayer(&layers->kbMark, fadeNext(ctx, layers, FADE_KB, 0, level),
                      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                      ctx->outlineSwapchain, OUTLINE_TEX, OUTLINE_TEX, view->space,
                      poseOffset(ctx->kbPose, local), (r[2] - r[0]) * ctx->kbW * grow,
                      (r[3] - r[1]) * ctx->kbH * grow);
            pushLayer(ctx, layers, &layers->kbMark);
        }
    }
}

// The controller models' own projection layer, alpha blended over everything
// in the room before it: a controller is nearer than the picture and the
// panels, so it is drawn over them wherever the two cross. World locked, with
// the poses its image was drawn from.
static void addModelLayer(XrCtx* ctx, FrameLayers* layers) {
    if (!ctx->modelsShowing || !ctx->modelRendered || ctx->passthrough) {
        return;
    }
    XrCompositionLayerProjection* models = &layers->models;
    memset(models, 0, sizeof(*models));
    memset(layers->modelViews, 0, sizeof(layers->modelViews));
    models->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    models->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    models->space = ctx->localSpace;
    models->viewCount = ROOM_EYES;
    models->views = layers->modelViews;
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        XrCompositionLayerProjectionView* projView = &layers->modelViews[eye];
        projView->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projView->pose = ctx->modelViews[eye].pose;
        projView->fov = ctx->modelViews[eye].fov;
        projView->subImage.swapchain = ctx->modelSwapchain;
        projView->subImage.imageRect.offset.x = eye * ctx->modelEyeWidth;
        projView->subImage.imageRect.offset.y = 0;
        projView->subImage.imageRect.extent.width = ctx->modelEyeWidth;
        projView->subImage.imageRect.extent.height = ctx->modelEyeHeight;
        projView->subImage.imageArrayIndex = 0;
    }
    pushLayer(ctx, layers, models);
}

// The laser and the cursor at the end of it
static void addPointerLayers(XrCtx* ctx, const FrameView* view, FrameLayers* layers) {
    // The beam goes when the ray is switched off, the hands' as well, unless
    // a panel is up. Only the beam: the dot stays where the ray lands.
    int beamShown = rayDrawn(ctx->raySetting, ctx->rayFlipped, panelUp(ctx));
    if (beamShown != ctx->rayDrawnSaid) {
        if (ctx->rayDrawnSaid >= 0 || !beamShown) {
            LOGI("ray %s", beamShown ? (raySwitchOn(ctx->raySetting, ctx->rayFlipped)
                                        ? "drawn" : "drawn while a panel is up")
                                     : "hidden, the dot stays");
        }
        ctx->rayDrawnSaid = beamShown;
    }

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

        if (length > 0.10f && beamShown) {
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

// The toast, hung off the eyes ahead and a little below where they look, so
// it reads wherever the picture is and never covers its middle. Nothing hit
// tests it: a press aimed through it lands on whatever is behind. It shows
// while the notice it was drawn for is the one up, and fades out after.
static void addToastLayer(XrCtx* ctx, FrameLayers* layers, long now) {
    int shown = ctx->toastArtReady && noticeShowing(&ctx->notices, now)
            && ctx->toastDrawnKind == ctx->notices.current.kind
            && ctx->toastDrawnArg == ctx->notices.current.arg;
    fadeStep(&ctx->toastFade, shown, now, ctx->fadeNs, ctx->colorScaleSupported);
    float level = ctx->toastFade.level;
    if (level <= 0.0f || !ctx->toastArtReady) {
        return;
    }
    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.w = 1.0f;
    pose.position.y = -TOAST_DROP_M;
    pose.position.z = -TOAST_DISTANCE_M;
    quadLayer(&layers->toast, fadeNext(ctx, layers, FADE_SLOT_TOAST, 1, level),
              XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, ctx->toastSwapchain,
              TOAST_TEX_W, TOAST_TEX_H, ctx->viewSpace, pose, TOAST_W_M,
              TOAST_W_M * TOAST_TEX_H / TOAST_TEX_W);
    pushLayer(ctx, layers, &layers->toast);
}

// Steps each panel's fade toward whether it is showing. A panel opening takes
// any other still on its way out away at once, so two are never up together,
// and the keyboard stands down at once for a modal the way it always has.
static void stepPanelFades(XrCtx* ctx, long now) {
    static const char* const NAMES[FADE_PANELS] = {
        "settings panel", "picker", "keyboard", "exit prompt", "report sheet"
    };
    int modal = ctx->pickerOpen || ctx->cogOpen || ctx->exitConfirmOpen || ctx->reportOpen;
    int shown[FADE_PANELS];
    shown[FADE_COG] = ctx->cogOpen;
    shown[FADE_PICKER] = ctx->pickerOpen;
    // Up under the report sheet, which is what it types into
    shown[FADE_KB] = ctx->kbOpen && (!modal || ctx->reportOpen);
    shown[FADE_EXIT] = ctx->exitConfirmOpen;
    shown[FADE_REPORT] = ctx->reportOpen;
    ctx->panelFadingOut = 0;
    for (int p = 0; p < FADE_PANELS; p++) {
        Fade* fade = &ctx->panelFades[p];
        if (modal && !shown[p]) {
            fade->level = 0.0f;
            fade->running = 0;
        }
        int landed = fadeStep(fade, shown[p], now, ctx->fadeNs, ctx->colorScaleSupported);
        if (landed != FADE_NONE) {
            LOGI("%s faded %s over %ld ms, %d frames", NAMES[p],
                 landed == FADE_IN_DONE ? "in" : "out", (long)((now - fade->fromNs) / 1000000L),
                 fade->frames);
        }
        if (!shown[p] && fade->level > 0.0f) {
            ctx->panelFadingOut = 1;
        }
    }
}

// What the splash is still waiting on: the panels' art, the room the picker
// is on built and drawn, and the depth model's first map, each only where it
// is wanted at all. A room that failed or a model that gave up is not waited
// on, since nothing more is coming from either.
static int splashWaiting(XrCtx* ctx) {
    int waiting = 0;
    if (!ctx->panelArtArrived) {
        waiting |= SPLASH_WAIT_PANELS;
    }
    int style = roomEffective(ctx);
    if (style > 0 && !ctx->roomFailed
            && !(ctx->roomRendered && ctx->roomBuiltStyle == style)) {
        waiting |= SPLASH_WAIT_ROOM;
    }
    if (ctx->stereoMode == DEPTH_MODE_MODEL
            && atomic_load_explicit(&ctx->depthMapsStaged, memory_order_relaxed) == 0
            && !atomic_load_explicit(&ctx->depthGaveUp, memory_order_relaxed)) {
        waiting |= SPLASH_WAIT_DEPTH;
    }
    return waiting;
}

// The line that says when the splash went and why: the floor, everything
// being ready, or the ceiling with whatever it was still waiting on
static void logSplashLift(XrCtx* ctx) {
    static const char* const NAMES[3] = { "panels", "room", "depth model" };
    long upMs = (long)((ctx->splash.liftNs - ctx->splash.firstNs) / 1000000L);
    char parts[160];
    parts[0] = '\0';
    for (int i = 0; i < 3; i++) {
        int wanted = i == 0 || (i == 1 && roomEffective(ctx) > 0)
                || (i == 2 && ctx->stereoMode == DEPTH_MODE_MODEL);
        if (!wanted) {
            continue;
        }
        size_t used = strlen(parts);
        if (ctx->splashReadyMs[i] >= 0) {
            snprintf(parts + used, sizeof(parts) - used, "%s%s at %d ms", used > 0 ? ", " : "",
                     NAMES[i], ctx->splashReadyMs[i]);
        }
        else {
            snprintf(parts + used, sizeof(parts) - used, "%s%s not ready", used > 0 ? ", " : "",
                     NAMES[i]);
        }
    }
    const char* why = ctx->splash.waitingAtLift != 0 ? "the ceiling"
            : upMs <= SPLASH_FLOOR_NS / 1000000L + 50 ? "the floor" : "ready";
    LOGEV("splash lifted after %ld ms (%s): %s, %s", upMs, why, parts,
          ctx->colorScaleSupported ? "fading" : "cut");
}

// One frame of the splash: what has become ready since the last, and whether
// it lifts now
static void stepSplash(XrCtx* ctx, long now) {
    if (ctx->splash.phase == SPLASH_GONE) {
        return;
    }
    int waiting = splashWaiting(ctx);
    if (ctx->splash.firstNs != 0) {
        int bits[3] = { SPLASH_WAIT_PANELS, SPLASH_WAIT_ROOM, SPLASH_WAIT_DEPTH };
        for (int i = 0; i < 3; i++) {
            if (ctx->splashReadyMs[i] < 0 && !(waiting & bits[i])) {
                ctx->splashReadyMs[i] = (int)((now - ctx->splash.firstNs) / 1000000L);
            }
        }
    }
    long fadeNs = ctx->colorScaleSupported ? 2 * ctx->fadeNs : 0;
    int phase = ctx->splash.phase;
    if (splashStep(&ctx->splash, now, waiting, fadeNs)) {
        logSplashLift(ctx);
    }
    if (phase == SPLASH_FADING && ctx->splash.phase == SPLASH_GONE) {
        LOGI("splash gone, faded over %ld ms", (long)((now - ctx->splash.liftNs) / 1000000L));
    }
}

// The splash, locked to the head and in front of everything: a black quad
// wider than any view, cut from the black strip under the sheet's rows, and
// the sheet itself on the row for the dots it is up to. While it is fully up
// it is all the frame carries.
static void addSplashLayers(XrCtx* ctx, FrameLayers* layers, long now) {
    if (ctx->splash.phase == SPLASH_GONE || !ctx->splashArtReady) {
        return;
    }
    float level = splashLevel(&ctx->splash, now, 2 * ctx->fadeNs);
    const void* next = fadeNext(ctx, layers, FADE_SLOT_SPLASH, 0, level);
    XrPosef pose;
    memset(&pose, 0, sizeof(pose));
    pose.orientation.w = 1.0f;

    pose.position.z = -SPLASH_BLACK_DISTANCE_M;
    quadLayer(&layers->splashBlack, next, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              ctx->splashSwapchain, SPLASH_TEX_W, SPLASH_TEX_H, ctx->viewSpace, pose,
              SPLASH_BLACK_M, SPLASH_BLACK_M);
    // Well inside the black strip, which arrives as the bottom rows of the
    // image, so filtering never reaches past it
    layers->splashBlack.subImage.imageRect.offset.x = 8;
    layers->splashBlack.subImage.imageRect.offset.y = 4;
    layers->splashBlack.subImage.imageRect.extent.width = 8;
    layers->splashBlack.subImage.imageRect.extent.height = SPLASH_BLACK_PX - 8;
    pushLayer(ctx, layers, &layers->splashBlack);

    // The rows were drawn top down and arrive bottom up
    int row = splashRow(&ctx->splash, now, SPLASH_ROWS);
    pose.position.z = -SPLASH_SHEET_DISTANCE_M;
    quadLayer(&layers->splashSheet, next, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              ctx->splashSwapchain, SPLASH_TEX_W, SPLASH_ROW_H, ctx->viewSpace, pose,
              SPLASH_SHEET_W_M, SPLASH_SHEET_W_M * SPLASH_ROW_H / SPLASH_TEX_W);
    layers->splashSheet.subImage.imageRect.offset.y = SPLASH_TEX_H - (row + 1) * SPLASH_ROW_H;
    pushLayer(ctx, layers, &layers->splashSheet);
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

    // Where the controllers are this frame, before anything draws them
    updateControllerModels(ctx);

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
    // The controller models on every frame, a new picture or not, since a
    // controller moves on its own. After the warp, so the picture never
    // waits on them.
    if (ctx->shouldRender && ctx->everRendered) {
        renderControllerModels(ctx);
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
    // A panel on its way out keeps the furniture down until it has gone, the
    // way an open one does, so the two never stack up in one frame
    long frameNs = nowNs();
    stepPanelFades(ctx, frameNs);
    stepSplash(ctx, frameNs);
    int splashUp = ctx->splash.phase == SPLASH_UP;
    view.barArea = !ctx->pickerOpen && !ctx->cogOpen && !ctx->kbOpen
            && !ctx->exitConfirmOpen && !ctx->reportOpen && !ctx->panelFadingOut
            && (ctx->hoverKind == HOVER_BAR || ctx->hoverKind == HOVER_ENVBUTTON
                || ctx->hoverKind == HOVER_COGBUTTON
                || ctx->hoverKind == HOVER_KBBUTTON
                || ctx->hoverKind == HOVER_EXITBUTTON
                || ctx->hoverKind == HOVER_STEREOBUTTON
                || ctx->hoverKind == HOVER_RAYBUTTON
                || ctx->hoverKind == HOVER_AIMBUTTON
                || ctx->hoverKind == HOVER_PADBUTTON);

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = ctx->predictedDisplayTime;
    // Some runtimes only bring the cameras up on a change of blend mode seen
    // after the session is focused, and asking for alpha blend from the very
    // first frame leaves them off for the whole session. Submitting the first
    // focused frame opaque gives every runtime the transition it wants. Later
    // switches from the picker are long past this point.
    // The splash is opaque black for as long as it is fully up, so the cameras
    // wait for it to start going, which is also after the first focused frame
    int wantPassthrough = ctx->passthrough && ctx->alphaBlendSupported;
    int blendNow = wantPassthrough && ctx->focusedFrames > 0 && !splashUp;
    endInfo.environmentBlendMode = blendNow
            ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (blendNow && !ctx->passthroughBlendSaid) {
        ctx->passthroughBlendSaid = 1;
        LOGI("passthrough blend enabled after %d focused frames", ctx->focusedFrames);
        LOGEV("passthrough blend enabled after %d focused frames", ctx->focusedFrames);
    }
    if (ctx->sessionState == XR_SESSION_STATE_FOCUSED) {
        ctx->focusedFrames++;
    }

    FrameLayers layers;
    layers.count = 0;
    setLayerSettings(ctx, &layers);

    // Behind the splash while it is fully up there is nothing to see, so
    // nothing else goes up with it
    if (!splashUp) {
        addRoomLayer(ctx, &view, &layers);
        addGlowLayer(ctx, &view, &layers);
    }
    if (!splashUp && ctx->everRendered && ctx->shouldRender) {
        addVideoLayers(ctx, &view, &layers);
        addOverlayLayer(ctx, &view, &layers);
        addHandleLayer(ctx, &view, &layers);
        addBarButtonLayers(ctx, &view, &layers);
        addExitPromptLayer(ctx, &view, &layers);
        addLockLayer(ctx, &view, &layers);
        addPickerLayers(ctx, &view, &layers);
        addCogLayers(ctx, &view, &layers);
        addReportLayer(ctx, &view, &layers);
        addKeyboardLayers(ctx, &view, &layers);
        addModelLayer(ctx, &layers);
        addPointerLayers(ctx, &view, &layers);
    }
    // Over everything in the scene, since it hangs off the eyes, but under
    // the splash, which keeps it back until it has gone
    addToastLayer(ctx, &layers, frameNs);
    // Last, so it is in front of everything
    addSplashLayers(ctx, &layers, frameNs);

    // The Room tab at its fullest, with a room, the glow, the stats and the
    // ray all up, is a layer past the Pico's sixteen, two with the toast, and
    // the controller models put three more of the tabs past it. A frame over
    // the limit is refused whole (the count is in the comment at the top).
    // The toast goes first: it is only ever a few seconds of words, and what
    // it says is still true without it. Then the controller models: a hand
    // that goes unseen while a full tab is up still points, and the beam and
    // the dot are still there to show where. Then the hover ring, which
    // every tab and the step buttons share: the cursor already shows where the
    // ray is. Then the cog button, which only says which panel is open while
    // it is: a press off the panel closes it the way pressing the button
    // would. Then the clock over the panel, which the stats can show as well.
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.toast);
    }
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.models);
    }
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.cogMark[COG_OPTION_COUNT]);
    }
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.cogButton);
    }
    if (layers.count > (uint32_t)ctx->maxLayerCount) {
        dropLayer(&layers, &layers.cogClock);
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
