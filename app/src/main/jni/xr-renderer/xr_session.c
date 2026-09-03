// The OpenXR instance, session and swapchains, the session state machine,
// and the JNI entry points that bring the renderer up and take it down.
#include "xr_renderer.h"

int checkXr(XrResult res, const char* what) {
    if (XR_FAILED(res)) {
        LOGE("%s failed: %d", what, res);
        return 0;
    }
    return 1;
}

static int initEgl(XrCtx* ctx) {
    ctx->eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->eglDisplay == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return 0;
    }
    if (!eglInitialize(ctx->eglDisplay, NULL, NULL)) {
        LOGE("eglInitialize failed");
        return 0;
    }

    const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLint numConfigs = 0;
    if (!eglChooseConfig(ctx->eglDisplay, configAttribs, &ctx->eglConfig, 1, &numConfigs) ||
            numConfigs < 1) {
        LOGE("eglChooseConfig failed");
        return 0;
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx->eglContext = eglCreateContext(ctx->eglDisplay, ctx->eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (ctx->eglContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: %d", eglGetError());
        return 0;
    }

    // The context needs a surface current but everything renders to FBOs
    const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ctx->eglPbuffer = eglCreatePbufferSurface(ctx->eglDisplay, ctx->eglConfig, pbufferAttribs);
    if (ctx->eglPbuffer == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed: %d", eglGetError());
        return 0;
    }

    if (!eglMakeCurrent(ctx->eglDisplay, ctx->eglPbuffer, ctx->eglPbuffer, ctx->eglContext)) {
        LOGE("eglMakeCurrent failed: %d", eglGetError());
        return 0;
    }

    return 1;
}

// Room for every extension this renderer knows how to ask for, with a little
// to spare
#define MAX_ENABLED_EXTS 16

// Adds an extension to the list handed to xrCreateInstance, within the list's
// own bound rather than past it
static void enableExt(const char** list, uint32_t* count, const char* name) {
    if (*count >= MAX_ENABLED_EXTS) {
        LOGW("no room to enable %s, leaving it out", name);
        return;
    }
    list[(*count)++] = name;
}

// The few results a user's log is likely to carry, named, since the number on
// its own sends everyone to the header
static const char* xrResultName(XrResult res) {
    switch (res) {
        case XR_ERROR_RUNTIME_UNAVAILABLE:
            return "no OpenXR runtime answered the loader";
        case XR_ERROR_RUNTIME_FAILURE:
            return "the OpenXR runtime failed";
        case XR_ERROR_INITIALIZATION_FAILED:
            return "initialization failed";
        case XR_ERROR_API_VERSION_UNSUPPORTED:
            return "API version unsupported";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
            return "headset not available";
        default:
            return "see XrResult in openxr.h";
    }
}

static int initXrInstance(XrCtx* ctx) {
    PFN_xrInitializeLoaderKHR initLoader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&initLoader);
    if (initLoader != NULL) {
        XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        loaderInfo.applicationVM = ctx->vm;
        loaderInfo.applicationContext = ctx->activity;
        checkXr(initLoader((XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo), "xrInitializeLoaderKHR");
    }

    // The failure a user is most likely to meet is here: on Android the loader
    // finds the runtime through the OS's OpenXR runtime broker, and a headset
    // without one, or one that refuses this app, answers with no runtime at
    // all. Nothing further along can work without it, so it is said plainly.
    uint32_t extCount = 0;
    XrResult listed = xrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL);
    if (XR_FAILED(listed)) {
        LOGE("xrEnumerateInstanceExtensionProperties failed: %d, %s", listed,
             xrResultName(listed));
        return 0;
    }
    XrExtensionProperties* exts = calloc(extCount, sizeof(XrExtensionProperties));
    if (extCount > 0 && exts == NULL) {
        LOGE("no memory for %u extension properties", extCount);
        return 0;
    }
    for (uint32_t i = 0; i < extCount; i++) {
        exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, exts);

    int haveGles = 0, haveAndroidCreate = 0;
    LOGEV("runtime offers %u OpenXR extensions", extCount);
    for (uint32_t i = 0; i < extCount; i++) {
        LOGI("  extension %s", exts[i].extensionName);
        if (!strcmp(exts[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) haveGles = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) haveAndroidCreate = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) ctx->cylinderSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) ctx->picoInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME)) ctx->equirectSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_HAND_INTERACTION_EXTENSION_NAME)) ctx->handInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_MSFT_HAND_INTERACTION_EXTENSION_NAME)) ctx->msftHandInteraction = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME)) ctx->handTracking = 1;
        if (!strcmp(exts[i].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) ctx->eyeGaze = 1;
        if (!strcmp(exts[i].extensionName, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME)) ctx->layerSettingsSupported = 1;
        if (!strcmp(exts[i].extensionName, XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME)) ctx->virtualKeyboardSupported = 1;
    }
    free(exts);

    // One gate for the whole feature. With it off none of the hand extensions
    // are enabled, so no hand profile is ever current and everything
    // downstream sees a headset with only controllers.
    if (!ctx->handsEnabled) {
        ctx->handInteraction = 0;
        ctx->msftHandInteraction = 0;
        ctx->handTracking = 0;
        LOGI("hand tracking off by preference");
    }

    if (!haveGles || !haveAndroidCreate) {
        LOGE("required OpenXR extensions missing (gles=%d androidCreate=%d)", haveGles, haveAndroidCreate);
        return 0;
    }

    const char* enabledExts[MAX_ENABLED_EXTS];
    uint32_t enabledCount = 0;
    enableExt(enabledExts, &enabledCount, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
    enableExt(enabledExts, &enabledCount, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    if (ctx->cylinderSupported) {
        enableExt(enabledExts, &enabledCount, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }
    if (ctx->picoInteraction) {
        enableExt(enabledExts, &enabledCount, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    }
    if (ctx->equirectSupported) {
        enableExt(enabledExts, &enabledCount, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME);
    }
    if (ctx->handInteraction) {
        enableExt(enabledExts, &enabledCount, XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    }
    // Some runtimes will not honour the hand interaction profile unless the
    // tracking extension is enabled next to it
    if (ctx->handTracking) {
        enableExt(enabledExts, &enabledCount, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    }
    if (ctx->eyeGaze) {
        enableExt(enabledExts, &enabledCount, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }
    if (ctx->msftHandInteraction) {
        enableExt(enabledExts, &enabledCount, XR_MSFT_HAND_INTERACTION_EXTENSION_NAME);
    }
    if (ctx->layerSettingsSupported) {
        enableExt(enabledExts, &enabledCount, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    }

    XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    androidInfo.applicationVM = ctx->vm;
    androidInfo.applicationActivity = ctx->activity;

    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.next = &androidInfo;
    strncpy(createInfo.applicationInfo.applicationName, "Moonlight", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "Moonlight", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = enabledCount;
    createInfo.enabledExtensionNames = enabledExts;

    XrResult created = xrCreateInstance(&createInfo, &ctx->instance);
    if (XR_FAILED(created)) {
        LOGE("xrCreateInstance failed: %d, %s", created, xrResultName(created));
        return 0;
    }

    // Which runtime we ended up on, since a report from a headset we do not
    // have starts with knowing what answered
    XrInstanceProperties instanceProps = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(ctx->instance, &instanceProps))) {
        LOGEV("runtime %s %u.%u.%u", instanceProps.runtimeName,
              (unsigned)XR_VERSION_MAJOR(instanceProps.runtimeVersion),
              (unsigned)XR_VERSION_MINOR(instanceProps.runtimeVersion),
              (unsigned)XR_VERSION_PATCH(instanceProps.runtimeVersion));
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!checkXr(xrGetSystem(ctx->instance, &systemInfo, &ctx->systemId), "xrGetSystem")) {
        return 0;
    }

    uint32_t blendModeCount = 0;
    xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     0, &blendModeCount, NULL);
    XrEnvironmentBlendMode* modes = blendModeCount > 0
            ? calloc(blendModeCount, sizeof(XrEnvironmentBlendMode)) : NULL;
    if (modes != NULL) {
        xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         blendModeCount, &blendModeCount, modes);
        for (uint32_t i = 0; i < blendModeCount; i++) {
            LOGEV("environment blend mode %u available", modes[i]);
            if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                ctx->alphaBlendSupported = 1;
            }
        }
        free(modes);
    }
    LOGEV("passthrough %s", ctx->alphaBlendSupported ? "available" : "not offered by this runtime");
    LOGEV("compositor sharpening %s", ctx->layerSettingsSupported
          ? "available (XR_FB_composition_layer_settings)" : "not offered by this runtime");
    LOGEV("virtual keyboard extension %s", ctx->virtualKeyboardSupported
          ? "available (XR_META_virtual_keyboard)" : "not offered by this runtime");

    // How many layers a frame may carry. The furniture, the panel and the glow
    // all come and go on their own, so the ceiling is worth knowing rather than
    // guessing at. A runtime that will not say gets the spec's minimum.
    ctx->maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    XrSystemProperties layerProps = { XR_TYPE_SYSTEM_PROPERTIES };
    if (!XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &layerProps))) {
        if (layerProps.graphicsProperties.maxLayerCount > 0) {
            ctx->maxLayerCount = (int)layerProps.graphicsProperties.maxLayerCount;
        }
        // Worth having in a user's log, it is the one place an unknown headset
        // names itself
        LOGEV("system %s (vendor 0x%x)", layerProps.systemName, layerProps.vendorId);
    }

    // What the runtime would like a rendered view to be. Asked once, and the
    // 3d room is the only thing that renders one, so nothing else looks at
    // it. Worth a line either way: it says what a headset's own idea of full
    // resolution is.
    uint32_t configViewCount = 0;
    xrEnumerateViewConfigurationViews(ctx->instance, ctx->systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &configViewCount, NULL);
    XrViewConfigurationView* configViews = configViewCount > 0
            ? calloc(configViewCount, sizeof(XrViewConfigurationView)) : NULL;
    if (configViews != NULL) {
        for (uint32_t i = 0; i < configViewCount; i++) {
            configViews[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        }
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                ctx->instance, ctx->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                configViewCount, &configViewCount, configViews))) {
            ctx->recommendedEyeWidth = (int)configViews[0].recommendedImageRectWidth;
            ctx->recommendedEyeHeight = (int)configViews[0].recommendedImageRectHeight;
            ctx->maxEyeWidth = (int)configViews[0].maxImageRectWidth;
            ctx->maxEyeHeight = (int)configViews[0].maxImageRectHeight;
            LOGI("recommended render size %dx%d per eye (max %dx%d), %u views",
                 ctx->recommendedEyeWidth, ctx->recommendedEyeHeight,
                 ctx->maxEyeWidth, ctx->maxEyeHeight, configViewCount);
        }
        free(configViews);
    }

    // Offering the extension is not the same as having the hardware, so the
    // system is asked directly before anything is bound to a gaze
    if (ctx->eyeGaze) {
        XrSystemEyeGazeInteractionPropertiesEXT gazeProps = {
            XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT
        };
        XrSystemProperties props = { XR_TYPE_SYSTEM_PROPERTIES };
        props.next = &gazeProps;
        if (XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &props))
                || !gazeProps.supportsEyeGazeInteraction) {
            ctx->eyeGaze = 0;
        }
        LOGEV("eye gaze %s", ctx->eyeGaze ? "available" : "offered but not supported by this system");
    }

    if (ctx->handTracking) {
        XrSystemHandTrackingPropertiesEXT handProps = {
            XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT
        };
        XrSystemProperties props = { XR_TYPE_SYSTEM_PROPERTIES };
        props.next = &handProps;
        if (XR_FAILED(xrGetSystemProperties(ctx->instance, ctx->systemId, &props))
                || !handProps.supportsHandTracking) {
            ctx->handTracking = 0;
        }
        LOGEV("hand joints %s", ctx->handTracking ? "available" : "not supported by this system");
    }

    xrGetInstanceProcAddr(ctx->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&ctx->pfnGetGlesReqs);
    if (ctx->pfnGetGlesReqs == NULL) {
        LOGE("xrGetOpenGLESGraphicsRequirementsKHR not found");
        return 0;
    }

    return 1;
}

