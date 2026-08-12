// OpenXR presentation for the decoded video stream. The decoder feeds a
// SurfaceTexture whose OES texture lives in the EGL context created here.
// Each new video frame is drawn into a single swapchain that the compositor
// shows on a quad (or cylinder) visible to both eyes. No projection layers,
// the compositor does all the reprojection work.

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "moonlight-xr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#define STATS_LOG_INTERVAL_FRAMES 300

// Return codes for waitBeginFrame
#define FRAME_EXIT   -1
#define FRAME_IDLE    0
#define FRAME_RENDER  1

// Synthetic depth patterns for the stereo test path
#define DEPTH_MODE_OFF   0
#define DEPTH_MODE_FLAT  1
#define DEPTH_MODE_RAMP  2
#define DEPTH_MODE_BLOB  3

#define DEPTH_TEX_SIZE 256

typedef struct {
    JavaVM* vm;
    jobject activity;

    EGLDisplay eglDisplay;
    EGLConfig eglConfig;
    EGLContext eglContext;
    EGLSurface eglPbuffer;

    XrInstance instance;
    XrSystemId systemId;
    XrSession session;
    XrSpace localSpace;
    XrSpace viewSpace;

    XrSwapchain swapchain;
    uint32_t swapchainImageCount;
    XrSwapchainImageOpenGLESKHR* swapchainImages;
    int64_t swapchainFormat;

    int videoWidth;
    int videoHeight;

    // Stereo test path. When stereoMode is not OFF the swapchain is double
    // wide and each eye gets its own warped copy of the frame
    int stereoMode;
    int depthDebug;
    GLuint depthTexture;

    GLuint oesTexture;
    GLuint program;
    GLint texMatrixUniform;
    GLint disparityUniform;
    GLuint fbo;

    XrSessionState sessionState;
    int sessionRunning;
    int exitRequested;
    XrTime predictedDisplayTime;
    int shouldRender;
    int everRendered;

    int cylinderSupported;
    int srgbWriteControl;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGlesReqs;

    long statFrames;
    long statTotalNs;
    long statMaxNs;
} XrCtx;

static const char* VERTEX_SRC =
    "#version 300 es\n"
    "in vec4 a_position;\n"
    "in vec4 a_texcoord;\n"
    "out vec2 v_plain;\n"
    "void main() {\n"
    "    gl_Position = a_position;\n"
    "    v_plain = a_texcoord.xy;\n"
    "}\n";

// Gather warp. Each output pixel samples the color frame shifted by a
// disparity derived from the depth map. u_disparity is signed per eye and
// zero in mono, which makes this exactly the old passthrough. The transform
// matrix is applied after the shift since the shift is defined in frame
// space, not in the video driver's transformed space.
static const char* FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_disparity;\n"
    "uniform float u_showDepth;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    float d = texture(u_depth, v_plain).r;\n"
    "    if (u_showDepth > 0.5) {\n"
    "        fragColor = vec4(d, d, d, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 tc = v_plain;\n"
    "    tc.x -= u_disparity * (d - 0.5);\n"
    "    fragColor = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy);\n"
    "}\n";