static int initXrSession(XrCtx* ctx) {
    // Spec requires this call before session creation
    XrGraphicsRequirementsOpenGLESKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    if (!checkXr(ctx->pfnGetGlesReqs(ctx->instance, ctx->systemId, &reqs), "get gles requirements")) {
        return 0;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    binding.display = ctx->eglDisplay;
    binding.config = ctx->eglConfig;
    binding.context = ctx->eglContext;

    XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    sessionInfo.systemId = ctx->systemId;
    if (!checkXr(xrCreateSession(ctx->instance, &sessionInfo, &ctx->session), "xrCreateSession")) {
        return 0;
    }

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!checkXr(xrCreateReferenceSpace(ctx->session, &spaceInfo, &ctx->localSpace), "create local space")) {
        return 0;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!checkXr(xrCreateReferenceSpace(ctx->session, &spaceInfo, &ctx->viewSpace), "create view space")) {
        return 0;
    }

    return 1;
}

static int initSwapchain(XrCtx* ctx) {
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(ctx->session, 0, &formatCount, NULL);
    int64_t* formats = calloc(formatCount, sizeof(int64_t));
    if (formatCount == 0 || formats == NULL) {
        LOGE("no swapchain formats to choose from");
        free(formats);
        return 0;
    }
    xrEnumerateSwapchainFormats(ctx->session, formatCount, &formatCount, formats);

    ctx->swapchainFormat = 0;
    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            ctx->swapchainFormat = GL_SRGB8_ALPHA8;
            break;
        }
    }
    if (ctx->swapchainFormat == 0) {
        ctx->swapchainFormat = formats[0];
        LOGW("no SRGB8_ALPHA8 swapchain format, using %lld", (long long)ctx->swapchainFormat);
    }
    free(formats);

    // Stereo renders left and right eye views side by side in one swapchain
    int chainWidth = ctx->stereoMode != DEPTH_MODE_OFF ? ctx->videoWidth * 2 : ctx->videoWidth;
    if (!createArtSwapchain(ctx, chainWidth, ctx->videoHeight, "xrCreateSwapchain",
                            &ctx->swapchain, &ctx->swapchainImages, &ctx->swapchainImageCount)) {
        return 0;
    }

    LOGEV("swapchain %dx%d format %lld, %u images (stereo mode %d)", chainWidth, ctx->videoHeight,
         (long long)ctx->swapchainFormat, ctx->swapchainImageCount, ctx->stereoMode);

    // The stream is worth more than the stats, so carry on without it
    createArtSwapchain(ctx, OVERLAY_WIDTH, OVERLAY_HEIGHT, "create overlay swapchain",
                       &ctx->overlaySwapchain, &ctx->overlayImages, &ctx->overlayImageCount);

    return 1;
}