// Same fullscreen strip as the 2d GL path, x y u v
static const float VERTEX_DATA[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

static long nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static int checkXr(XrResult res, const char* what) {
    if (XR_FAILED(res)) {
        LOGE("%s failed: %d", what, res);
        return 0;
    }
    return 1;
}

static GLuint compileShader(GLenum type, const char* src) {
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

static int initXrInstance(XrCtx* ctx) {
    PFN_xrInitializeLoaderKHR initLoader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&initLoader);
    if (initLoader != NULL) {
        XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        loaderInfo.applicationVM = ctx->vm;
        loaderInfo.applicationContext = ctx->activity;
        initLoader((XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo);
    }

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL);
    XrExtensionProperties* exts = calloc(extCount, sizeof(XrExtensionProperties));
    for (uint32_t i = 0; i < extCount; i++) {
        exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, exts);

    int haveGles = 0, haveAndroidCreate = 0;
    for (uint32_t i = 0; i < extCount; i++) {
        if (!strcmp(exts[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) haveGles = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) haveAndroidCreate = 1;
        if (!strcmp(exts[i].extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) ctx->cylinderSupported = 1;
    }
    free(exts);

    if (!haveGles || !haveAndroidCreate) {
        LOGE("required OpenXR extensions missing (gles=%d androidCreate=%d)", haveGles, haveAndroidCreate);
        return 0;
    }

    const char* enabledExts[3];
    uint32_t enabledCount = 0;
    enabledExts[enabledCount++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    enabledExts[enabledCount++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    if (ctx->cylinderSupported) {
        enabledExts[enabledCount++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
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

    if (!checkXr(xrCreateInstance(&createInfo, &ctx->instance), "xrCreateInstance")) {
        return 0;
    }

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!checkXr(xrGetSystem(ctx->instance, &systemInfo, &ctx->systemId), "xrGetSystem")) {
        return 0;
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
    xrEnumerateSwapchainFormats(ctx->session, formatCount, &formatCount, formats);

    ctx->swapchainFormat = 0;
    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            ctx->swapchainFormat = GL_SRGB8_ALPHA8;
            break;
        }
    }
    if (ctx->swapchainFormat == 0 && formatCount > 0) {
        ctx->swapchainFormat = formats[0];
        LOGW("no SRGB8_ALPHA8 swapchain format, using %lld", (long long)ctx->swapchainFormat);
    }
    free(formats);

    // Stereo renders left and right eye views side by side in one swapchain
    int chainWidth = ctx->stereoMode != DEPTH_MODE_OFF ? ctx->videoWidth * 2 : ctx->videoWidth;

    XrSwapchainCreateInfo swapInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swapInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapInfo.format = ctx->swapchainFormat;
    swapInfo.sampleCount = 1;
    swapInfo.width = chainWidth;
    swapInfo.height = ctx->videoHeight;
    swapInfo.faceCount = 1;
    swapInfo.arraySize = 1;
    swapInfo.mipCount = 1;
    if (!checkXr(xrCreateSwapchain(ctx->session, &swapInfo, &ctx->swapchain), "xrCreateSwapchain")) {
        return 0;
    }

    xrEnumerateSwapchainImages(ctx->swapchain, 0, &ctx->swapchainImageCount, NULL);
    ctx->swapchainImages = calloc(ctx->swapchainImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
    for (uint32_t i = 0; i < ctx->swapchainImageCount; i++) {
        ctx->swapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    if (!checkXr(xrEnumerateSwapchainImages(ctx->swapchain, ctx->swapchainImageCount,
            &ctx->swapchainImageCount, (XrSwapchainImageBaseHeader*)ctx->swapchainImages),
            "enumerate swapchain images")) {
        return 0;
    }

    LOGI("swapchain %dx%d format %lld, %u images (stereo mode %d)", chainWidth, ctx->videoHeight,
         (long long)ctx->swapchainFormat, ctx->swapchainImageCount, ctx->stereoMode);
    return 1;
}

// Builds the hardcoded depth map for the stereo test path. Depth convention:
// 0 far, 1 near, 0.5 sits exactly on the screen plane (zero disparity)
static void fillSyntheticDepth(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;
    unsigned char* buf = malloc(n * n);

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
                case DEPTH_MODE_FLAT:
                default:
                    d = 0.5f;
                    break;
            }
            if (d < 0.0f) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            buf[y * n + x] = (unsigned char)(d * 255.0f + 0.5f);
        }
    }

    glBindTexture(GL_TEXTURE_2D, ctx->depthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, n, n, 0, GL_RED, GL_UNSIGNED_BYTE, buf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(buf);
}

static int initGl(XrCtx* ctx) {
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

    // Sampler units are fixed: color on 0, depth on 1
    glUseProgram(ctx->program);
    glUniform1i(glGetUniformLocation(ctx->program, "u_texture"), 0);
    glUniform1i(glGetUniformLocation(ctx->program, "u_depth"), 1);
    glUniform1f(glGetUniformLocation(ctx->program, "u_showDepth"),
                ctx->depthDebug ? 1.0f : 0.0f);

    glGenTextures(1, &ctx->depthTexture);
    fillSyntheticDepth(ctx);

    glGenTextures(1, &ctx->oesTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->fbo);

    const char* glExts = (const char*)glGetString(GL_EXTENSIONS);
    ctx->srgbWriteControl = glExts != NULL && strstr(glExts, "GL_EXT_sRGB_write_control") != NULL;

    // The video frames are already gamma encoded. With an sRGB swapchain the
    // GPU would encode again on write, so turn that off. Without the
    // extension colors will look washed out and we would need a shader fix.
    if (ctx->swapchainFormat == GL_SRGB8_ALPHA8 && !ctx->srgbWriteControl) {
        LOGW("GL_EXT_sRGB_write_control not available, expect wrong gamma");
    }

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
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                ctx->exitRequested = 1;
                break;
            default:
                break;
        }
    }
}

static void destroyCtx(JNIEnv* env, XrCtx* ctx) {
    if (ctx->swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->swapchain);
    }
    free(ctx->swapchainImages);
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
                                                       jint stereoMode, jboolean depthDebug) {
    XrCtx* ctx = calloc(1, sizeof(XrCtx));
    ctx->videoWidth = width;
    ctx->videoHeight = height;
    ctx->stereoMode = stereoMode;
    ctx->depthDebug = depthDebug;
    ctx->sessionState = XR_SESSION_STATE_UNKNOWN;
    (*env)->GetJavaVM(env, &ctx->vm);
    ctx->activity = (*env)->NewGlobalRef(env, activity);

    if (!initXrInstance(ctx) || !initEgl(ctx) || !initXrSession(ctx) ||
            !initSwapchain(ctx) || !initGl(ctx)) {
        destroyCtx(env, ctx);
        return 0;
    }

    LOGI("OpenXR init complete (cylinder=%d srgbWriteControl=%d)",
         ctx->cylinderSupported, ctx->srgbWriteControl);
    return (jlong)(intptr_t)ctx;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetTexId(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return (jint)ctx->oesTexture;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeWaitBeginFrame(JNIEnv* env, jobject thiz, jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;

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

static void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation) {
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->swapchain, &acquireInfo, &imageIndex), "acquire image")) {
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
    glBindTexture(GL_TEXTURE_2D, ctx->depthTexture);
    glUniformMatrix4fv(ctx->texMatrixUniform, 1, GL_FALSE, texMatrix);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);

    // Mono is a single full width draw with zero disparity. Stereo draws the
    // left eye into the left half and the right eye into the right half,
    // with opposite disparity signs
    int eyes = ctx->stereoMode != DEPTH_MODE_OFF ? 2 : 1;
    for (int eye = 0; eye < eyes; eye++) {
        glViewport(eye * ctx->videoWidth, 0, ctx->videoWidth, ctx->videoHeight);
        float disparity = 0.0f;
        if (eyes == 2) {
            disparity = (eye == 0) ? separation : -separation;
        }
        glUniform1f(ctx->disparityUniform, disparity);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->swapchain, &releaseInfo);

    ctx->everRendered = 1;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeEndFrame(JNIEnv* env, jobject thiz, jlong handle,
                                                           jboolean newFrame, jfloatArray texMatrixArr,
                                                           jfloat distance, jfloat quadWidth,
                                                           jfloat curvature, jboolean headLocked,
                                                           jfloat separation, jboolean eyeSwap) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;

    if (newFrame && ctx->shouldRender) {
        long startNs = nowNs();

        float texMatrix[16];
        (*env)->GetFloatArrayRegion(env, texMatrixArr, 0, 16, texMatrix);
        renderVideoFrame(ctx, texMatrix, separation);

        long elapsed = nowNs() - startNs;
        ctx->statFrames++;
        ctx->statTotalNs += elapsed;
        if (elapsed > ctx->statMaxNs) ctx->statMaxNs = elapsed;
        if (ctx->statFrames == STATS_LOG_INTERVAL_FRAMES) {
            LOGI("XR blit: %ld frames, avg %.2f ms, max %.2f ms",
                 ctx->statFrames, ctx->statTotalNs / (double)ctx->statFrames / 1e6,
                 ctx->statMaxNs / 1e6);
            ctx->statFrames = 0;
            ctx->statTotalNs = 0;
            ctx->statMaxNs = 0;
        }
    }

    float aspect = (float)ctx->videoHeight / (float)ctx->videoWidth;
    XrSpace space = headLocked ? ctx->viewSpace : ctx->localSpace;
    int stereo = ctx->stereoMode != DEPTH_MODE_OFF;

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = ctx->predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerQuad quadLayers[2];
    XrCompositionLayerCylinderKHR cylLayers[2];
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 0;

    if (ctx->everRendered && ctx->shouldRender) {
        int viewCount = stereo ? 2 : 1;
        for (int eye = 0; eye < viewCount; eye++) {
            XrSwapchainSubImage subImage;
            subImage.swapchain = ctx->swapchain;
            // The swap toggle reroutes which half each eye sees. Any stereo
            // inversion bug found later is then depth or warp, not routing
            int half = eyeSwap ? (1 - eye) : eye;
            subImage.imageRect.offset.x = stereo ? half * ctx->videoWidth : 0;
            subImage.imageRect.offset.y = 0;
            subImage.imageRect.extent.width = ctx->videoWidth;
            subImage.imageRect.extent.height = ctx->videoHeight;
            subImage.imageArrayIndex = 0;

            XrEyeVisibility visibility = !stereo ? XR_EYE_VISIBILITY_BOTH :
                    (eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);

            if (curvature > 0.01f && ctx->cylinderSupported) {
                XrCompositionLayerCylinderKHR* cyl = &cylLayers[eye];
                memset(cyl, 0, sizeof(*cyl));
                cyl->type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
                // Radius runs from 4x distance (slightly curved) down to the
                // distance itself (wrapped around the viewer) as curvature rises
                float radius = distance * (1.0f + 3.0f * (1.0f - curvature));
                cyl->eyeVisibility = visibility;
                cyl->subImage = subImage;
                cyl->space = space;
                cyl->pose.orientation.w = 1.0f;
                // Keep the surface at the requested distance
                cyl->pose.position.z = radius - distance;
                cyl->radius = radius;
                cyl->centralAngle = quadWidth / radius;
                cyl->aspectRatio = quadWidth / (quadWidth * aspect);
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)cyl;
            }
            else {
                XrCompositionLayerQuad* quad = &quadLayers[eye];
                memset(quad, 0, sizeof(*quad));
                quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                quad->eyeVisibility = visibility;
                quad->subImage = subImage;
                quad->space = space;
                quad->pose.orientation.w = 1.0f;
                quad->pose.position.z = -distance;
                quad->size.width = quadWidth;
                quad->size.height = quadWidth * aspect;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)quad;
            }
        }
    }

    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    checkXr(xrEndFrame(ctx->session, &endInfo), "xrEndFrame");
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