static void handleSessionStateChange(XrCtx* ctx, XrSessionState newState) {
    LOGI("session state %d -> %d", ctx->sessionState, newState);
    ctx->sessionState = newState;

    switch (newState) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (checkXr(xrBeginSession(ctx->session, &beginInfo), "xrBeginSession")) {
                ctx->sessionRunning = 1;
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            xrEndSession(ctx->session);
            ctx->sessionRunning = 0;
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            ctx->sessionRunning = 0;
            ctx->exitRequested = 1;
            break;
        default:
            break;
    }
}

static void pollEvents(XrCtx* ctx) {
    XrEventDataBuffer event;
    for (;;) {
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        event.next = NULL;
        XrResult res = xrPollEvent(ctx->instance, &event);
        if (res != XR_SUCCESS) {
            break;
        }
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                XrEventDataSessionStateChanged* sc = (XrEventDataSessionStateChanged*)&event;
                handleSessionStateChange(ctx, sc->state);
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                XrEventDataReferenceSpaceChangePending* change =
                        (XrEventDataReferenceSpaceChangePending*)&event;
                if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    // Recentring is the user saying where forward is, so the
                    // screen goes back to the placement a fresh install has
                    // rather than keeping an offset from the old origin
                    ctx->placementValid = 0;
                    ctx->grabMode = GRAB_NONE;
                    LOGI("recentred, screen placement reset");
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                // Picking a controller up or putting it down swaps the profile
                // on that hand, and the pointer wakes differently for each
                refreshInputSource(ctx);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                ctx->exitRequested = 1;
                break;
            default:
                break;
        }
    }
}

static void destroyCtx(JNIEnv* env, XrCtx* ctx) {
    destroyXrInput(ctx);

    // The depth thread has been joined by now, but a slot published after the
    // frame loop last sampled it still carries a fence nothing waited on
    for (int i = 0; i < DEPTH_TEX_COUNT; i++) {
        if (ctx->depthFences[i] != NULL) {
            glDeleteSync(ctx->depthFences[i]);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (ctx->captureFences[i] != NULL) {
            glDeleteSync(ctx->captureFences[i]);
        }
    }
    glDeleteBuffers(2, ctx->depthPbos);
    glDeleteBuffers(1, &ctx->ambiDetectPbo);
    free(ctx->modelInput);
    free(ctx->modelOutput);
    free(ctx->depthUploadBuf);
    free(ctx->depthEma);
    free(ctx->depthLow);
    free(ctx->depthScratch);
    free(ctx->depthColSums);

    // Destroying the context below would take these anyway. Said explicitly so
    // the glow's resources go together with the swapchain it draws into.
    glDeleteFramebuffers(1, &ctx->ambiFbo);
    glDeleteFramebuffers(1, &ctx->ambiDetectFbo);
    glDeleteFramebuffers(1, &ctx->glowFbo);
    glDeleteTextures(1, &ctx->ambiTexture);
    glDeleteTextures(1, &ctx->ambiDetectTexture);
    if (ctx->ambiProgram != 0) {
        glDeleteProgram(ctx->ambiProgram);
    }
    if (ctx->glowProgram != 0) {
        glDeleteProgram(ctx->glowProgram);
    }

    // Same for the room, whose resources only exist at all if a frame ever ran
    // with it on
    glDeleteFramebuffers(1, &ctx->roomFbo);
    glDeleteRenderbuffers(1, &ctx->roomDepthBuffer);
    glDeleteBuffers(1, &ctx->roomVertexBuffer);
    glDeleteBuffers(1, &ctx->roomIndexBuffer);
    glDeleteTextures(1, &ctx->roomTexture);
    glDeleteTextures(1, &ctx->roomWhiteTexture);
    if (ctx->roomProgram != 0) {
        glDeleteProgram(ctx->roomProgram);
    }
    free(ctx->roomModelVerts);
    free(ctx->roomModelIndices);

    destroyArtSwapchain(&ctx->swapchain, &ctx->swapchainImages);
    destroyArtSwapchain(&ctx->overlaySwapchain, &ctx->overlayImages);
    destroyArtSwapchain(&ctx->pointerSwapchain, &ctx->pointerImages);
    destroyArtSwapchain(&ctx->barSwapchain, &ctx->barImages);
    destroyArtSwapchain(&ctx->cornerSwapchain, &ctx->cornerImages);
    destroyArtSwapchain(&ctx->backgroundSwapchain, &ctx->backgroundImages);
    destroyArtSwapchain(&ctx->pickerSwapchain, &ctx->pickerImages);
    destroyArtSwapchain(&ctx->envButtonSwapchain, &ctx->envButtonImages);
    for (int tab = 0; tab < COG_ART_COUNT; tab++) {
        destroyArtSwapchain(&ctx->cogPanelSwapchains[tab], &ctx->cogPanelImages[tab]);
    }
    destroyArtSwapchain(&ctx->cogButtonSwapchain, &ctx->cogButtonImages);
    destroyArtSwapchain(&ctx->cogThumbSwapchain, &ctx->cogThumbImages);
    for (int state = 0; state < KB_STATE_COUNT; state++) {
        destroyArtSwapchain(&ctx->kbPanelSwapchains[state], &ctx->kbPanelImages[state]);
    }
    destroyArtSwapchain(&ctx->kbButtonSwapchain, &ctx->kbButtonImages);
    for (int sheet = 0; sheet < EXIT_ART_COUNT; sheet++) {
        destroyArtSwapchain(&ctx->exitPromptSwapchains[sheet], &ctx->exitPromptImages[sheet]);
    }
    destroyArtSwapchain(&ctx->exitButtonSwapchain, &ctx->exitButtonImages);
    destroyArtSwapchain(&ctx->lockSwapchain, &ctx->lockImages);
    destroyArtSwapchain(&ctx->unlockSwapchain, &ctx->unlockImages);
    destroyArtSwapchain(&ctx->outlineSwapchain, &ctx->outlineImages);
    destroyArtSwapchain(&ctx->glowSwapchain, &ctx->glowImages);
    destroyArtSwapchain(&ctx->roomSwapchain, &ctx->roomImages);
    if (ctx->localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(ctx->localSpace);
    }
    if (ctx->viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(ctx->viewSpace);
    }
    if (ctx->session != XR_NULL_HANDLE) {
        xrDestroySession(ctx->session);
    }
    if (ctx->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(ctx->instance);
    }

    if (ctx->timerSupported) {
        pfnDeleteQueries(2, ctx->timerQueries);
        pfnDeleteQueries(2, ctx->roomTimerQueries);
    }

    if (ctx->eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(ctx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx->eglPbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(ctx->eglDisplay, ctx->eglPbuffer);
        }
        if (ctx->eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(ctx->eglDisplay, ctx->eglContext);
        }
        eglReleaseThread();
    }

    if (ctx->activity != NULL) {
        (*env)->DeleteGlobalRef(env, ctx->activity);
    }
    free(ctx);
}

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeInit(JNIEnv* env, jobject thiz,
                                                       jobject activity, jint width, jint height,
                                                       jint stereoMode, jboolean depthDebug,
                                                       jint convergence, jint depthScale,
                                                       jboolean handTracking, jint sharpenMode,
                                                       jboolean perfOverlay, jboolean ambilight,
                                                       jint ambiLevel, jboolean roomLight,
                                                       jint envResTier) {
    XrCtx* ctx = calloc(1, sizeof(XrCtx));
    ctx->handsEnabled = handTracking;
    // EnvResTier: 0 low, 1 standard, 2 high, 3 ultra
    ctx->envResTier = envResTier;
    ctx->videoWidth = width;
    ctx->videoHeight = height;
    ctx->stereoMode = stereoMode;
    ctx->depthDebug = depthDebug;
    ctx->sessionState = XR_SESSION_STATE_UNKNOWN;
    // Depth arrives at about 20 Hz, so 0.6 settles in roughly two updates.
    // The range moves much more slowly on purpose, it should track the scene
    // rather than the frame.
    ctx->depthAlpha = 0.60f;
    ctx->rangeAlpha = 0.15f;
    // 0.25 measured best on a captured frame: same 5 px edge as tighter
    // values with a tenth of the speckle
    ctx->upsampleSigmaR = 0.25f;
    ctx->upsampleEnabled = 1;
    ctx->occlusionEnabled = 1;
    // Off until it earns its place in a blind comparison on device
    ctx->depthSharp = 0.0f;
    // Starts where the preference left it, and the panel can change it live
    ctx->overlayVisible = perfOverlay;
    ctx->separationOverride = -1.0f;
    ctx->distanceOverride = -1.0f;
    ctx->screenOverride = -1.0f;
    // Nothing on the settings panel is being dragged, and the curve and
    // separation preferences still own their values
    ctx->panelCurve = -1.0f;
    ctx->panelSeparation = -1.0f;
    // Only a placeholder: the first endFrame writes the real one, long before
    // there is any way to open the panel and read it
    ctx->separationCurrent = 0.005f;
    ctx->cogDragSlider = -1;
    ctx->cogDragHand = -1;
    ctx->cogHoverSlider = -1;
    // No key under the ray, and zero is a real key
    ctx->kbHoverKey = -1;
    ctx->pointerMinCutoff = POINTER_MIN_CUTOFF;
    ctx->pointerBeta = POINTER_BETA;
    ctx->aimMinCutoff = AIM_MIN_CUTOFF;
    ctx->aimBeta = AIM_BETA;
    ctx->pointerWake = POINTER_WAKE_SEC;
    ctx->pointerSleep = POINTER_SLEEP_SEC;
    // 1 cm reads as a thin line at 3 m without disappearing
    ctx->beamWidth = 0.010f;
    ctx->envRadius = ENV_RADIUS_M;
    // Comfort comes from absolute disparity and depth comes from the steps
    // between objects, so the overall shape is pulled toward the screen plane
    // while the local detail is boosted. Measured on captured frames this is
    // about 40 percent more depth at the object boundaries for slightly less
    // clipping than leaving it alone, where the best plain tone curve managed
    // 16 percent.
    ctx->depthGlobal = 1.0f;
    ctx->convergence = convergence / 100.0f;
    ctx->depthLocal = depthScale / 100.0f;
    ctx->sharpenMode = sharpenMode >= 0 && sharpenMode <= 2 ? sharpenMode : 0;
    // The panel owns the glow until a debug property says otherwise
    ctx->ambilightOn = ambilight;
    ctx->ambiIntensity = (ambiLevel < 0 ? 0 : (ambiLevel > 100 ? 100 : ambiLevel)) / 100.0f;
    ctx->ambiOverride = -1;
    // The room's own light off the picture, which the panel owns from here on
    ctx->roomLightOn = roomLight;
    // Same for the room, which the picker sets and a property can force, and
    // for the size and brightness it is drawn at, which its params own until
    // a property says otherwise
    ctx->roomOverride = -1;
    ctx->roomScaleOverride = -1.0f;
    ctx->roomDimOverride = -1.0f;
    // Roughly ten frames to cross a scene cut, which reads as the glow
    // following the picture rather than flashing with it
    ctx->ambiSmooth = 0.08f;
    // Letterbox detection on, with nothing found yet, so the sample pass starts
    // on the whole frame
    ctx->ambiBarDetect = 1;
    ctx->ambiCrop[0] = 0.0f;
    ctx->ambiCrop[1] = 0.0f;
    ctx->ambiCrop[2] = 1.0f;
    ctx->ambiCrop[3] = 1.0f;
    // No environment logged yet, and cell 0 is a real choice
    ctx->loggedChoice = -1;
    (*env)->GetJavaVM(env, &ctx->vm);
    ctx->activity = (*env)->NewGlobalRef(env, activity);

    if (!initXrInstance(ctx) || !initEgl(ctx) || !initXrSession(ctx) ||
            !initSwapchain(ctx) || !initGl(ctx)) {
        destroyCtx(env, ctx);
        return 0;
    }

    // Optional: a runtime with no controllers, or one that rejects every
    // binding we know, still streams. It just has no pointer.
    if (!initXrInput(ctx)) {
        LOGW("controller input unavailable, pointer off");
        destroyXrInput(ctx);
    }
    else if (!createPointerSwapchain(ctx)) {
        LOGW("pointer swapchain unavailable, the ray will not be drawn");
    }

    LOGEV("OpenXR init complete (cylinder=%d equirect=%d srgbWriteControl=%d maxLayers=%d)",
         ctx->cylinderSupported, ctx->equirectSupported, ctx->srgbWriteControl,
         ctx->maxLayerCount);
    return (jlong)(intptr_t)ctx;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetCaptureDir(JNIEnv* env, jobject thiz,
                                                                jlong handle, jstring dir) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || dir == NULL) {
        return;
    }
    const char* chars = (*env)->GetStringUTFChars(env, dir, NULL);
    if (chars != NULL) {
        strncpy(ctx->captureDir, chars, sizeof(ctx->captureDir) - 1);
        (*env)->ReleaseStringUTFChars(env, dir, chars);
        LOGI("capture dir %s, setprop %s to dump a frame", ctx->captureDir, CAPTURE_PROP);
    }
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetTexId(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return ctx != NULL ? (jint)ctx->oesTexture : 0;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeWaitBeginFrame(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return FRAME_EXIT;
    }

    pollEvents(ctx);

    if (ctx->exitRequested) {
        return FRAME_EXIT;
    }

    if (!ctx->sessionRunning) {
        usleep(10000);
        return FRAME_IDLE;
    }

    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    if (!checkXr(xrWaitFrame(ctx->session, NULL, &frameState), "xrWaitFrame")) {
        return FRAME_EXIT;
    }
    if (!checkXr(xrBeginFrame(ctx->session, NULL), "xrBeginFrame")) {
        return FRAME_EXIT;
    }

    ctx->predictedDisplayTime = frameState.predictedDisplayTime;
    ctx->shouldRender = frameState.shouldRender;
    return FRAME_RENDER;
}

// Whether curved screens are available at all, which is what says if the panel
// should draw its curve row live or greyed out
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetCylinderSupported(JNIEnv* env, jobject thiz,
                                                                       jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return (ctx != NULL && ctx->cylinderSupported) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeDestroy(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL) {
        return;
    }
    destroyCtx(env, ctx);
    LOGI("OpenXR renderer destroyed");
}
