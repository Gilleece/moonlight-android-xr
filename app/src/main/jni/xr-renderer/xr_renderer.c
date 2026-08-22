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
#include <sys/system_properties.h>
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

// Pico ships its controller bindings behind an extension. Older headers may
// not have the name, and the runtime may not offer it at all
#ifndef XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME
#define XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_BD_controller_interaction"
#endif

// Beam and cursor art share one small swapchain, beam on top, dot below
#define PTR_TEX_W 64
#define PTR_BEAM_H 256
#define PTR_DOT_H 64
#define PTR_TEX_H (PTR_BEAM_H + PTR_DOT_H)

#define HAND_LEFT  0
#define HAND_RIGHT 1
#define HAND_COUNT 2
// Some headsets aim by looking rather than by pointing. Gaze is a third source
// of a ray, so the pointing code counts it alongside the two hands and
// everything downstream stays the same. Only the hands carry buttons.
#define SRC_GAZE  HAND_COUNT
#define SRC_COUNT (HAND_COUNT + 1)

// Trigger and grip are analog, and a single threshold chatters around the
// crossing, so presses and releases use different ones
#define PRESS_ON   0.65f
#define PRESS_OFF  0.35f
// Thumbstick travel before it counts as a scroll, and how fast a full
// deflection winds the wheel
#define SCROLL_DEADZONE 0.30f
#define SCROLL_CLICKS_PER_SEC 7.5f

// One euro filter on the hit point. A hand at rest still shakes, and at 3 m
// that tremor is several pixels of cursor, so the cutoff drops when the
// pointer is still and rises with speed to keep fast moves from lagging.
#define POINTER_MIN_CUTOFF 1.2f
#define POINTER_BETA 8.0f
#define POINTER_D_CUTOFF 1.0f
// A gap this long means the pointer left the screen or changed hands, and
// filtering across it would slide the cursor in from where it used to be
#define POINTER_RESET_NS 250000000L

// The pointer waits for deliberate movement before it appears, so knocking a
// controller does not throw a laser across the picture, and it goes away again
// once a controller has been put down
#define POINTER_WAKE_SEC 0.5f
#define POINTER_SLEEP_SEC 5.0f
// Metres per second and radians per second. A resting hand manages about a
// tenth of these.
#define POINTER_MOVE_SPEED 0.06f
#define POINTER_TURN_SPEED 0.35f

#define VR_BUTTON_LEFT   0x1
#define VR_BUTTON_RIGHT  0x2
#define VR_BUTTON_MIDDLE 0x4

// Slots in the float array handed back to Java each frame
#define IN_HIT      0
#define IN_U        1
#define IN_V        2
#define IN_BUTTONS  3
#define IN_SCROLL   4
#define IN_POINTER  5
#define IN_POSE_DIRTY 6
// x y z, then the orientation quaternion, then width and cylinder radius
#define IN_POSE     8
// The cell just chosen in the environment grid, or -1
#define IN_PICKER_PICK 17
#define IN_SLOTS    20

// Grab thresholds for the grip, and the range a resize is allowed to reach
#define SCREEN_MIN_WIDTH 0.8f
#define SCREEN_MAX_WIDTH 8.0f

// What the ray is over. Handles only show while hovered, which is how spatial
// panels usually behave: nothing visible until you go looking for it.
#define HOVER_NONE   0
#define HOVER_SCREEN 1
#define HOVER_BAR    2
#define HOVER_CORNER 3

#define GRAB_NONE   0
#define GRAB_MOVE   1
#define GRAB_RESIZE 2

// All as a fraction of screen width, so the handles keep their proportions as
// the screen is resized
#define BAR_WIDTH_FRAC  0.14f
// Height follows the art rather than being picked separately. The two used to
// disagree by 2.5x, which stretched the rounded ends into a slab.
#define BAR_HEIGHT_FRAC (BAR_WIDTH_FRAC * (float)BAR_TEX_H / (float)BAR_TEX_W)
#define BAR_GAP_FRAC    0.035f
#define CORNER_FRAC     0.075f
// Hover zones are bigger than the art, since aiming at a thin bar is fussy
#define HOVER_MARGIN 1.7f
#define CORNER_HOVER 1.5f
// The bar is small on purpose, so its hover zone is proportionally wider
#define BAR_HOVER 2.0f

// Handle art, one small swapchain each so there is no atlas offset convention
// to get wrong
#define BAR_TEX_W 256
#define BAR_TEX_H 24
#define CORNER_TEX_W 64
#define CORNER_TEX_H 64

// Environment picker. A grid of thumbnails drawn in Java and shown as one
// quad, with the hover and selection marks as separate outline quads so
// pointing around the grid never costs an upload.
#define PICKER_COLS 3
#define PICKER_ROWS 2
#define PICKER_CELLS (PICKER_COLS * PICKER_ROWS)
#define PICKER_TEX_W 768
#define PICKER_TEX_H 512
#define PICKER_WIDTH_FRAC 0.55f
#define OUTLINE_TEX 128
// The button that opens it, sitting to the left of the move bar
#define ENV_BUTTON_FRAC 0.048f
#define ENV_GAP_FRAC 0.02f

// The padlock that locks the hands out, on the left edge at eye level. Away
// from the bar on purpose: a hand resting in the lap holding a controller
// points at the bottom of the screen, and that kept lighting the furniture
// down there. Bigger than the env button because while locked it is the only
// thing left to aim at, so it has to be findable without a ray to guide you.
#define LOCK_BUTTON_FRAC 0.09f
#define LOCK_GAP_FRAC 0.025f
#define LOCK_TEX 128

#define HOVER_ENVBUTTON 4
#define HOVER_PICKER    5
// Nothing under the ray, but close enough to the screen to keep drawing it
#define HOVER_HALO      6
#define HOVER_LOCK      7
// How far past each edge that reaches, as a fraction of the screen
#define HALO_FRAC 0.5f
// How far the ray runs when it is aimed at nothing at all, in metres
#define FREE_BEAM_M 4.0f

// Radius of the environment sphere in metres. Finite, so leaning gives the
// room a size instead of it sitting infinitely far off.
#define ENV_RADIUS_M 12.0f

// Return codes for waitBeginFrame
#define FRAME_EXIT   -1
#define FRAME_IDLE    0
#define FRAME_RENDER  1

// Synthetic depth patterns for the stereo test path
#define DEPTH_MODE_OFF   0
#define DEPTH_MODE_FLAT  1
#define DEPTH_MODE_RAMP  2
#define DEPTH_MODE_BLOB  3
// Tints each eye instead of warping, so eye routing can be checked by
// closing one eye rather than by judging depth
#define DEPTH_MODE_EYETEST 4
// Draws a synthetic bar through the warp and reads back where it landed in
// each eye, so the shift direction is measured rather than eyeballed
#define DEPTH_MODE_SHIFTTEST 5
// Real depth from the MiDaS model, run in Java on LiteRT
#define DEPTH_MODE_MODEL 6

#define DEPTH_TEX_SIZE 256

// setprop this to any new value to dump one frame's worth of warp inputs and
// outputs, so shader changes can be tried on captured frames off device
#define CAPTURE_PROP "debug.moonlight.capture"
#define CAPTURE_POLL_FRAMES 30

// Tuning knobs, all live over setprop so a headset session can A/B them
// without a rebuild. Each is an integer percent of the real value.
#define PROP_DEPTH_ALPHA "debug.moonlight.depthalpha"
#define PROP_RANGE_ALPHA "debug.moonlight.rangealpha"
#define PROP_UPSAMPLE "debug.moonlight.upsample"
#define PROP_UPSAMPLE_SIGMA "debug.moonlight.upsamplesigma"
#define PROP_DEPTH_SHARP "debug.moonlight.depthsharp"
#define PROP_OVERLAY "debug.moonlight.overlay"
#define PROP_PASSTHROUGH "debug.moonlight.passthrough"

// Enough for a dozen lines of stats without being big enough to matter
#define OVERLAY_WIDTH 768
#define OVERLAY_HEIGHT 512
#define PROP_OCCLUSION "debug.moonlight.occlusion"
#define PROP_SEPARATION "debug.moonlight.separation"
#define PROP_DISTANCE "debug.moonlight.distance"
#define PROP_SCREEN "debug.moonlight.screen"
#define PROP_CONVERGENCE "debug.moonlight.convergence"
#define PROP_DEPTH_GLOBAL "debug.moonlight.depthglobal"
#define PROP_DEPTH_LOCAL "debug.moonlight.depthlocal"
#define PROP_POINTER_CUTOFF "debug.moonlight.pointercutoff"
#define PROP_POINTER_BETA "debug.moonlight.pointerbeta"
#define PROP_BEAM_WIDTH "debug.moonlight.beamwidth"
#define PROP_POINTER_WAKE "debug.moonlight.pointerwake"
#define PROP_POINTER_SLEEP "debug.moonlight.pointersleep"
#define PROP_ENV_RADIUS "debug.moonlight.envradius"

// Bins for the percentile search over the model output
#define DEPTH_HIST_BINS 512

// Radius of the low pass that splits the depth map into an overall shape and
// the local detail on top of it. About a tenth of the frame.
#define DEPTH_LOWPASS_RADIUS 11

typedef struct { float x, y, z; } Vec3;

// One euro filter: a low pass whose cutoff rises with speed, so a resting
// hand is smoothed hard while a fast sweep is barely delayed
typedef struct {
    int valid;
    float x;
    float dx;
} EuroState;

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

    // Stats overlay. The activity window is not on screen in an immersive
    // session, so the 2d TextView upstream uses is invisible here and the
    // numbers have to go into the scene as their own layer. Keeping it out of
    // the video swapchain means it is never warped, never doubled, and costs
    // no warp time.
    XrSwapchain overlaySwapchain;
    uint32_t overlayImageCount;
    XrSwapchainImageOpenGLESKHR* overlayImages;
    int overlayHasContent;
    int overlayVisible;

    int videoWidth;
    int videoHeight;

    // Stereo test path. When stereoMode is not OFF the swapchain is double
    // wide and each eye gets its own warped copy of the frame
    int stereoMode;
    int depthDebug;
    // Double buffered: the frame loop samples one while the depth thread
    // writes the other, so neither ever waits on the other
    GLuint depthTextures[2];
    volatile int depthReadIndex;

    // Second context in the same share group for the depth thread. Inference
    // takes longer than a display frame, so it cannot run on the frame loop
    EGLContext depthContext;
    EGLSurface depthPbuffer;

    // Depth model staging. The frame is downscaled to DEPTH_TEX_SIZE on the
    // GPU, read back, run through the model in Java, and the result goes
    // back up into the depth texture
    GLuint downscaleProgram;
    GLint downscaleTexMatrixUniform;
    GLuint downscaleTexture;
    GLuint downscaleFbo;
    unsigned char* readbackBuf;
    float* modelInput;
    float* modelOutput;
    unsigned char* depthUploadBuf;

    // Temporal smoothing. The normalization range is smoothed separately from
    // the map itself: a single outlier pixel moving the min or max used to
    // shift the whole mapping, which pumps the entire image.
    float* depthEma;
    float* depthLow;
    float* depthScratch;
    float* depthColSums;
    float depthGlobal;
    float depthLocal;
    int depthEmaValid;
    float smoothLo;
    float smoothHi;
    int rangeValid;
    float depthAlpha;
    float rangeAlpha;

    // Edge aware upsample of the depth map, quarter of the video size
    GLuint upsampleProgram;
    GLint upsampleTexMatrixUniform;
    GLint upsampleSigmaUniform;
    GLint upsampleSharpUniform;
    float depthSharp;
    GLuint upsampleTexture;
    GLuint upsampleFbo;
    int upsampleWidth;
    int upsampleHeight;
    int upsampleEnabled;
    float upsampleSigmaR;

    // Occlusion aware offset map, both eyes packed into rg
    GLuint offsetProgram;
    GLint offsetDispUniform;
    GLint offsetConvUniform;
    GLuint offsetTexture;
    GLuint offsetFbo;
    int occlusionEnabled;
    float convergence;
    float separationOverride;
    float distanceOverride;
    float screenOverride;

    GLuint oesTexture;
    GLuint program;
    GLint texMatrixUniform;
    GLint disparityUniform;
    GLint tintUniform;
    GLint barTestUniform;
    GLint occlusionUniform;
    GLint eyeIndexUniform;
    GLint convergenceUniform;
    GLint dispTexelsUniform;
    GLint lowResWidthUniform;
    GLint frameWidthUniform;
    GLuint fbo;
    int barTestFramesLogged;

    // Frame capture for offline shader work
    char captureDir[256];
    char captureTag[PROP_VALUE_MAX];
    char lastCaptureTag[PROP_VALUE_MAX];
    int captureRequested;
    long capturePollCounter;

    XrSessionState sessionState;
    int sessionRunning;
    int exitRequested;
    XrTime predictedDisplayTime;
    int shouldRender;
    int everRendered;

    int cylinderSupported;
    int equirectSupported;

    // 360 photo shown behind everything when passthrough is off. An equirect
    // layer, so the compositor draws the environment and we still have no
    // projection layer and no geometry.
    XrSwapchain backgroundSwapchain;
    uint32_t backgroundImageCount;
    XrSwapchainImageOpenGLESKHR* backgroundImages;
    int backgroundWidth;
    int backgroundHeight;
    int backgroundReady;
    int backgroundEnabled;
    float envRadius;
    int srgbWriteControl;
    // Passthrough is just an environment blend mode: with alpha blend the
    // runtime shows the room wherever our layers do not cover. Both headsets
    // offer it, but Meta only turns the cameras on if the manifest asks.
    int alphaBlendSupported;
    int passthrough;
    // Hand tracking arrives as another interaction profile rather than as a
    // separate input path, so the pointer does not know the difference
    int handsEnabled;
    int handInteraction;
    int msftHandInteraction;
    XrPath handProfile;
    XrPath msftHandProfile;
    int handTracking;
    int handClickOk;
    // Looking at something instead of pointing at it. Lowest priority of the
    // three, so a controller or a hand always wins when one is aiming.
    int eyeGaze;
    int gazeEnabled;
    XrAction gazeAction;
    int lastSnapshot;
    // Reading the joints directly, because a pinch is not always offered as an
    // input. Thumb to fingertip is the whole of it.
    int jointTracking;
    XrHandTrackerEXT handTrackers[HAND_COUNT];
    int jointPinch[HAND_COUNT];
    Vec3 pinchPoint[HAND_COUNT];
    int pinchPointValid[HAND_COUNT];
    // A ray built out of the joints, for runtimes that track hands but do not
    // offer a pointer pose of their own
    XrPosef handRay[HAND_COUNT];
    int handRayValid[HAND_COUNT];
    PFN_xrCreateHandTrackerEXT pfnCreateHandTracker;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker;
    PFN_xrLocateHandJointsEXT pfnLocateHandJoints;
    int usingHands[SRC_COUNT];
    // A pinch that woke the pointer is not also a click, so it is swallowed
    // until the hand opens again
    int pinchSwallowed[SRC_COUNT];
    // Hands locked out for the session, so a gamepad can be used without a
    // stray pinch clicking the desktop or dragging the screen around. The
    // padlock is the one thing they can still reach. Controllers are never
    // affected, and it starts off every session.
    int handsLocked;
    int lockHot;
    // The pinch has to start on the padlock. Sweeping onto it with one already
    // held would otherwise read as a press, because a locked hand has its
    // trigger cleared every frame and so arrives looking like a fresh edge.
    int lockArmed[SRC_COUNT];
    XrSwapchain lockSwapchain;
    XrSwapchain unlockSwapchain;
    uint32_t lockImageCount;
    uint32_t unlockImageCount;
    XrSwapchainImageOpenGLESKHR* lockImages;
    XrSwapchainImageOpenGLESKHR* unlockImages;
    int lockArtReady;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGlesReqs;

    // Controller input. The aim ray is intersected with the screen and the hit
    // point drives the host mouse, so the PC sees an ordinary absolute mouse
    XrActionSet actionSet;
    XrAction aimAction;
    XrAction triggerAction;
    XrAction rightClickAction;
    XrAction middleClickAction;
    XrAction scrollAction;
    XrAction grabAction;
    XrAction toggleAction;
    XrSpace aimSpaces[SRC_COUNT];
    XrPath handPaths[HAND_COUNT];
    int inputReady;
    int picoInteraction;
    // Pointing is a per session toggle on top of the preference, since
    // absolute positions fight any game that does its own mouse look
    int pointerOn;
    int togglePrev;
    int triggerDown[SRC_COUNT];
    // Rising edges, so a button already held when the ray wanders onto a handle
    // does not grab it. Dragging a window on the host desktop past the edge of
    // the picture would otherwise turn into a resize.
    int triggerEdge[SRC_COUNT];
    // Indexed by the chosen source, and gaze has no grip, so it needs the
    // extra slot even though nothing ever writes to it
    int gripEdge[SRC_COUNT];
    int grabByTrigger;
    int buttonsDown;
    float scrollCarry;
    long lastInputNs;

    // One euro filter state for the hit point, per axis
    EuroState filterU;
    EuroState filterV;
    float pointerMinCutoff;
    float pointerBeta;
    long lastHitNs;
    int lastHand;

    // Movement gate. The pointer only appears after the controller has been
    // moved deliberately, and disappears once it has been still a while.
    int poseSeen[SRC_COUNT];
    XrPosef lastAim[SRC_COUNT];
    float movingFor;
    float stillFor;
    int pointerAwake;
    float pointerWake;
    float pointerSleep;

    // Laser. Two tiny quad layers rather than a projection layer: the whole
    // renderer draws nothing per frame for this, the compositor places it
    XrSwapchain pointerSwapchain;
    uint32_t pointerImageCount;
    XrSwapchainImageOpenGLESKHR* pointerImages;
    int pointerArtReady;
    int beamVisible;
    // Ray drawn with nothing under it, so there is no cursor to go with it
    int beamFree;
    // Aimed by the eyes, so there is a cursor but no ray
    int beamGaze;
    XrVector3f beamStart;
    XrVector3f beamEnd;
    XrVector3f headPos;
    XrQuaternionf screenOrientation;
    float beamWidth;

    // Where the screen actually is. Seeded from the distance and width
    // preferences and then owned by the grab, so moving it does not fight the
    // sliders. Touching either slider puts it back under their control.
    XrPosef screenPose;
    float screenWidth;
    float screenRadius;
    int placementValid;
    int sliderSeen;
    float lastDistance;
    float lastQuadWidth;

    int grabDown[SRC_COUNT];
    int grabMode;
    int grabHand;
    float grabU, grabV;
    XrPosef grabAim;
    XrPosef grabScreen;
    float grabWidth;
    float grabHeight;
    float grabRadius;
    // Resize works against the corner opposite the one being dragged, which
    // stays put, and along the diagonal it started on
    float grabOppX, grabOppY;
    float grabDiagX, grabDiagY;
    int poseDirty;

    // Hover state, read by the frame loop to decide which handle to draw
    int hoverKind;
    int hoverCorner;
    XrSwapchain barSwapchain;
    XrSwapchain cornerSwapchain;
    uint32_t barImageCount;
    uint32_t cornerImageCount;
    XrSwapchainImageOpenGLESKHR* barImages;
    XrSwapchainImageOpenGLESKHR* cornerImages;
    int handleArtReady;

    XrSwapchain pickerSwapchain;
    XrSwapchain envButtonSwapchain;
    XrSwapchain outlineSwapchain;
    uint32_t pickerImageCount;
    uint32_t envButtonImageCount;
    uint32_t outlineImageCount;
    XrSwapchainImageOpenGLESKHR* pickerImages;
    XrSwapchainImageOpenGLESKHR* envButtonImages;
    XrSwapchainImageOpenGLESKHR* outlineImages;
    int pickerReady;
    int envButtonReady;
    int outlineReady;
    int pickerOpen;
    int pickerHover;
    int pickerChoice;
    int pickerPick;
    int envButtonHot;

    long statFrames;
    long statTotalNs;
    long statMaxNs;

    // Real GPU time for the warp passes. The wall clock around the draw calls
    // only ever measured how long submission took, since nothing waits on the
    // GPU, so it read about 0.1 ms no matter what the shaders did.
    int timerSupported;
    GLuint timerQueries[2];
    int timerSlot;
    int timerPending[2];
    // A query whose result never lands would wedge the pair forever, since
    // the slot only flips once the outstanding one is collected
    int timerPendingFrames[2];
    long gpuTotalNs;
    long gpuMaxNs;
    long gpuSamples;

    // Separate accumulator so reading the number for the overlay does not
    // disturb the logcat cadence
    long overlayGpuTotalNs;
    long overlayGpuSamples;
} XrCtx;

typedef void (*PFNGENQUERIESEXT)(GLsizei, GLuint*);
typedef void (*PFNBEGINQUERYEXT)(GLenum, GLuint);
typedef void (*PFNENDQUERYEXT)(GLenum);
typedef void (*PFNGETQUERYOBJECTUIVEXT)(GLuint, GLenum, GLuint*);
typedef void (*PFNGETQUERYOBJECTUI64VEXT)(GLuint, GLenum, GLuint64*);

static PFNGENQUERIESEXT pfnGenQueries;
static PFNBEGINQUERYEXT pfnBeginQuery;
static PFNENDQUERYEXT pfnEndQuery;
static PFNGETQUERYOBJECTUIVEXT pfnGetQueryObjectuiv;
static PFNGETQUERYOBJECTUI64VEXT pfnGetQueryObjectui64v;

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif

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
    "uniform sampler2D u_offsets;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_disparity;\n"
    "uniform float u_showDepth;\n"
    "uniform float u_barTest;\n"
    "uniform vec3 u_tint;\n"
    "uniform float u_occlusion;\n"
    "uniform float u_eyeIndex;\n"
    "uniform float u_convergence;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_lowResWidth;\n"
    "uniform float u_frameWidth;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    float d = texture(u_depth, v_plain).a;\n"
    "    if (u_showDepth > 0.5) {\n"
    "        fragColor = vec4(d, d, d, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 tc = v_plain;\n"
    "    if (u_occlusion > 0.5) {\n"
    // The offset map already picked the right surface. All that is left is
    // the exact position on it, which the low resolution search only knew to
    // within a texel, and that quantization stair steps along a diagonal
    // silhouette. Two Newton steps against the full resolution depth settle
    // it to well under a pixel.
    "        int reach = int(ceil(abs(u_dispTexels)\n"
    "                        * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "        vec2 enc = texture(u_offsets, v_plain).rg;\n"
    "        float off = (u_eyeIndex < 0.5 ? enc.r : enc.g) - 0.5;\n"
    "        tc.x = v_plain.x + off * 2.0 * float(reach) / u_lowResWidth;\n"
    "        float h = 1.0 / u_frameWidth;\n"
    "        for (int i = 0; i < 2; i++) {\n"
    "            float d0 = texture(u_depth, vec2(tc.x, v_plain.y)).a;\n"
    "            float dm = texture(u_depth, vec2(tc.x - h, v_plain.y)).a;\n"
    "            float dp = texture(u_depth, vec2(tc.x + h, v_plain.y)).a;\n"
    "            float e = (tc.x - v_plain.x) + u_disparity * (d0 - u_convergence);\n"
    "            float slope = 1.0 + u_disparity * (dp - dm) / (2.0 * h);\n"
    "            if (abs(slope) < 0.25) {\n"
    "                slope = 0.25;\n"
    "            }\n"
    "            tc.x -= clamp(e / slope, -4.0 * h, 4.0 * h);\n"
    "        }\n"
    "    }\n"
    "    else {\n"
    "        tc.x -= u_disparity * (d - u_convergence);\n"
    "    }\n"
    "    if (u_barTest > 0.5) {\n"
    "        float b = 1.0 - step(0.004, abs(tc.x - 0.5));\n"
    "        fragColor = vec4(b, b, b, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    fragColor = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy);\n"
    "    fragColor.rgb *= u_tint;\n"
    "}\n";

// Joint bilateral upsample of the depth map. The model output is 256x256
// against a 4K frame, so one depth texel covers a 15x8 block and every depth
// boundary reaches the warp as a 15 pixel ramp. That ramp is the halo: it
// shears whatever colour happens to sit under it.
//
// Each output pixel weights the 5x5 low resolution depth neighbourhood by how
// closely each neighbour's colour matches the colour here, so the depth edge
// snaps to the colour edge instead of straddling it. Measured on a captured
// frame this takes the edge from 15 px to 5 px, which is the resolution limit
// of a 256x256 source rather than of this filter.
//
// The guide rides in the rgb of the depth texture, so it is by construction
// the same frame the depth was inferred from. u_sigmaR trades edge snapping
// against depth detail invented out of colour texture: grass and carpet will
// speckle if it is set too tight.
static const char* UPSAMPLE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_sigmaR;\n"
    "uniform float u_sharp;\n"
    "out vec4 fragColor;\n"
    "const float N = 256.0;\n"
    "const float SIGMA_S = 1.5;\n"
    "const float FLAT = 0.05;\n"
    "void main() {\n"
    "    vec3 hi = texture(u_texture, (u_texmatrix * vec4(v_plain, 0.0, 1.0)).xy).rgb;\n"
    "    vec2 lp = v_plain * N - 0.5;\n"
    "    ivec2 base = ivec2(floor(lp));\n"
    "    float num = 0.0;\n"
    "    float den = 0.0;\n"
    "    float dlo = 1.0;\n"
    "    float dhi = 0.0;\n"
    "    for (int dy = -2; dy <= 2; dy++) {\n"
    "        for (int dx = -2; dx <= 2; dx++) {\n"
    "            ivec2 q = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(int(N) - 1));\n"
    "            vec4 s = texelFetch(u_depth, q, 0);\n"
    "            vec2 off = vec2(q) - lp;\n"
    "            float ws = exp(-dot(off, off) / (2.0 * SIGMA_S * SIGMA_S));\n"
    "            vec3 cd = hi - s.rgb;\n"
    "            float wr = exp(-dot(cd, cd) / (2.0 * u_sigmaR * u_sigmaR));\n"
    "            float w = ws * wr;\n"
    "            num += w * s.a;\n"
    "            den += w;\n"
    "            dlo = min(dlo, s.a);\n"
    "            dhi = max(dhi, s.a);\n"
    "        }\n"
    "    }\n"
    "    float d = num / max(den, 1e-6);\n"
    // A soft depth ramp across a silhouette spreads the disocclusion over the
    // width of the ramp, and that band is the smear. Pushing each texel to
    // whichever side of the local range it is nearer turns the ramp back into
    // a step, using the min and max of taps already read. Flat neighbourhoods
    // are left alone, so only boundaries move.
    "    float span = dhi - dlo;\n"
    "    if (u_sharp > 0.0 && span >= FLAT) {\n"
    "        float u = clamp((d - dlo) / max(span, 1e-6), 0.0, 1.0);\n"
    "        float snapped = dlo + span / (1.0 + exp(-24.0 * (u - 0.5)));\n"
    "        d = mix(d, snapped, u_sharp);\n"
    "    }\n"
    "    fragColor = vec4(d);\n"
    "}\n";

// Inverts the warp properly, once per frame for both eyes, at the same
// quarter resolution as the depth map.
//
// A source pixel at offset t from this one lands here with error
//     e(t) = t + disp * (d(here + t) - convergence)
// so every zero crossing of e is a source that genuinely lands on this pixel.
// Sampling depth at the destination, which is what the warp did before, is
// only right where depth is flat; at a depth step it is wrong by most of the
// disparity range, which is 57 px at 4K, and that is the smearing. More than
// one crossing means two surfaces compete for this pixel, and the nearest one
// wins, which is what occlusion means.
//
// The whole search span is only about nine texels at this resolution, so the
// exhaustive version is affordable. Both eyes share the depth reads.
static const char* OFFSET_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform sampler2D u_depth;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_convergence;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    ivec2 sz = textureSize(u_depth, 0);\n"
    "    int x = int(gl_FragCoord.x);\n"
    "    int y = int(gl_FragCoord.y);\n"
    "    int reach = int(ceil(abs(u_dispTexels)\n"
    "                    * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "    vec2 result = vec2(0.0);\n"
    "    for (int eye = 0; eye < 2; eye++) {\n"
    "        float disp = (eye == 0) ? u_dispTexels : -u_dispTexels;\n"
    "        float here = texelFetch(u_depth, ivec2(x, y), 0).a;\n"
    "        float bestD = -1.0;\n"
    "        float bestOff = -disp * (here - u_convergence);\n"
    "        float pd = texelFetch(u_depth,\n"
    "                ivec2(clamp(x - reach, 0, sz.x - 1), y), 0).a;\n"
    "        float pe = float(-reach) + disp * (pd - u_convergence);\n"
    "        for (int t = -reach + 1; t <= reach; t++) {\n"
    "            float cd = texelFetch(u_depth,\n"
    "                    ivec2(clamp(x + t, 0, sz.x - 1), y), 0).a;\n"
    "            float ce = float(t) + disp * (cd - u_convergence);\n"
    "            float span = ce - pe;\n"
    "            if (pe * ce <= 0.0 && abs(span) > 1e-6) {\n"
    "                float f = clamp(-pe / span, 0.0, 1.0);\n"
    "                float rd = pd + f * (cd - pd);\n"
    "                if (rd > bestD) {\n"
    "                    bestD = rd;\n"
    "                    bestOff = float(t - 1) + f;\n"
    "                }\n"
    "            }\n"
    "            pd = cd;\n"
    "            pe = ce;\n"
    "        }\n"
    "        result[eye] = bestOff;\n"
    "    }\n"
    "    fragColor = vec4(result / (2.0 * float(reach)) + 0.5, 0.0, 1.0);\n"
    "}\n";

// Feeds the depth model. The video is far larger than 256x256, so a single
// bilinear tap per output pixel aliases badly and the depth map crawls with
// it. A 4x4 box over each destination pixel is still nothing on this GPU.
static const char* DOWNSCALE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        for (int x = 0; x < 4; x++) {\n"
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 / 256.0);\n"
    "            vec2 tc = v_plain + off;\n"
    "            sum += texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
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
    LOGI("runtime offers %u OpenXR extensions", extCount);
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

    const char* enabledExts[9];
    uint32_t enabledCount = 0;
    enabledExts[enabledCount++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    enabledExts[enabledCount++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    if (ctx->cylinderSupported) {
        enabledExts[enabledCount++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
    }
    if (ctx->picoInteraction) {
        enabledExts[enabledCount++] = XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME;
    }
    if (ctx->equirectSupported) {
        enabledExts[enabledCount++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME;
    }
    if (ctx->handInteraction) {
        enabledExts[enabledCount++] = XR_EXT_HAND_INTERACTION_EXTENSION_NAME;
    }
    // Some runtimes will not honour the hand interaction profile unless the
    // tracking extension is enabled next to it
    if (ctx->handTracking) {
        enabledExts[enabledCount++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
    }
    if (ctx->eyeGaze) {
        enabledExts[enabledCount++] = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    }
    if (ctx->msftHandInteraction) {
        enabledExts[enabledCount++] = XR_MSFT_HAND_INTERACTION_EXTENSION_NAME;
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

    uint32_t blendModeCount = 0;
    xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     0, &blendModeCount, NULL);
    if (blendModeCount > 0) {
        XrEnvironmentBlendMode* modes = calloc(blendModeCount, sizeof(XrEnvironmentBlendMode));
        xrEnumerateEnvironmentBlendModes(ctx->instance, ctx->systemId,
                                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         blendModeCount, &blendModeCount, modes);
        for (uint32_t i = 0; i < blendModeCount; i++) {
            LOGI("environment blend mode %u available", modes[i]);
            if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                ctx->alphaBlendSupported = 1;
            }
        }
        free(modes);
    }
    LOGI("passthrough %s", ctx->alphaBlendSupported ? "available" : "not offered by this runtime");

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
        LOGI("eye gaze %s", ctx->eyeGaze ? "available" : "offered but not supported by this system");
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
        LOGI("hand joints %s", ctx->handTracking ? "available" : "not supported by this system");
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

    XrSwapchainCreateInfo overlayInfo = swapInfo;
    overlayInfo.width = OVERLAY_WIDTH;
    overlayInfo.height = OVERLAY_HEIGHT;
    if (checkXr(xrCreateSwapchain(ctx->session, &overlayInfo, &ctx->overlaySwapchain),
                "create overlay swapchain")) {
        xrEnumerateSwapchainImages(ctx->overlaySwapchain, 0, &ctx->overlayImageCount, NULL);
        ctx->overlayImages = calloc(ctx->overlayImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->overlayImageCount; i++) {
            ctx->overlayImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->overlaySwapchain, ctx->overlayImageCount,
                                   &ctx->overlayImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->overlayImages);
    }
    else {
        // The stream is worth more than the stats, so carry on without it
        ctx->overlaySwapchain = XR_NULL_HANDLE;
    }

    return 1;
}

// Builds the hardcoded depth map for the stereo test path. Depth convention:
// 0 far, 1 near, 0.5 sits exactly on the screen plane (zero disparity)
static void fillSyntheticDepth(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;
    // RGBA throughout: depth in alpha, guide colour in rgb. The synthetic
    // patterns have no guide, so it stays neutral and the upsample falls back
    // to a plain blur on them.
    unsigned char* buf = malloc((size_t)n * n * 4);

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
}

static int linkProgram(GLuint* out, const char* fragmentSrc, const char* what) {
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

// GL side of the depth model path: the downscale target the frame is
// rendered into, and the staging buffers it is read back through
static int initDepthModel(XrCtx* ctx) {
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

    if (glExts != NULL && strstr(glExts, "GL_EXT_disjoint_timer_query") != NULL) {
        pfnGenQueries = (PFNGENQUERIESEXT)eglGetProcAddress("glGenQueriesEXT");
        pfnBeginQuery = (PFNBEGINQUERYEXT)eglGetProcAddress("glBeginQueryEXT");
        pfnEndQuery = (PFNENDQUERYEXT)eglGetProcAddress("glEndQueryEXT");
        pfnGetQueryObjectuiv = (PFNGETQUERYOBJECTUIVEXT)eglGetProcAddress("glGetQueryObjectuivEXT");
        pfnGetQueryObjectui64v =
                (PFNGETQUERYOBJECTUI64VEXT)eglGetProcAddress("glGetQueryObjectui64vEXT");
        if (pfnGenQueries != NULL && pfnBeginQuery != NULL && pfnEndQuery != NULL &&
                pfnGetQueryObjectuiv != NULL && pfnGetQueryObjectui64v != NULL) {
            pfnGenQueries(2, ctx->timerQueries);
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

    if (ctx->stereoMode == DEPTH_MODE_MODEL) {
        if (!initDepthModel(ctx) || !initUpsample(ctx)) {
            return 0;
        }
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

// A pinch is how these headsets click, but it is not always offered as an
// input to bind to. The joints always are, so it is measured here instead:
// thumb tip to index tip, with a gap between the closing and opening distances
// so a hand held near the threshold does not chatter.
#define PINCH_ON_M  0.020f
#define PINCH_OFF_M 0.032f

static void initJointTracking(XrCtx* ctx) {
    if (!ctx->handTracking) {
        return;
    }
    if (XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrCreateHandTrackerEXT",
                                        (PFN_xrVoidFunction*)&ctx->pfnCreateHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrDestroyHandTrackerEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnDestroyHandTracker))
            || XR_FAILED(xrGetInstanceProcAddr(ctx->instance, "xrLocateHandJointsEXT",
                                               (PFN_xrVoidFunction*)&ctx->pfnLocateHandJoints))
            || ctx->pfnCreateHandTracker == NULL || ctx->pfnLocateHandJoints == NULL) {
        LOGW("hand joint entry points missing");
        ctx->jointTracking = 0;
        return;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrHandTrackerCreateInfoEXT info = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
        info.hand = h == HAND_LEFT ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!checkXr(ctx->pfnCreateHandTracker(ctx->session, &info, &ctx->handTrackers[h]),
                     "create hand tracker")) {
            ctx->handTrackers[h] = XR_NULL_HANDLE;
            return;
        }
    }
    ctx->jointTracking = 1;
    LOGI("reading hand joints for pinch");
}

// Which kind of thing is driving each hand. Hands are never still enough for
// the movement gate to mean anything, so they wake the pointer a different way
// and need to be told apart from controllers.
static void refreshInputSource(XrCtx* ctx) {
    if (ctx->session == XR_NULL_HANDLE || !ctx->inputReady) {
        return;
    }
    for (int h = 0; h < HAND_COUNT; h++) {
        XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(ctx->session, ctx->handPaths[h], &state))) {
            continue;
        }
        // Without a pinch bound there is nothing to wake the pointer with, so
        // those hands stay on the movement gate rather than becoming unusable
        int hands = ctx->handClickOk && state.interactionProfile != XR_NULL_PATH
                && (state.interactionProfile == ctx->handProfile
                    || state.interactionProfile == ctx->msftHandProfile);
        if (hands != ctx->usingHands[h]) {
            LOGI("hand %d is now driven by %s", h, hands ? "hand tracking" : "a controller");
        }
        ctx->usingHands[h] = hands;
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

static Vec3 vecSub(Vec3 a, Vec3 b) {
    Vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z };
    return r;
}

static XrQuaternionf quatConj(XrQuaternionf q) {
    XrQuaternionf r = { -q.x, -q.y, -q.z, q.w };
    return r;
}

static XrQuaternionf quatMul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

// Repeated products drift off the unit sphere and the compositor is entitled
// to reject that
static XrQuaternionf quatNorm(XrQuaternionf q) {
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-6f) {
        XrQuaternionf id = { 0.0f, 0.0f, 0.0f, 1.0f };
        return id;
    }
    q.x /= len;
    q.y /= len;
    q.z /= len;
    q.w /= len;
    return q;
}

static Vec3 quatRotate(XrQuaternionf q, Vec3 v) {
    // v + w * (2 * cross(q.xyz, v)) + cross(q.xyz, 2 * cross(q.xyz, v))
    Vec3 u = { q.x, q.y, q.z };
    Vec3 t = { 2.0f * (u.y * v.z - u.z * v.y),
               2.0f * (u.z * v.x - u.x * v.z),
               2.0f * (u.x * v.y - u.y * v.x) };
    Vec3 r = { v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
               v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
               v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
    return r;
}

static float euroAlpha(float cutoff, float dt) {
    float tau = 1.0f / (2.0f * (float)M_PI * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

static float euroFilter(EuroState* s, float x, float dt, float minCutoff, float beta) {
    if (!s->valid || dt <= 0.0f) {
        s->valid = 1;
        s->x = x;
        s->dx = 0.0f;
        return x;
    }
    float dx = (x - s->x) / dt;
    s->dx += euroAlpha(POINTER_D_CUTOFF, dt) * (dx - s->dx);
    float cutoff = minCutoff + beta * fabsf(s->dx);
    s->x += euroAlpha(cutoff, dt) * (x - s->x);
    return s->x;
}

// Rotation whose local axes are the three given unit vectors. Used to stand a
// quad layer up along the beam while keeping its face toward the viewer.
static XrQuaternionf quatFromBasis(Vec3 x, Vec3 y, Vec3 z) {
    float m[3][3] = {
        { x.x, y.x, z.x },
        { x.y, y.y, z.y },
        { x.z, y.z, z.z },
    };
    float trace = m[0][0] + m[1][1] + m[2][2];
    XrQuaternionf q;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

static Vec3 vecNorm(Vec3 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) {
        Vec3 zero = { 0.0f, 0.0f, 0.0f };
        return zero;
    }
    Vec3 r = { v.x / len, v.y / len, v.z / len };
    return r;
}

static Vec3 vecCross(Vec3 a, Vec3 b) {
    Vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}

static XrPath toPath(XrCtx* ctx, const char* str) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(ctx->instance, str, &path);
    return path;
}

static XrAction makeAction(XrCtx* ctx, XrActionType type, const char* name, const char* label) {
    XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
    info.actionType = type;
    strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = HAND_COUNT;
    info.subactionPaths = ctx->handPaths;

    XrAction action = XR_NULL_HANDLE;
    if (!checkXr(xrCreateAction(ctx->actionSet, &info, &action), name)) {
        return XR_NULL_HANDLE;
    }
    return action;
}

// One unsupported path rejects a whole profile, so the full set is offered
// first and a runtime that does not recognise this controller falls back to
// aim and trigger, which every profile has.
static void suggestBindings(XrCtx* ctx, const char* profile, int full) {
    XrActionSuggestedBinding b[16];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };
    // x and y on the left controller, a and b on the right
    static const char* rightClick[HAND_COUNT] = { "input/x/click", "input/a/click" };
    static const char* middleClick[HAND_COUNT] = { "input/y/click", "input/b/click" };
    int simple = strstr(profile, "/khr/") != NULL;

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/input/aim/pose", hands[h]);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h],
                 simple ? "input/select/click" : "input/trigger/value");
        b[n].action = ctx->triggerAction;
        b[n++].binding = toPath(ctx, path);

        if (!full || simple) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", hands[h], rightClick[h]);
        b[n].action = ctx->rightClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/%s", hands[h], middleClick[h]);
        b[n].action = ctx->middleClickAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick", hands[h]);
        b[n].action = ctx->scrollAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/thumbstick/click", hands[h]);
        b[n].action = ctx->toggleAction;
        b[n++].binding = toPath(ctx, path);

        snprintf(path, sizeof(path), "%s/input/squeeze/value", hands[h]);
        b[n].action = ctx->grabAction;
        b[n++].binding = toPath(ctx, path);
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    XrResult res = xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
    if (XR_SUCCEEDED(res)) {
        LOGI("bindings accepted for %s (%s)", profile, full ? "full" : "reduced");
    }
    else if (full) {
        LOGW("full bindings rejected for %s (%d), trying aim and trigger only", profile, res);
        suggestBindings(ctx, profile, 0);
    }
    else {
        LOGW("bindings rejected for %s (%d)", profile, res);
    }
}

// Hands come in through the same actions the controllers use, so everything
// downstream of here treats them identically: same ray, same handles, same
// picker. Only the paths differ, which is why this is its own function rather
// than another flag on the one above.
static XrResult trySuggestHands(XrCtx* ctx, const char* profile, const char* aim,
                                const char* click, const char* grasp) {
    XrActionSuggestedBinding b[6];
    uint32_t n = 0;
    static const char* hands[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };

    for (int h = 0; h < HAND_COUNT; h++) {
        char path[XR_MAX_PATH_LENGTH];

        snprintf(path, sizeof(path), "%s/%s", hands[h], aim);
        b[n].action = ctx->aimAction;
        b[n++].binding = toPath(ctx, path);

        if (click != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], click);
            b[n].action = ctx->triggerAction;
            b[n++].binding = toPath(ctx, path);
        }

        if (grasp != NULL) {
            snprintf(path, sizeof(path), "%s/%s", hands[h], grasp);
            b[n].action = ctx->grabAction;
            b[n++].binding = toPath(ctx, path);
        }
    }

    XrInteractionProfileSuggestedBinding suggest = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggest.interactionProfile = toPath(ctx, profile);
    suggest.countSuggestedBindings = n;
    suggest.suggestedBindings = b;

    return xrSuggestInteractionProfileBindings(ctx->instance, &suggest);
}

// Runtimes that offer the hand profile do not all implement every input in it,
// and one unsupported path throws out the whole suggestion. So the inputs are
// offered up in falling order of usefulness until a set is accepted. Returns
// whether a pinch ended up bound, since without one the hands cannot wake the
// pointer and are better left to the movement gate.
static int suggestHandBindings(XrCtx* ctx, const char* profile, const char* aim,
                               const char* const* clicks, int clickCount,
                               const char* grasp) {
    XrResult res = XR_SUCCESS;
    for (int c = 0; c < clickCount; c++) {
        if (grasp != NULL) {
            res = trySuggestHands(ctx, profile, aim, clicks[c], grasp);
            if (XR_SUCCEEDED(res)) {
                LOGI("hand bindings accepted for %s (%s and grasp)", profile, clicks[c]);
                return 1;
            }
        }
        res = trySuggestHands(ctx, profile, aim, clicks[c], NULL);
        if (XR_SUCCEEDED(res)) {
            LOGI("hand bindings accepted for %s (%s)", profile, clicks[c]);
            return 1;
        }
    }
    res = trySuggestHands(ctx, profile, aim, NULL, NULL);
    if (XR_SUCCEEDED(res)) {
        LOGW("only the aim pose bound for %s, so hands cannot click", profile);
        return 0;
    }
    LOGW("hand bindings rejected for %s, even the aim pose alone (%d)", profile, res);
    return 0;
}

static int initXrInput(XrCtx* ctx) {
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "moonlight", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "Moonlight", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!checkXr(xrCreateActionSet(ctx->instance, &setInfo, &ctx->actionSet), "create action set")) {
        return 0;
    }

    ctx->handPaths[HAND_LEFT] = toPath(ctx, "/user/hand/left");
    ctx->handPaths[HAND_RIGHT] = toPath(ctx, "/user/hand/right");

    ctx->aimAction = makeAction(ctx, XR_ACTION_TYPE_POSE_INPUT, "aim", "Pointer");
    ctx->triggerAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Left click");
    ctx->rightClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "rightclick", "Right click");
    ctx->middleClickAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "middleclick", "Middle click");
    ctx->scrollAction = makeAction(ctx, XR_ACTION_TYPE_VECTOR2F_INPUT, "scroll", "Scroll");
    ctx->grabAction = makeAction(ctx, XR_ACTION_TYPE_FLOAT_INPUT, "grab", "Move the screen");
    ctx->toggleAction = makeAction(ctx, XR_ACTION_TYPE_BOOLEAN_INPUT, "pointertoggle", "Pointer on or off");

    if (ctx->aimAction == XR_NULL_HANDLE || ctx->triggerAction == XR_NULL_HANDLE) {
        return 0;
    }

    suggestBindings(ctx, "/interaction_profiles/khr/simple_controller", 1);
    suggestBindings(ctx, "/interaction_profiles/oculus/touch_controller", 1);
    if (ctx->picoInteraction) {
        suggestBindings(ctx, "/interaction_profiles/bytedance/pico4_controller", 1);
    }

    // Hands. aim_activate is the spec's own name for pointing at something out
    // of reach and pinching to act on it, which is exactly what the ray does.
    // Gaze is its own top level path rather than a hand, so it needs an action
    // of its own. There is no click on it: whatever the runtime reports as a
    // trigger, usually a pinch, does the clicking.
    if (ctx->eyeGaze) {
        XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strncpy(info.actionName, "gaze", XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(info.localizedActionName, "Gaze pointer",
                XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        if (checkXr(xrCreateAction(ctx->actionSet, &info, &ctx->gazeAction), "gaze action")) {
            XrActionSuggestedBinding b;
            b.action = ctx->gazeAction;
            b.binding = toPath(ctx, "/user/eyes_ext/input/gaze_ext/pose");

            XrInteractionProfileSuggestedBinding suggest = {
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
            };
            suggest.interactionProfile = toPath(ctx,
                    "/interaction_profiles/ext/eye_gaze_interaction");
            suggest.countSuggestedBindings = 1;
            suggest.suggestedBindings = &b;
            if (XR_FAILED(xrSuggestInteractionProfileBindings(ctx->instance, &suggest))) {
                LOGW("gaze bindings rejected");
                ctx->gazeAction = XR_NULL_HANDLE;
                ctx->eyeGaze = 0;
            }
        }
        else {
            ctx->gazeAction = XR_NULL_HANDLE;
            ctx->eyeGaze = 0;
        }
    }

    if (ctx->handInteraction) {
        // aim_activate is the spec's own name for the far pointer pinch, and
        // pinch is the plain one. Runtimes vary in which they implement.
        static const char* const clicks[] = {
            "input/aim_activate_ext/value", "input/pinch_ext/value"
        };
        const char* profile = "/interaction_profiles/ext/hand_interaction_ext";
        ctx->handClickOk |= suggestHandBindings(ctx, profile, "input/aim_ext/pose",
                                                clicks, 2, "input/grasp_ext/value");
        ctx->handProfile = toPath(ctx, profile);
    }
    // Older runtimes that predate the EXT profile. Same idea, fewer inputs.
    if (ctx->msftHandInteraction) {
        static const char* const clicks[] = { "input/select/value" };
        const char* profile = "/interaction_profiles/microsoft/hand_interaction";
        ctx->handClickOk |= suggestHandBindings(ctx, profile, "input/aim/pose",
                                                clicks, 1, "input/squeeze/value");
        ctx->msftHandProfile = toPath(ctx, profile);
    }

    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &ctx->actionSet;
    if (!checkXr(xrAttachSessionActionSets(ctx->session, &attach), "attach action sets")) {
        return 0;
    }

    for (int h = 0; h < HAND_COUNT; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->aimAction;
        spaceInfo.subactionPath = ctx->handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[h]),
                     "create aim space")) {
            return 0;
        }
    }

    if (ctx->gazeAction != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = ctx->gazeAction;
        spaceInfo.subactionPath = XR_NULL_PATH;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (!checkXr(xrCreateActionSpace(ctx->session, &spaceInfo, &ctx->aimSpaces[SRC_GAZE]),
                     "create gaze space")) {
            ctx->aimSpaces[SRC_GAZE] = XR_NULL_HANDLE;
        }
    }

    ctx->inputReady = 1;
    ctx->pointerOn = 1;
    initJointTracking(ctx);
    refreshInputSource(ctx);
    LOGI("controller input ready (pico bindings %s, hand pinch %s)",
         ctx->picoInteraction ? "offered" : "not offered by this runtime",
         ctx->handClickOk ? "bound" : (ctx->jointTracking ? "from joints" : "unavailable"));
    return 1;
}

static float actionFloat(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0.0f;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(ctx->session, &get, &state)) || !state.isActive) {
        return 0.0f;
    }
    return state.currentState;
}

static int actionBool(XrCtx* ctx, XrAction action, int hand) {
    if (action == XR_NULL_HANDLE) {
        return 0;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(ctx->session, &get, &state)) || !state.isActive) {
        return 0;
    }
    return state.currentState != 0;
}

static XrVector2f actionVec2(XrCtx* ctx, XrAction action, int hand) {
    XrVector2f zero = { 0.0f, 0.0f };
    if (action == XR_NULL_HANDLE) {
        return zero;
    }
    XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
    get.action = action;
    get.subactionPath = hand < 0 ? XR_NULL_PATH : ctx->handPaths[hand];

    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(ctx->session, &get, &state)) || !state.isActive) {
        return zero;
    }
    return state.currentState;
}

// Where the aim ray lands on the screen, in 0..1 texture coordinates with v
// running down the picture. Handles the cylinder as well, since the surface
// bulges toward the viewer and a flat approximation is wrong at the edges by
// the sagitta, which is a fifth of a metre on a wrapped 3 m screen.
static int screenProject(XrPosef aim, XrPosef screen, float width, float height,
                         float radius, int curved, float* outU, float* outV) {
    XrQuaternionf inv = quatConj(screen.orientation);
    Vec3 aimPos = { aim.position.x, aim.position.y, aim.position.z };
    Vec3 screenPos = { screen.position.x, screen.position.y, screen.position.z };
    Vec3 forward = { 0.0f, 0.0f, -1.0f };

    // Both into the screen's own frame, where the surface sits in the xy plane
    Vec3 o = quatRotate(inv, vecSub(aimPos, screenPos));
    Vec3 d = quatRotate(inv, quatRotate(aim.orientation, forward));

    float hx, hy;
    if (curved) {
        // Axis is vertical through the cylinder centre, which sits behind the
        // surface by the radius. The viewer is inside, so there is one root.
        float cz = radius;
        float ox = o.x, oz = o.z - cz;
        float a = d.x * d.x + d.z * d.z;
        float b = 2.0f * (ox * d.x + oz * d.z);
        float c = ox * ox + oz * oz - radius * radius;
        if (a < 1e-6f) {
            return 0;
        }
        float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) {
            return 0;
        }
        float t = (-b + sqrtf(disc)) / (2.0f * a);
        if (t <= 0.0f) {
            return 0;
        }
        float px = o.x + t * d.x;
        float py = o.y + t * d.y;
        float pz = o.z + t * d.z;
        // Angle off the centre of the arc, which faces -z from the axis
        float angle = atan2f(px, cz - pz);
        float centralAngle = width / radius;
        hx = angle / centralAngle;
        hy = py / height;
    }
    else {
        // The quad faces +z in its own frame, so the viewer has to be in front
        // of it and pointing back at it
        if (o.z <= 0.0f || d.z >= -1e-6f) {
            return 0;
        }
        float t = -o.z / d.z;
        hx = (o.x + t * d.x) / width;
        hy = (o.y + t * d.y) / height;
    }

    *outU = hx + 0.5f;
    // Texture rows run down the picture, world y runs up it
    *outV = 0.5f - hy;
    return 1;
}
// A pointer ray from the joints, for runtimes that track hands but never offer
// a pointer pose. Cast from a shoulder rather than from the hand itself: a ray
// along the finger swings wildly with small movements of the wrist, while one
// through the hand from the shoulder is what the arm is actually aiming and is
// steady enough to hold on a target.
static void buildHandRay(XrCtx* ctx, int hand, const XrPosef* head,
                         const XrHandJointLocationEXT* joints) {
    const XrHandJointLocationEXT* knuckle = &joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
    if (!(knuckle->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->handRayValid[hand] = 0;
        return;
    }

    Vec3 offset = { hand == HAND_RIGHT ? 0.17f : -0.17f, -0.20f, 0.05f };
    Vec3 shoulder = quatRotate(head->orientation, offset);
    shoulder.x += head->position.x;
    shoulder.y += head->position.y;
    shoulder.z += head->position.z;

    Vec3 origin = { knuckle->pose.position.x, knuckle->pose.position.y,
                    knuckle->pose.position.z };
    Vec3 dir = vecSub(origin, shoulder);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 0.05f) {
        ctx->handRayValid[hand] = 0;
        return;
    }
    dir = vecNorm(dir);

    // A pose points down its own -Z, so the basis is built around that
    Vec3 worldUp = { 0.0f, 1.0f, 0.0f };
    Vec3 rayZ = { -dir.x, -dir.y, -dir.z };
    Vec3 rayX = vecCross(worldUp, rayZ);
    float side = sqrtf(rayX.x * rayX.x + rayX.y * rayX.y + rayX.z * rayX.z);
    if (side < 0.01f) {
        Vec3 fallback = { 1.0f, 0.0f, 0.0f };
        rayX = vecCross(fallback, rayZ);
    }
    rayX = vecNorm(rayX);
    Vec3 rayY = vecCross(rayZ, rayX);

    ctx->handRay[hand].orientation = quatFromBasis(rayX, rayY, rayZ);
    ctx->handRay[hand].position = knuckle->pose.position;
    ctx->handRayValid[hand] = 1;
}

static int jointPinching(XrCtx* ctx, int hand, XrSpace space, const XrPosef* head,
                         int headValid) {
    if (!ctx->jointTracking || ctx->handTrackers[hand] == XR_NULL_HANDLE) {
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;

    XrHandJointsLocateInfoEXT locate = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    locate.baseSpace = space;
    locate.time = ctx->predictedDisplayTime;
    if (XR_FAILED(ctx->pfnLocateHandJoints(ctx->handTrackers[hand], &locate, &locations))
            || !locations.isActive) {
        ctx->jointPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        ctx->handRayValid[hand] = 0;
        return 0;
    }

    if (headValid) {
        buildHandRay(ctx, hand, head, joints);
    }

    const XrHandJointLocationEXT* thumb = &joints[XR_HAND_JOINT_THUMB_TIP_EXT];
    const XrHandJointLocationEXT* index = &joints[XR_HAND_JOINT_INDEX_TIP_EXT];
    if (!(thumb->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            || !(index->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        ctx->jointPinch[hand] = 0;
        ctx->pinchPointValid[hand] = 0;
        return 0;
    }

    float dx = thumb->pose.position.x - index->pose.position.x;
    float dy = thumb->pose.position.y - index->pose.position.y;
    float dz = thumb->pose.position.z - index->pose.position.z;
    float gap = sqrtf(dx * dx + dy * dy + dz * dz);

    // Where the pinch happened, which is what a drag follows
    ctx->pinchPoint[hand].x = (thumb->pose.position.x + index->pose.position.x) * 0.5f;
    ctx->pinchPoint[hand].y = (thumb->pose.position.y + index->pose.position.y) * 0.5f;
    ctx->pinchPoint[hand].z = (thumb->pose.position.z + index->pose.position.z) * 0.5f;
    ctx->pinchPointValid[hand] = 1;

    ctx->jointPinch[hand] = gap < (ctx->jointPinch[hand] ? PINCH_OFF_M : PINCH_ON_M);
    return ctx->jointPinch[hand];
}



// Which affordance the ray is over. Corners are numbered 0 top left, 1 top
// right, 2 bottom left, 3 bottom right.
static int hoverTest(float u, float v, float width, float height, int* corner) {
    // Centred on the corner, reaching as far outside the picture as inside,
    // because that is where the bracket is drawn
    float reachM = CORNER_FRAC * width * CORNER_HOVER * 0.5f;
    float cu = reachM / width;
    float cv = reachM / height;

    int left = fabsf(u) < cu;
    int right = fabsf(u - 1.0f) < cu;
    int top = fabsf(v) < cv;
    int bottom = fabsf(v - 1.0f) < cv;
    if ((left || right) && (top || bottom)) {
        *corner = (top ? 0 : 2) + (right ? 1 : 0);
        return HOVER_CORNER;
    }

    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
        return HOVER_SCREEN;
    }

    // The move bar sits under the bottom edge, so v runs past 1 here
    float barU = BAR_WIDTH_FRAC * BAR_HOVER * 0.5f;
    float reach = (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 3.0f) * width / height;
    if (v > 1.0f && v < 1.0f + reach && fabsf(u - 0.5f) < barU) {
        return HOVER_BAR;
    }

    // Beyond the picture the ray still draws out to a margin, so it does not
    // blink out on the way to the handles underneath
    if (u > -HALO_FRAC && u < 1.0f + HALO_FRAC && v > -HALO_FRAC && v < 1.0f + HALO_FRAC) {
        return HOVER_HALO;
    }

    return HOVER_NONE;
}

// The inverse of screenHit: where a texture coordinate sits in space. The beam
// is drawn to the filtered point rather than the raw one, so the ray and the
// cursor agree instead of the ray shaking around a steady cursor.
static Vec3 screenPoint(float u, float v, XrPosef screen, float width, float height,
                        float radius, int curved) {
    Vec3 local;
    local.y = (0.5f - v) * height;
    if (curved) {
        float angle = (u - 0.5f) * (width / radius);
        local.x = radius * sinf(angle);
        local.z = radius - radius * cosf(angle);
    }
    else {
        local.x = (u - 0.5f) * width;
        local.z = 0.0f;
    }

    Vec3 rotated = quatRotate(screen.orientation, local);
    Vec3 world = { screen.position.x + rotated.x,
                   screen.position.y + rotated.y,
                   screen.position.z + rotated.z };
    return world;
}

static int createPointerSwapchain(XrCtx* ctx) {
    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = ctx->swapchainFormat;
    info.sampleCount = 1;
    info.width = PTR_TEX_W;
    info.height = PTR_TEX_H;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->pointerSwapchain),
                 "create pointer swapchain")) {
        ctx->pointerSwapchain = XR_NULL_HANDLE;
        return 0;
    }

    xrEnumerateSwapchainImages(ctx->pointerSwapchain, 0, &ctx->pointerImageCount, NULL);
    ctx->pointerImages = calloc(ctx->pointerImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
    for (uint32_t i = 0; i < ctx->pointerImageCount; i++) {
        ctx->pointerImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    xrEnumerateSwapchainImages(ctx->pointerSwapchain, ctx->pointerImageCount,
                               &ctx->pointerImageCount,
                               (XrSwapchainImageBaseHeader*)ctx->pointerImages);

    // Handles get a swapchain each rather than a corner of the atlas, so there
    // is no image rect origin convention to guess at
    info.width = BAR_TEX_W;
    info.height = BAR_TEX_H;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->barSwapchain), "create bar swapchain")) {
        xrEnumerateSwapchainImages(ctx->barSwapchain, 0, &ctx->barImageCount, NULL);
        ctx->barImages = calloc(ctx->barImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->barImageCount; i++) {
            ctx->barImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->barSwapchain, ctx->barImageCount, &ctx->barImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->barImages);
    }
    else {
        ctx->barSwapchain = XR_NULL_HANDLE;
    }

    info.width = PICKER_TEX_W;
    info.height = PICKER_TEX_H;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->pickerSwapchain),
                "create picker swapchain")) {
        xrEnumerateSwapchainImages(ctx->pickerSwapchain, 0, &ctx->pickerImageCount, NULL);
        ctx->pickerImages = calloc(ctx->pickerImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->pickerImageCount; i++) {
            ctx->pickerImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->pickerSwapchain, ctx->pickerImageCount,
                                   &ctx->pickerImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->pickerImages);
    }
    else {
        ctx->pickerSwapchain = XR_NULL_HANDLE;
    }

    info.width = OUTLINE_TEX;
    info.height = OUTLINE_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->envButtonSwapchain),
                "create env button swapchain")) {
        xrEnumerateSwapchainImages(ctx->envButtonSwapchain, 0, &ctx->envButtonImageCount, NULL);
        ctx->envButtonImages = calloc(ctx->envButtonImageCount,
                                      sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->envButtonImageCount; i++) {
            ctx->envButtonImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->envButtonSwapchain, ctx->envButtonImageCount,
                                   &ctx->envButtonImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->envButtonImages);
    }
    else {
        ctx->envButtonSwapchain = XR_NULL_HANDLE;
    }

    // Two padlocks rather than one, since a quad layer has no way to swap
    // its own texture and open and shut have to read differently
    info.width = LOCK_TEX;
    info.height = LOCK_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->lockSwapchain),
                "create lock swapchain")) {
        xrEnumerateSwapchainImages(ctx->lockSwapchain, 0, &ctx->lockImageCount, NULL);
        ctx->lockImages = calloc(ctx->lockImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->lockImageCount; i++) {
            ctx->lockImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->lockSwapchain, ctx->lockImageCount, &ctx->lockImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->lockImages);
    }
    else {
        ctx->lockSwapchain = XR_NULL_HANDLE;
    }

    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->unlockSwapchain),
                "create unlock swapchain")) {
        xrEnumerateSwapchainImages(ctx->unlockSwapchain, 0, &ctx->unlockImageCount, NULL);
        ctx->unlockImages = calloc(ctx->unlockImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->unlockImageCount; i++) {
            ctx->unlockImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->unlockSwapchain, ctx->unlockImageCount,
                                   &ctx->unlockImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->unlockImages);
    }
    else {
        ctx->unlockSwapchain = XR_NULL_HANDLE;
    }

    info.width = OUTLINE_TEX;
    info.height = OUTLINE_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->outlineSwapchain),
                "create outline swapchain")) {
        xrEnumerateSwapchainImages(ctx->outlineSwapchain, 0, &ctx->outlineImageCount, NULL);
        ctx->outlineImages = calloc(ctx->outlineImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->outlineImageCount; i++) {
            ctx->outlineImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->outlineSwapchain, ctx->outlineImageCount,
                                   &ctx->outlineImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->outlineImages);
    }
    else {
        ctx->outlineSwapchain = XR_NULL_HANDLE;
    }

    info.width = CORNER_TEX_W;
    info.height = CORNER_TEX_H;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->cornerSwapchain),
                "create corner swapchain")) {
        xrEnumerateSwapchainImages(ctx->cornerSwapchain, 0, &ctx->cornerImageCount, NULL);
        ctx->cornerImages = calloc(ctx->cornerImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->cornerImageCount; i++) {
            ctx->cornerImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->cornerSwapchain, ctx->cornerImageCount,
                                   &ctx->cornerImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->cornerImages);
    }
    else {
        ctx->cornerSwapchain = XR_NULL_HANDLE;
    }

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

    int ok = uploadArt(ctx, ctx->barSwapchain, ctx->barImages, bar, BAR_TEX_W, BAR_TEX_H);
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
static int uploadPointerArt(XrCtx* ctx) {
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

// The sliders place the screen, the grab moves it from there. Moving either
// slider is taken as the user asking for the placement back.
static void updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature) {
    int sliderMoved = ctx->sliderSeen
            && (fabsf(distance - ctx->lastDistance) > 1e-4f
                || fabsf(quadWidth - ctx->lastQuadWidth) > 1e-4f);

    if (!ctx->placementValid || sliderMoved) {
        memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
        ctx->screenPose.orientation.w = 1.0f;
        ctx->screenPose.position.z = -distance;
        ctx->screenWidth = quadWidth;
        // Radius runs from 4x distance (slightly curved) down to the distance
        // itself (wrapped around the viewer) as curvature rises
        ctx->screenRadius = distance * (1.0f + 3.0f * (1.0f - curvature));
        ctx->placementValid = 1;
        ctx->grabMode = GRAB_NONE;
        ctx->poseDirty = 1;
    }

    ctx->lastDistance = distance;
    ctx->lastQuadWidth = quadWidth;
    ctx->sliderSeen = 1;
}

// Handed back only when a grab ends, so preferences are written once per move
// rather than every frame of it
static void writeInputPose(XrCtx* ctx, float* out) {
    if (!ctx->poseDirty) {
        return;
    }
    ctx->poseDirty = 0;
    out[IN_POSE_DIRTY] = 1.0f;
    out[IN_POSE + 0] = ctx->screenPose.position.x;
    out[IN_POSE + 1] = ctx->screenPose.position.y;
    out[IN_POSE + 2] = ctx->screenPose.position.z;
    out[IN_POSE + 3] = ctx->screenPose.orientation.x;
    out[IN_POSE + 4] = ctx->screenPose.orientation.y;
    out[IN_POSE + 5] = ctx->screenPose.orientation.z;
    out[IN_POSE + 6] = ctx->screenPose.orientation.w;
    out[IN_POSE + 7] = ctx->screenWidth;
    out[IN_POSE + 8] = ctx->screenRadius;
}

// Move and resize both work off the handle the ray was over when the grip
// closed. Gripping the picture itself does nothing, which keeps the panel from
// being dragged by accident while pointing at something.
static void applyGrab(XrCtx* ctx, XrPosef* aims, const int* valid, int hand,
                      int hover, int corner, int offPicture, float height, int curved) {
    for (int h = 0; h < HAND_COUNT; h++) {
        int wasDown = ctx->grabDown[h];
        float value = actionFloat(ctx, ctx->grabAction, h);
        ctx->grabDown[h] = value > (wasDown ? PRESS_OFF : PRESS_ON);
        ctx->gripEdge[h] = ctx->grabDown[h] && !wasDown;
    }

    if (ctx->grabMode != GRAB_NONE) {
        int stillHeld = ctx->grabByTrigger ? ctx->triggerDown[ctx->grabHand]
                                           : ctx->grabDown[ctx->grabHand];
        if (!stillHeld || !valid[ctx->grabHand]) {
            // Persist where it ended up, not every frame of the drag
            ctx->grabMode = GRAB_NONE;
            ctx->poseDirty = 1;
            return;
        }
    }

    if (ctx->grabMode == GRAB_NONE) {
        if (hand < 0 || (hover != HOVER_BAR && hover != HOVER_CORNER)) {
            return;
        }

        // Apps disagree about which button grabs, so both do. The trigger only
        // counts where the handle hangs outside the picture, since inside it is
        // a left click and the bottom corners of a desktop are worth clicking.
        int byGrip = ctx->gripEdge[hand];
        int byTrigger = ctx->triggerEdge[hand] && offPicture;
        if (!byGrip && !byTrigger) {
            return;
        }
        ctx->grabByTrigger = !byGrip;

        ctx->grabHand = hand;
        ctx->grabAim = aims[hand];
        ctx->grabScreen = ctx->screenPose;
        ctx->grabWidth = ctx->screenWidth;
        ctx->grabHeight = height;
        ctx->grabRadius = ctx->screenRadius;

        if (hover == HOVER_BAR) {
            ctx->grabMode = GRAB_MOVE;
            return;
        }

        float u, v;
        if (!screenProject(aims[hand], ctx->grabScreen, ctx->screenWidth, height,
                           ctx->screenRadius, curved, &u, &v)) {
            return;
        }

        // The corner across the diagonal is the anchor, and the drag is
        // measured along the diagonal it started on
        int right = (corner == 1 || corner == 3);
        int bottom = (corner >= 2);
        ctx->grabOppX = (right ? -0.5f : 0.5f) * ctx->grabWidth;
        ctx->grabOppY = (bottom ? 0.5f : -0.5f) * ctx->grabHeight;
        ctx->grabDiagX = (u - 0.5f) * ctx->grabWidth - ctx->grabOppX;
        ctx->grabDiagY = (0.5f - v) * ctx->grabHeight - ctx->grabOppY;
        if (fabsf(ctx->grabDiagX) < 1e-3f && fabsf(ctx->grabDiagY) < 1e-3f) {
            return;
        }
        ctx->grabMode = GRAB_RESIZE;
        return;
    }

    int h = ctx->grabHand;
    if (ctx->grabMode == GRAB_MOVE) {
        // Rigid attach: the screen keeps its offset and rotation relative to
        // the hand, so it swings around naturally instead of sliding flat
        XrQuaternionf turn = quatMul(aims[h].orientation, quatConj(ctx->grabAim.orientation));
        Vec3 offset = { ctx->grabScreen.position.x - ctx->grabAim.position.x,
                        ctx->grabScreen.position.y - ctx->grabAim.position.y,
                        ctx->grabScreen.position.z - ctx->grabAim.position.z };
        Vec3 moved = quatRotate(turn, offset);

        ctx->screenPose.orientation = quatNorm(quatMul(turn, ctx->grabScreen.orientation));
        ctx->screenPose.position.x = aims[h].position.x + moved.x;
        ctx->screenPose.position.y = aims[h].position.y + moved.y;
        ctx->screenPose.position.z = aims[h].position.z + moved.z;
        return;
    }

    // Resize. Everything is measured against the pose the grab started from,
    // so growing the screen cannot feed back into where the ray lands on it.
    float u, v;
    if (!screenProject(aims[h], ctx->grabScreen, ctx->grabWidth, ctx->grabHeight,
                       ctx->grabRadius, curved, &u, &v)) {
        return;
    }

    float dx = (u - 0.5f) * ctx->grabWidth - ctx->grabOppX;
    float dy = (0.5f - v) * ctx->grabHeight - ctx->grabOppY;
    float diagLen = ctx->grabDiagX * ctx->grabDiagX + ctx->grabDiagY * ctx->grabDiagY;
    float scale = (dx * ctx->grabDiagX + dy * ctx->grabDiagY) / diagLen;
    if (scale < 0.05f) {
        scale = 0.05f;
    }

    float width = ctx->grabWidth * scale;
    if (width < SCREEN_MIN_WIDTH) width = SCREEN_MIN_WIDTH;
    if (width > SCREEN_MAX_WIDTH) width = SCREEN_MAX_WIDTH;
    float newHeight = ctx->grabHeight * (width / ctx->grabWidth);

    // Keeping the arc the same shape rather than flattening as it grows
    ctx->screenRadius = ctx->grabRadius * (width / ctx->grabWidth);
    ctx->screenWidth = width;

    // The anchor corner stays where it was, so the screen grows away from it
    Vec3 centreLocal;
    centreLocal.x = ctx->grabOppX + (ctx->grabOppX > 0.0f ? -0.5f : 0.5f) * width;
    centreLocal.y = ctx->grabOppY + (ctx->grabOppY > 0.0f ? -0.5f : 0.5f) * newHeight;
    centreLocal.z = 0.0f;

    Vec3 centre = quatRotate(ctx->grabScreen.orientation, centreLocal);
    ctx->screenPose.orientation = ctx->grabScreen.orientation;
    ctx->screenPose.position.x = ctx->grabScreen.position.x + centre.x;
    ctx->screenPose.position.y = ctx->grabScreen.position.y + centre.y;
    ctx->screenPose.position.z = ctx->grabScreen.position.z + centre.z;
}

// The picker floats just in front of the screen, centred on it
static XrPosef pickerPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = ctx->screenWidth * PICKER_WIDTH_FRAC;
    *outWidth = width;
    *outHeight = width * (float)PICKER_TEX_H / (float)PICKER_TEX_W;

    Vec3 local = { 0.0f, 0.0f, 0.06f };
    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose = ctx->screenPose;
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Button sits to the left of the move bar, at the same height
static void envButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float side = ctx->screenWidth * ENV_BUTTON_FRAC;
    float barW = ctx->screenWidth * BAR_WIDTH_FRAC;
    float barH = ctx->screenWidth * BAR_HEIGHT_FRAC;
    outLocal->x = -(barW * 0.5f + ctx->screenWidth * ENV_GAP_FRAC + side * 0.5f);
    outLocal->y = -(height * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

static int envButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    envButtonPlacement(ctx, height, &local, &side);

    // Back into uv, where the button reaches a little further than it draws
    float cu = 0.5f + local.x / ctx->screenWidth;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / ctx->screenWidth;
    float halfV = side * HOVER_MARGIN * 0.5f / height;
    return fabsf(u - cu) < halfU && fabsf(v - cv) < halfV;
}

// Padlock sits off the left edge, halfway up
static void lockButtonPlacement(XrCtx* ctx, Vec3* outLocal, float* outSide) {
    float side = ctx->screenWidth * LOCK_BUTTON_FRAC;
    outLocal->x = -(ctx->screenWidth * (0.5f + LOCK_GAP_FRAC) + side * 0.5f);
    outLocal->y = 0.0f;
    outLocal->z = 0.005f;
    *outSide = side;
}

static int lockButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    lockButtonPlacement(ctx, &local, &side);

    float cu = 0.5f + local.x / ctx->screenWidth;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / ctx->screenWidth;
    float halfV = side * HOVER_MARGIN * 0.5f / height;
    return fabsf(u - cu) < halfU && fabsf(v - cv) < halfV;
}

// Where the ray lands on furniture rather than on the picture. The grid has a
// plane of its own, everything else sits on the screen.
static Vec3 furniturePoint(XrCtx* ctx, int hover, float u, float v, XrPosef screenPose,
                           float height, float radius, int curved) {
    if (hover == HOVER_PICKER) {
        float pickW, pickH;
        XrPosef pose = pickerPose(ctx, &pickW, &pickH);
        return screenPoint(u, v, pose, pickW, pickH, 0.0f, 0);
    }
    return screenPoint(u, v, screenPose, ctx->screenWidth, height, radius, curved);
}

static void destroyXrInput(XrCtx* ctx) {
    for (int h = 0; h < HAND_COUNT; h++) {
        if (ctx->handTrackers[h] != XR_NULL_HANDLE && ctx->pfnDestroyHandTracker != NULL) {
            ctx->pfnDestroyHandTracker(ctx->handTrackers[h]);
            ctx->handTrackers[h] = XR_NULL_HANDLE;
        }
    }
    for (int h = 0; h < SRC_COUNT; h++) {
        if (ctx->aimSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(ctx->aimSpaces[h]);
            ctx->aimSpaces[h] = XR_NULL_HANDLE;
        }
    }
    if (ctx->actionSet != XR_NULL_HANDLE) {
        // Takes its actions with it
        xrDestroyActionSet(ctx->actionSet);
        ctx->actionSet = XR_NULL_HANDLE;
    }
    ctx->inputReady = 0;
}

static void destroyCtx(JNIEnv* env, XrCtx* ctx) {
    destroyXrInput(ctx);

    free(ctx->readbackBuf);
    free(ctx->modelInput);
    free(ctx->modelOutput);
    free(ctx->depthUploadBuf);
    free(ctx->depthEma);
    free(ctx->depthLow);
    free(ctx->depthScratch);
    free(ctx->depthColSums);

    if (ctx->swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->swapchain);
    }
    free(ctx->swapchainImages);
    if (ctx->overlaySwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->overlaySwapchain);
    }
    free(ctx->overlayImages);
    if (ctx->pointerSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->pointerSwapchain);
    }
    free(ctx->pointerImages);
    if (ctx->barSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->barSwapchain);
    }
    free(ctx->barImages);
    if (ctx->cornerSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->cornerSwapchain);
    }
    free(ctx->cornerImages);
    if (ctx->backgroundSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->backgroundSwapchain);
    }
    free(ctx->backgroundImages);
    if (ctx->pickerSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->pickerSwapchain);
    }
    free(ctx->pickerImages);
    if (ctx->envButtonSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->envButtonSwapchain);
    }
    free(ctx->envButtonImages);
    if (ctx->lockSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->lockSwapchain);
    }
    free(ctx->lockImages);
    if (ctx->unlockSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->unlockSwapchain);
    }
    free(ctx->unlockImages);
    if (ctx->outlineSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->outlineSwapchain);
    }
    free(ctx->outlineImages);
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
                                                       jint stereoMode, jboolean depthDebug,
                                                       jint convergence, jint depthScale,
                                                       jboolean handTracking) {
    XrCtx* ctx = calloc(1, sizeof(XrCtx));
    ctx->handsEnabled = handTracking;
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
    // Shown whenever there is text to show, the preference is the real gate
    ctx->overlayVisible = 1;
    ctx->separationOverride = -1.0f;
    ctx->distanceOverride = -1.0f;
    ctx->screenOverride = -1.0f;
    ctx->pointerMinCutoff = POINTER_MIN_CUTOFF;
    ctx->pointerBeta = POINTER_BETA;
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

    LOGI("OpenXR init complete (cylinder=%d equirect=%d srgbWriteControl=%d)",
         ctx->cylinderSupported, ctx->equirectSupported, ctx->srgbWriteControl);
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
    return (jint)ctx->oesTexture;
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

// 2nd and 98th percentile of the model output, via a histogram. Using the
// raw min and max lets one stray pixel own the whole mapping: on a measured
// frame the 2..98 span was 638 of an 805 wide min/max range, so a fifth of
// the output range was being spent on a handful of pixels.
static void robustRange(const float* v, int count, float* outLo, float* outHi) {
    float lo = v[0], hi = v[0];
    for (int i = 1; i < count; i++) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    if (hi <= lo) {
        *outLo = lo;
        *outHi = lo + 1.0f;
        return;
    }

    int hist[DEPTH_HIST_BINS];
    memset(hist, 0, sizeof(hist));
    float scale = DEPTH_HIST_BINS / (hi - lo);
    for (int i = 0; i < count; i++) {
        int b = (int)((v[i] - lo) * scale);
        if (b < 0) b = 0;
        if (b >= DEPTH_HIST_BINS) b = DEPTH_HIST_BINS - 1;
        hist[b]++;
    }

    int loTarget = (int)(count * 0.02f);
    int hiTarget = (int)(count * 0.98f);
    int acc = 0, loBin = 0, hiBin = DEPTH_HIST_BINS - 1;
    for (int b = 0; b < DEPTH_HIST_BINS; b++) {
        acc += hist[b];
        if (acc >= loTarget) {
            loBin = b;
            break;
        }
    }
    acc = 0;
    for (int b = 0; b < DEPTH_HIST_BINS; b++) {
        acc += hist[b];
        if (acc >= hiTarget) {
            hiBin = b;
            break;
        }
    }

    *outLo = lo + loBin / scale;
    *outHi = lo + (hiBin + 1) / scale;
    if (*outHi <= *outLo) {
        *outHi = *outLo + 1e-6f;
    }
}

static void boxBlurH(const float* src, float* dst, int n, int r) {
    float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < n; y++) {
        const float* s = src + (size_t)y * n;
        float* d = dst + (size_t)y * n;
        float sum = 0.0f;
        for (int i = -r; i <= r; i++) {
            int x = i < 0 ? 0 : (i >= n ? n - 1 : i);
            sum += s[x];
        }
        for (int x = 0; x < n; x++) {
            d[x] = sum * inv;
            int add = x + r + 1;
            int sub = x - r;
            sum += s[add >= n ? n - 1 : add] - s[sub < 0 ? 0 : sub];
        }
    }
}

// Column sums carried a row at a time. The obvious version, one column at a
// time, strides a whole row between reads and misses cache on every access,
// which cost 15 ms here rather than 1.
static void boxBlurV(const float* src, float* dst, int n, int r, float* colSums) {
    float inv = 1.0f / (float)(2 * r + 1);
    memset(colSums, 0, (size_t)n * sizeof(float));
    for (int i = -r; i <= r; i++) {
        int y = i < 0 ? 0 : (i >= n ? n - 1 : i);
        const float* s = src + (size_t)y * n;
        for (int x = 0; x < n; x++) {
            colSums[x] += s[x];
        }
    }
    for (int y = 0; y < n; y++) {
        float* d = dst + (size_t)y * n;
        for (int x = 0; x < n; x++) {
            d[x] = colSums[x] * inv;
        }
        int add = y + r + 1;
        int sub = y - r;
        const float* a = src + (size_t)(add >= n ? n - 1 : add) * n;
        const float* b = src + (size_t)(sub < 0 ? 0 : sub) * n;
        for (int x = 0; x < n; x++) {
            colSums[x] += a[x] - b[x];
        }
    }
}

// Three box passes is close enough to a gaussian
static void lowPass(const float* src, float* dst, float* scratch, float* colSums, int n, int r) {
    boxBlurH(src, scratch, n, r);
    boxBlurV(scratch, dst, n, r, colSums);
    boxBlurH(dst, scratch, n, r);
    boxBlurV(scratch, dst, n, r, colSums);
    boxBlurH(dst, scratch, n, r);
    boxBlurV(scratch, dst, n, r, colSums);
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

// Reads the controllers and works out where they are pointing on the screen.
// Java turns the result into host mouse events, so nothing here knows about
// the connection.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUpdateInput(JNIEnv* env, jobject thiz,
                                                              jlong handle, jfloat distance,
                                                              jfloat quadWidth, jfloat curvature,
                                                              jboolean headLocked,
                                                              jboolean pointerEnabled,
                                                              jboolean gazeEnabled,
                                                              jfloatArray outArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    float out[IN_SLOTS];
    memset(out, 0, sizeof(out));
    if (ctx != NULL) {
        ctx->gazeEnabled = gazeEnabled;
    }
    // Zero is a real cell, so "nothing picked" has to be said explicitly. Every
    // early return below would otherwise read as a press on the first one.
    out[IN_PICKER_PICK] = -1.0f;

    // Anything held has to come back up when pointing stops, or the host is
    // left with a stuck button
    if (ctx == NULL || !ctx->inputReady || !pointerEnabled || !ctx->placementValid
            || ctx->sessionState != XR_SESSION_STATE_FOCUSED) {
        if (ctx != NULL) {
            ctx->buttonsDown = 0;
            ctx->beamVisible = 0;
            if (ctx->grabMode != 0) {
                // Dropping focus mid grab has to count as letting go, or the
                // anchor is stale when focus comes back and the screen jumps
                ctx->grabMode = 0;
                ctx->poseDirty = 1;
            }
        }
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    XrActiveActionSet active;
    active.actionSet = ctx->actionSet;
    active.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(ctx->session, &sync))) {
        ctx->buttonsDown = 0;
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    int toggle = actionBool(ctx, ctx->toggleAction, -1);
    if (toggle && !ctx->togglePrev) {
        ctx->pointerOn = !ctx->pointerOn;
        LOGI("pointer %s", ctx->pointerOn ? "on" : "off");
    }
    ctx->togglePrev = toggle;
    out[IN_POINTER] = ctx->pointerOn ? 1.0f : 0.0f;

    XrSpace space = headLocked ? ctx->viewSpace : ctx->localSpace;
    float height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    int curved = curvature > 0.01f && ctx->cylinderSupported;
    float radius = ctx->screenRadius;
    XrPosef screenPose = ctx->screenPose;

    long now = nowNs();
    float dt = ctx->lastInputNs != 0 ? (now - ctx->lastInputNs) / 1e9f : 0.0f;
    ctx->lastInputNs = now;
    if (dt > 0.1f) {
        dt = 0.1f;
    }

    XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
    int headValid = XR_SUCCEEDED(xrLocateSpace(ctx->viewSpace, space,
                                               ctx->predictedDisplayTime, &headLoc))
            && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if (headValid) {
        ctx->headPos = headLoc.pose.position;
    }

    float hitU[SRC_COUNT], hitV[SRC_COUNT];
    int hovers[SRC_COUNT] = { HOVER_NONE, HOVER_NONE, HOVER_NONE };
    int corners[SRC_COUNT] = { 0, 0, 0 };
    int aimValid[SRC_COUNT] = { 0, 0, 0 };
    // Tracked separately from the hover, because the lock filter below wipes
    // the hovers and this is what says which source to spare
    int atLock[SRC_COUNT] = { 0, 0, 0 };
    XrPosef aimPoses[SRC_COUNT];
    int moved = 0;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (h < HAND_COUNT) {
            int wasDown = ctx->triggerDown[h];
            float value = actionFloat(ctx, ctx->triggerAction, h);
            // Either a bound trigger or a measured pinch will do. Runtimes
            // that offer neither leave this at rest, which is what a headset
            // with nothing in its hands should report.
            ctx->triggerDown[h] = value > (wasDown ? PRESS_OFF : PRESS_ON)
                    || jointPinching(ctx, h, space, &headLoc.pose, headValid);
            ctx->triggerEdge[h] = ctx->triggerDown[h] && !wasDown;
        }
        else if (!ctx->eyeGaze || !ctx->gazeEnabled
                 || ctx->aimSpaces[SRC_GAZE] == XR_NULL_HANDLE) {
            continue;
        }

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT
                | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        int located = XR_SUCCEEDED(xrLocateSpace(ctx->aimSpaces[h], space,
                                                 ctx->predictedDisplayTime, &loc))
                && (loc.locationFlags & needed) == needed;
        if (!located) {
            // No controller and no pointer pose from the runtime, so the ray
            // built out of the joints stands in. This is what makes hand
            // pointing work on runtimes that refuse the hand profile.
            if (h < HAND_COUNT && ctx->handRayValid[h]) {
                loc.pose = ctx->handRay[h];
            }
            else {
                continue;
            }
        }
        aimPoses[h] = loc.pose;
        aimValid[h] = 1;
        if (screenProject(loc.pose, screenPose, ctx->screenWidth, height, radius, curved,
                          &hitU[h], &hitV[h])) {
            hovers[h] = hoverTest(hitU[h], hitV[h], ctx->screenWidth, height, &corners[h]);
            // The button reaches past the left end of the bar's zone, so it is
            // tested here rather than after a hand has been picked. Otherwise
            // the part of it outside that zone belongs to no hand at all.
            if ((hovers[h] == HOVER_NONE || hovers[h] == HOVER_BAR)
                    && envButtonHit(ctx, hitU[h], hitV[h], height)) {
                hovers[h] = HOVER_ENVBUTTON;
            }
            // Off the left edge, so the halo owns that ground until the
            // padlock claims it back
            if (ctx->handsEnabled && hovers[h] != HOVER_ENVBUTTON
                    && (hovers[h] == HOVER_NONE || hovers[h] == HOVER_HALO)
                    && lockButtonHit(ctx, hitU[h], hitV[h], height)) {
                hovers[h] = HOVER_LOCK;
                atLock[h] = 1;
            }
        }

        if (ctx->poseSeen[h] && dt > 0.0f) {
            Vec3 now3 = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            Vec3 was3 = { ctx->lastAim[h].position.x, ctx->lastAim[h].position.y,
                          ctx->lastAim[h].position.z };
            Vec3 step = vecSub(now3, was3);
            float speed = sqrtf(step.x * step.x + step.y * step.y + step.z * step.z) / dt;

            // Angle between the two orientations, from the dot product of the
            // quaternions, which is half the rotation
            XrQuaternionf a = loc.pose.orientation, b = ctx->lastAim[h].orientation;
            float dot = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            if (dot > 1.0f) {
                dot = 1.0f;
            }
            float turn = 2.0f * acosf(dot) / dt;

            // Hands and eyes are never still, so their motion says nothing
            // about intent and the gate would just hold the pointer on forever
            if (!ctx->usingHands[h] && h != SRC_GAZE
                    && (speed > POINTER_MOVE_SPEED || turn > POINTER_TURN_SPEED)) {
                moved = 1;
            }
        }
        ctx->lastAim[h] = loc.pose;
        ctx->poseSeen[h] = 1;
    }

    // Locked hands reach the padlock and nothing else. Everything is dropped
    // at once, the aim as well as the pinch, so there is no ray to chase, no
    // click to land and no grab to start. Controllers are untouched: they
    // never had the problem, and one has to stay able to unlock.
    for (int h = 0; h < HAND_COUNT; h++) {
        if (!ctx->handsLocked || !ctx->usingHands[h] || atLock[h]) {
            continue;
        }
        hovers[h] = HOVER_NONE;
        aimValid[h] = 0;
        ctx->triggerDown[h] = 0;
        ctx->triggerEdge[h] = 0;
    }

    for (int h = 0; h < SRC_COUNT; h++) {
        if (!atLock[h]) {
            ctx->lockArmed[h] = 0;
        }
        else if (!ctx->triggerDown[h]) {
            ctx->lockArmed[h] = 1;
        }
    }

    // Gaze has no button of its own, so a pinch from either hand clicks
    // wherever the eyes have landed
    if (aimValid[SRC_GAZE]) {
        ctx->triggerDown[SRC_GAZE] = ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT];
        ctx->triggerEdge[SRC_GAZE] = ctx->triggerEdge[HAND_LEFT] || ctx->triggerEdge[HAND_RIGHT];
        ctx->usingHands[SRC_GAZE] = 1;
    }
    else {
        ctx->triggerDown[SRC_GAZE] = 0;
        ctx->triggerEdge[SRC_GAZE] = 0;
        ctx->usingHands[SRC_GAZE] = 0;
    }

    // A pinch is what a hand has instead of deliberate movement: it turns the
    // pointer on, and keeps it on for as long as pinches keep arriving. The
    // one that does the waking is swallowed rather than passed on as a click,
    // since the user was reaching for the pointer and not for the screen.
    int pinching = 0;
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->usingHands[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        // A pinch on the padlock is always meant as a press. The swallow is
        // there to keep a waking pinch off the host, and the padlock is not
        // the host, so charging the user a pinch for it buys nothing.
        if (atLock[h]) {
            ctx->pinchSwallowed[h] = 0;
            continue;
        }
        if (ctx->triggerDown[h]) {
            pinching = 1;
            if (!ctx->pointerAwake) {
                ctx->pointerAwake = 1;
                ctx->pinchSwallowed[h] = 1;
            }
        }
        else {
            ctx->pinchSwallowed[h] = 0;
        }
        if (ctx->pinchSwallowed[h]) {
            ctx->triggerDown[h] = 0;
            ctx->triggerEdge[h] = 0;
        }
    }

    // Deliberate movement wakes the pointer, a controller put down retires it
    if (pinching) {
        // Only the pinch clock matters while hands are in charge
        ctx->stillFor = 0.0f;
        ctx->movingFor = 0.0f;
    }
    else if (moved) {
        ctx->movingFor += dt;
        ctx->stillFor = 0.0f;
        if (ctx->movingFor >= ctx->pointerWake) {
            ctx->pointerAwake = 1;
        }
    }
    else {
        ctx->stillFor += dt;
        ctx->movingFor = 0.0f;
        if (ctx->stillFor >= ctx->pointerSleep) {
            ctx->pointerAwake = 0;
        }
    }

    // The hand holding the trigger wins, so a drag is never stolen by the other
    // one drifting across the screen. Right hand otherwise.
    static const int order[SRC_COUNT] = { HAND_RIGHT, HAND_LEFT, SRC_GAZE };
    int hand = -1;
    for (int i = 0; i < SRC_COUNT; i++) {
        int h = order[i];
        if (hovers[h] == HOVER_SCREEN && ctx->triggerDown[h]) {
            hand = h;
            break;
        }
    }
    // A hand on something beats one merely near it, so a controller resting in
    // the margin never takes the pointer off the one being aimed
    for (int pass = 0; pass < 2 && hand < 0; pass++) {
        for (int i = 0; i < SRC_COUNT && hand < 0; i++) {
            int h = order[i];
            if (hovers[h] == HOVER_NONE || (pass == 0 && hovers[h] == HOVER_HALO)) {
                continue;
            }
            hand = h;
        }
    }

    // The padlock is reachable with the pointer asleep, because locking is
    // what put it to sleep and there would otherwise be no way back
    int reachingLock = hand >= 0 && atLock[hand];
    if (!ctx->pointerAwake && !reachingLock && ctx->grabMode == GRAB_NONE) {
        hand = -1;
        for (int h = 0; h < SRC_COUNT; h++) {
            hovers[h] = HOVER_NONE;
        }
    }

    int hover = hand >= 0 ? hovers[hand] : HOVER_NONE;
    if (hover == HOVER_CORNER) {
        ctx->hoverCorner = corners[hand];
    }

    // The picker is modal: while it is open the ray belongs to it and nothing
    // reaches the picture behind
    ctx->pickerHover = -1;
    ctx->envButtonHot = 0;
    ctx->lockHot = 0;
    ctx->pickerPick = -1;
    if (ctx->pickerOpen) {
        hover = HOVER_PICKER;
        // Anything the hands were pointing at before belongs to the screen,
        // and reading those coordinates as grid coordinates would land the
        // ray somewhere it never was
        hand = -1;
        float pickW, pickH;
        XrPosef pose = pickerPose(ctx, &pickW, &pickH);
        for (int h = 0; h < SRC_COUNT; h++) {
            float pu, pv;
            if (!aimValid[h] || !ctx->pointerAwake) {
                continue;
            }
            if (!screenProject(aimPoses[h], pose, pickW, pickH, 0.0f, 0, &pu, &pv)) {
                continue;
            }
            if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
                continue;
            }
            int col = (int)(pu * PICKER_COLS);
            int row = (int)(pv * PICKER_ROWS);
            if (col >= PICKER_COLS) col = PICKER_COLS - 1;
            if (row >= PICKER_ROWS) row = PICKER_ROWS - 1;
            ctx->pickerHover = row * PICKER_COLS + col;
            hand = h;
            hitU[h] = pu;
            hitV[h] = pv;

            if (ctx->triggerEdge[h]) {
                ctx->pickerPick = ctx->pickerHover;
                ctx->pickerChoice = ctx->pickerHover;
                ctx->pickerOpen = 0;
            }
            break;
        }

        // A press that lands nowhere near the grid closes it
        if (ctx->pickerOpen && ctx->pickerHover < 0) {
            for (int h = 0; h < SRC_COUNT; h++) {
                if (ctx->triggerEdge[h]) {
                    ctx->pickerOpen = 0;
                }
            }
        }
    }
    else if (hover == HOVER_ENVBUTTON) {
        ctx->envButtonHot = 1;
        if (ctx->triggerEdge[hand]) {
            ctx->pickerOpen = 1;
        }
    }
    else if (hover == HOVER_LOCK) {
        ctx->lockHot = 1;
        if (ctx->triggerEdge[hand] && ctx->lockArmed[hand]) {
            ctx->lockArmed[hand] = 0;
            ctx->handsLocked = !ctx->handsLocked;
            LOGI("hands %s", ctx->handsLocked ? "locked" : "unlocked");
            if (ctx->handsLocked) {
                // Put the ray away and let go of anything held, so locking
                // mid drag does not leave the screen stuck to a hand or a
                // button down on the host
                ctx->pointerAwake = 0;
                ctx->buttonsDown = 0;
                ctx->stillFor = 0.0f;
                ctx->movingFor = 0.0f;
                if (ctx->grabMode != GRAB_NONE) {
                    ctx->grabMode = GRAB_NONE;
                    ctx->poseDirty = 1;
                }
            }
        }
    }

    // One line that says whether gaze is tracking, whether it is the thing
    // doing the pointing, and whether a pinch is reaching us at all. Logged
    // only when it changes, so it costs nothing while it sits still.
    int snapshot = (aimValid[SRC_GAZE] ? 1 : 0) | (hand == SRC_GAZE ? 2 : 0)
            | ((ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]) ? 4 : 0)
            | (ctx->pointerAwake ? 8 : 0);
    if (snapshot != ctx->lastSnapshot) {
        ctx->lastSnapshot = snapshot;
        LOGI("input: gaze tracked %d, pointing by gaze %d, pinch %d, awake %d",
             (snapshot & 1) != 0, (snapshot & 2) != 0, (snapshot & 4) != 0,
             (snapshot & 8) != 0);
    }

    // Where the handle is clear of the picture, so a trigger press there cannot
    // have been meant as a click
    int offPicture = hand >= 0 && (hitU[hand] < 0.0f || hitU[hand] > 1.0f
                                   || hitV[hand] < 0.0f || hitV[hand] > 1.0f);
    applyGrab(ctx, aimPoses, aimValid, hand, hover, ctx->hoverCorner, offPicture,
              height, curved);
    screenPose = ctx->screenPose;
    height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    radius = ctx->screenRadius;

    // A handle stays lit while it is being dragged, however far the ray has
    // wandered from it in the meantime
    if (ctx->grabMode == GRAB_MOVE) {
        ctx->hoverKind = HOVER_BAR;
    }
    else if (ctx->grabMode == GRAB_RESIZE) {
        ctx->hoverKind = HOVER_CORNER;
    }
    else {
        ctx->hoverKind = hover;
    }

    ctx->screenOrientation = screenPose.orientation;
    ctx->beamVisible = 0;
    ctx->beamFree = 0;
    // Eyes aim by looking, so a ray out of the face would be nonsense, and a
    // cursor riding on them shakes too much to be anything but a distraction.
    // Gaze draws nothing: the handle lighting up is the feedback.
    ctx->beamGaze = (ctx->grabMode != GRAB_NONE ? ctx->grabHand : hand) == SRC_GAZE;

    if (ctx->grabMode != GRAB_NONE) {
        // Nothing goes to the host mid drag, and the ray ends on the handle
        // being held rather than wherever it is now pointing
        ctx->buttonsDown = 0;
        ctx->scrollCarry = 0.0f;
        ctx->filterU.valid = 0;
        ctx->filterV.valid = 0;

        if (headValid) {
            Vec3 local;
            local.z = 0.0f;
            if (ctx->grabMode == GRAB_MOVE) {
                local.x = 0.0f;
                local.y = -(height * 0.5f + (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f)
                            * ctx->screenWidth);
            }
            else {
                local.x = (ctx->grabOppX > 0.0f ? -0.5f : 0.5f) * ctx->screenWidth;
                local.y = (ctx->grabOppY > 0.0f ? -0.5f : 0.5f) * height;
            }
            Vec3 handle = quatRotate(screenPose.orientation, local);
            ctx->beamStart = aimPoses[ctx->grabHand].position;
            ctx->beamEnd.x = screenPose.position.x + handle.x;
            ctx->beamEnd.y = screenPose.position.y + handle.y;
            ctx->beamEnd.z = screenPose.position.z + handle.z;
            ctx->beamVisible = 1;
        }

        writeInputPose(ctx, out);
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    if (!ctx->pointerOn) {
        // The ray still shows on the handles and the grid, so the screen can
        // be tidied and the environment changed with the mouse switched off
        if (hand >= 0 && hover != HOVER_NONE && hover != HOVER_SCREEN) {
            Vec3 end = furniturePoint(ctx, hover, hitU[hand], hitV[hand], screenPose,
                                      height, radius, curved);
            ctx->beamStart = aimPoses[hand].position;
            ctx->beamEnd.x = end.x;
            ctx->beamEnd.y = end.y;
            ctx->beamEnd.z = end.z;
            ctx->beamVisible = headValid;
        }
        ctx->buttonsDown = 0;
        writeInputPose(ctx, out);
        (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
        return;
    }

    // The bar, the button and the picker all sit off the picture, so pointing
    // at them must not drag the host cursor to the edge
    int hit = (hover == HOVER_SCREEN || hover == HOVER_CORNER) && hand != SRC_GAZE;
    if ((hover == HOVER_BAR || hover == HOVER_ENVBUTTON || hover == HOVER_PICKER
            || hover == HOVER_LOCK || hover == HOVER_HALO) && headValid && hand >= 0) {
        Vec3 end = furniturePoint(ctx, hover, hitU[hand], hitV[hand], screenPose,
                                  height, radius, curved);
        ctx->beamStart = aimPoses[hand].position;
        ctx->beamEnd.x = end.x;
        ctx->beamEnd.y = end.y;
        ctx->beamEnd.z = end.z;
        ctx->beamVisible = 1;
    }
    if (hit) {
        // Filtering across a gap or a change of hands would slide the cursor
        // in from wherever it used to be
        if (hand != ctx->lastHand || now - ctx->lastHitNs > POINTER_RESET_NS) {
            ctx->filterU.valid = 0;
            ctx->filterV.valid = 0;
        }
        ctx->lastHand = hand;
        ctx->lastHitNs = now;

        float u = euroFilter(&ctx->filterU, hitU[hand], dt, ctx->pointerMinCutoff, ctx->pointerBeta);
        float v = euroFilter(&ctx->filterV, hitV[hand], dt, ctx->pointerMinCutoff, ctx->pointerBeta);
        out[IN_HIT] = 1.0f;
        out[IN_U] = u;
        out[IN_V] = v;

        // The ray is only drawn when it lands on something, which is what
        // makes a laser readable rather than a light show
        Vec3 endPoint = screenPoint(u, v, screenPose, ctx->screenWidth, height, radius, curved);
        ctx->beamStart = aimPoses[hand].position;
        ctx->beamEnd.x = endPoint.x;
        ctx->beamEnd.y = endPoint.y;
        ctx->beamEnd.z = endPoint.z;
        ctx->beamVisible = headValid;
    }

    int mask = 0;
    if (ctx->triggerDown[HAND_LEFT] || ctx->triggerDown[HAND_RIGHT]) {
        mask |= VR_BUTTON_LEFT;
    }
    if (actionBool(ctx, ctx->rightClickAction, -1)) {
        mask |= VR_BUTTON_RIGHT;
    }
    if (actionBool(ctx, ctx->middleClickAction, -1)) {
        mask |= VR_BUTTON_MIDDLE;
    }
    // A press only counts while aimed at the screen, but a release always
    // does, so walking the pointer off the edge mid drag still lets go
    ctx->buttonsDown = (ctx->buttonsDown & mask) | (hit ? mask : 0);
    out[IN_BUTTONS] = (float)ctx->buttonsDown;

    XrVector2f stick = actionVec2(ctx, ctx->scrollAction, -1);
    if (hit && fabsf(stick.y) > SCROLL_DEADZONE) {
        float past = (fabsf(stick.y) - SCROLL_DEADZONE) / (1.0f - SCROLL_DEADZONE);
        ctx->scrollCarry += copysignf(past * SCROLL_CLICKS_PER_SEC * dt, stick.y);
    }
    else {
        ctx->scrollCarry = 0.0f;
    }
    float clicks = truncf(ctx->scrollCarry);
    ctx->scrollCarry -= clicks;
    out[IN_SCROLL] = clicks;

    // Aimed at nothing at all, so the ray runs off into the room rather than
    // blinking out. A laser that comes and goes is harder to aim than one that
    // always shows where the hand is looking, so the only thing that retires it
    // is the controller being put down.
    if (!ctx->beamVisible && ctx->pointerAwake && headValid && !ctx->beamGaze) {
        int free = hand;
        if (free < 0) {
            free = aimValid[HAND_RIGHT] ? HAND_RIGHT : (aimValid[HAND_LEFT] ? HAND_LEFT : -1);
        }
        if (free >= 0) {
            Vec3 forward = { 0.0f, 0.0f, -1.0f };
            Vec3 d = quatRotate(aimPoses[free].orientation, forward);
            ctx->beamStart = aimPoses[free].position;
            ctx->beamEnd.x = ctx->beamStart.x + d.x * FREE_BEAM_M;
            ctx->beamEnd.y = ctx->beamStart.y + d.y * FREE_BEAM_M;
            ctx->beamEnd.z = ctx->beamStart.z + d.z * FREE_BEAM_M;
            ctx->beamVisible = 1;
            // No target, so no cursor. The dot is what says a click would
            // land somewhere.
            ctx->beamFree = 1;
        }
    }

    writeInputPose(ctx, out);
    out[IN_PICKER_PICK] = (float)ctx->pickerPick;
    (*env)->SetFloatArrayRegion(env, outArr, 0, IN_SLOTS, out);
}

// Integer valued tuning property, left alone if unset or unparseable
static void propScaled(const char* name, float* target, float scale, long maxRaw) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end != value && v >= 0 && v <= maxRaw) {
        *target = v * scale;
    }
}

static void propPercent(const char* name, float* target) {
    propScaled(name, target, 0.01f, 100);
}

static void propFlag(const char* name, int* target) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    *target = value[0] != '0';
}

// Fires once each time the property is set to a value it has not seen. The
// value becomes the filename tag, so setprop 1, 2, 3 gives three captures.
static void pollCaptureRequest(XrCtx* ctx) {
    if (++ctx->capturePollCounter < CAPTURE_POLL_FRAMES) {
        return;
    }
    ctx->capturePollCounter = 0;

    propPercent(PROP_DEPTH_ALPHA, &ctx->depthAlpha);
    propPercent(PROP_RANGE_ALPHA, &ctx->rangeAlpha);
    propPercent(PROP_UPSAMPLE_SIGMA, &ctx->upsampleSigmaR);
    propPercent(PROP_DEPTH_SHARP, &ctx->depthSharp);
    propFlag(PROP_OVERLAY, &ctx->overlayVisible);
    propFlag(PROP_UPSAMPLE, &ctx->upsampleEnabled);
    propFlag(PROP_OCCLUSION, &ctx->occlusionEnabled);
    propPercent(PROP_CONVERGENCE, &ctx->convergence);
    // Same tenths of a percent of frame width the preference uses
    propScaled(PROP_SEPARATION, &ctx->separationOverride, 0.001f, 50);
    // Tenths of a metre, the same units the preferences use
    propScaled(PROP_DISTANCE, &ctx->distanceOverride, 0.1f, 80);
    propScaled(PROP_SCREEN, &ctx->screenOverride, 0.1f, 120);
    propPercent(PROP_DEPTH_GLOBAL, &ctx->depthGlobal);
    propScaled(PROP_DEPTH_LOCAL, &ctx->depthLocal, 0.01f, 400);
    // Tenths of a Hz, and half units of speed sensitivity
    propScaled(PROP_POINTER_CUTOFF, &ctx->pointerMinCutoff, 0.1f, 200);
    propScaled(PROP_POINTER_BETA, &ctx->pointerBeta, 0.5f, 100);
    // Millimetres
    propScaled(PROP_BEAM_WIDTH, &ctx->beamWidth, 0.001f, 100);
    // Tenths of a second
    propScaled(PROP_POINTER_WAKE, &ctx->pointerWake, 0.1f, 100);
    propScaled(PROP_POINTER_SLEEP, &ctx->pointerSleep, 0.1f, 600);
    // Metres. Zero is the infinite sphere the layer starts out as.
    propScaled(PROP_ENV_RADIUS, &ctx->envRadius, 1.0f, 200);

    if (ctx->captureDir[0] == '\0') {
        return;
    }

    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(CAPTURE_PROP, value) <= 0 || value[0] == '\0') {
        return;
    }
    if (strcmp(value, ctx->lastCaptureTag) == 0) {
        return;
    }
    strncpy(ctx->lastCaptureTag, value, sizeof(ctx->lastCaptureTag) - 1);
    strncpy(ctx->captureTag, value, sizeof(ctx->captureTag) - 1);
    ctx->captureRequested = 1;
    LOGI("capture: request %s", ctx->captureTag);
}

static void writeCapture(XrCtx* ctx, const char* what, const void* data, size_t bytes) {
    if (data == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/cap_%s_%s.raw", ctx->captureDir, ctx->captureTag, what);
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        LOGE("capture: cannot write %s", path);
        return;
    }
    size_t written = fwrite(data, 1, bytes, f);
    fclose(f);
    LOGI("capture: %s %zu bytes", path, written);
}

// Reads back the depth texture the warp actually sampled this frame, so the
// captured warp can be reproduced exactly rather than approximately. Depth is
// the alpha channel, the rgb alongside it is the guide.
static void writeCaptureDepthTexture(XrCtx* ctx) {
    const int n = DEPTH_TEX_SIZE;
    unsigned char* rgba = malloc((size_t)n * n * 4);
    unsigned char* red = malloc((size_t)n * n);
    if (rgba == NULL || red == NULL) {
        free(rgba);
        free(red);
        return;
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->depthTextures[ctx->depthReadIndex], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        for (int i = 0; i < n * n; i++) {
            red[i] = rgba[i * 4 + 3];
        }
        writeCapture(ctx, "depthtex", red, (size_t)n * n);
        writeCapture(ctx, "guidetex", rgba, (size_t)n * n * 4);
    }
    else {
        LOGW("capture: depth texture not readable");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    free(rgba);
    free(red);
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

static void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation) {
    int upsampling = ctx->stereoMode == DEPTH_MODE_MODEL && ctx->upsampleEnabled;
    int occluding = upsampling && ctx->occlusionEnabled && separation > 0.0f;

    // Capture frames do readbacks and file writes inside what would be the
    // query window, which both ruins the number and, on this driver, leaves a
    // query that never becomes available. Skip timing them.
    int timing = ctx->timerSupported && !ctx->captureRequested;

    if (timing && !ctx->timerPending[ctx->timerSlot]) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->timerQueries[ctx->timerSlot]);
    }

    if (upsampling) {
        runUpsample(ctx, texMatrix);
    }
    if (occluding) {
        runOffsetSearch(ctx, separation);
    }

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
                ctx->gpuTotalNs += (long)elapsed;
                ctx->gpuSamples++;
                ctx->overlayGpuTotalNs += (long)elapsed;
                ctx->overlayGpuSamples++;
                if ((long)elapsed > ctx->gpuMaxNs) {
                    ctx->gpuMaxNs = (long)elapsed;
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

    ctx->everRendered = 1;
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
    if (grid != NULL) {
        const unsigned char* px = (*env)->GetDirectBufferAddress(env, grid);
        if (px != NULL) {
            ctx->pickerReady = uploadFlipped(ctx, ctx->pickerSwapchain, ctx->pickerImages,
                                             px, PICKER_TEX_W, PICKER_TEX_H);
        }
    }
    if (button != NULL) {
        const unsigned char* px = (*env)->GetDirectBufferAddress(env, button);
        if (px != NULL) {
            ctx->envButtonReady = uploadFlipped(ctx, ctx->envButtonSwapchain,
                                                ctx->envButtonImages, px,
                                                OUTLINE_TEX, OUTLINE_TEX);
        }
    }
    LOGI("picker art %s, button %s", ctx->pickerReady ? "ready" : "missing",
         ctx->envButtonReady ? "ready" : "missing");
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
        xrDestroySwapchain(ctx->backgroundSwapchain);
        ctx->backgroundSwapchain = XR_NULL_HANDLE;
        free(ctx->backgroundImages);
        ctx->backgroundImages = NULL;
        ctx->backgroundReady = 0;
    }

    if (ctx->backgroundSwapchain != XR_NULL_HANDLE) {
        ctx->backgroundReady = uploadFlipped(ctx, ctx->backgroundSwapchain,
                                             ctx->backgroundImages, px, width, height);
        return;
    }

    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = ctx->swapchainFormat;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->backgroundSwapchain),
                 "create background swapchain")) {
        ctx->backgroundSwapchain = XR_NULL_HANDLE;
        return;
    }

    xrEnumerateSwapchainImages(ctx->backgroundSwapchain, 0, &ctx->backgroundImageCount, NULL);
    ctx->backgroundImages = calloc(ctx->backgroundImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
    for (uint32_t i = 0; i < ctx->backgroundImageCount; i++) {
        ctx->backgroundImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    xrEnumerateSwapchainImages(ctx->backgroundSwapchain, ctx->backgroundImageCount,
                               &ctx->backgroundImageCount,
                               (XrSwapchainImageBaseHeader*)ctx->backgroundImages);

    ctx->backgroundReady = uploadFlipped(ctx, ctx->backgroundSwapchain, ctx->backgroundImages,
                                         px, width, height);
    ctx->backgroundWidth = width;
    ctx->backgroundHeight = height;
    LOGI("background %dx%d %s", width, height, ctx->backgroundReady ? "ready" : "failed");
}

// Puts back a placement saved from a previous session. Marking the sliders as
// already seen stops the first frame taking the screen straight back off it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetScreenPose(JNIEnv* env, jobject thiz,
                                                                jlong handle, jfloatArray poseArr) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || poseArr == NULL) {
        return;
    }
    float p[9];
    if ((*env)->GetArrayLength(env, poseArr) < 9) {
        return;
    }
    (*env)->GetFloatArrayRegion(env, poseArr, 0, 9, p);

    if (p[7] < SCREEN_MIN_WIDTH || p[7] > SCREEN_MAX_WIDTH || p[8] <= 0.0f) {
        LOGW("stored screen placement out of range, ignoring it");
        return;
    }

    ctx->screenPose.position.x = p[0];
    ctx->screenPose.position.y = p[1];
    ctx->screenPose.position.z = p[2];
    ctx->screenPose.orientation.x = p[3];
    ctx->screenPose.orientation.y = p[4];
    ctx->screenPose.orientation.z = p[5];
    ctx->screenPose.orientation.w = p[6];
    ctx->screenPose.orientation = quatNorm(ctx->screenPose.orientation);
    ctx->screenWidth = p[7];
    ctx->screenRadius = p[8];
    ctx->placementValid = 1;
    ctx->sliderSeen = 0;
    LOGI("restored screen placement %.2f %.2f %.2f, %.2f m wide",
         p[0], p[1], p[2], p[7]);
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

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->overlaySwapchain, &acquireInfo, &imageIndex),
                 "acquire overlay image")) {
        return;
    }
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->overlaySwapchain, &waitInfo);

    glBindTexture(GL_TEXTURE_2D, ctx->overlayImages[imageIndex].image);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->overlaySwapchain, &releaseInfo);
    ctx->overlayHasContent = 1;
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

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeEndFrame(JNIEnv* env, jobject thiz, jlong handle,
                                                           jboolean newFrame, jfloatArray texMatrixArr,
                                                           jfloat distance, jfloat quadWidth,
                                                           jfloat curvature, jboolean headLocked,
                                                           jfloat separation, jboolean eyeSwap,
                                                           jboolean passthrough) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;

    ctx->passthrough = passthrough;
    pollCaptureRequest(ctx);
    propFlag(PROP_PASSTHROUGH, &ctx->passthrough);
    if (ctx->separationOverride >= 0.0f) {
        separation = ctx->separationOverride;
    }
    if (ctx->distanceOverride > 0.0f) {
        distance = ctx->distanceOverride;
    }
    if (ctx->screenOverride > 0.0f) {
        quadWidth = ctx->screenOverride;
    }

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
            // Submit is the wall clock around the draw calls, which is only
            // how long the driver took to queue them. GPU is the real cost.
            if (ctx->gpuSamples > 0) {
                LOGI("XR warp: %ld frames, GPU avg %.2f ms, GPU max %.2f ms, submit avg %.2f ms",
                     ctx->statFrames, ctx->gpuTotalNs / (double)ctx->gpuSamples / 1e6,
                     ctx->gpuMaxNs / 1e6,
                     ctx->statTotalNs / (double)ctx->statFrames / 1e6);
            }
            else {
                LOGI("XR warp: %ld frames, submit avg %.2f ms, max %.2f ms (no GPU timer)",
                     ctx->statFrames, ctx->statTotalNs / (double)ctx->statFrames / 1e6,
                     ctx->statMaxNs / 1e6);
            }
            ctx->statFrames = 0;
            ctx->statTotalNs = 0;
            ctx->statMaxNs = 0;
            ctx->gpuTotalNs = 0;
            ctx->gpuMaxNs = 0;
            ctx->gpuSamples = 0;
        }
    }

    float aspect = (float)ctx->videoHeight / (float)ctx->videoWidth;
    XrSpace space = headLocked ? ctx->viewSpace : ctx->localSpace;
    int stereo = ctx->stereoMode != DEPTH_MODE_OFF;

    if (!ctx->pointerArtReady && ctx->pointerSwapchain != XR_NULL_HANDLE && ctx->shouldRender) {
        uploadPointerArt(ctx);
    }

    updatePlacement(ctx, distance, quadWidth, curvature);
    XrPosef screenPose = ctx->screenPose;
    float screenWidth = ctx->screenWidth;
    float screenHeight = screenWidth * aspect;

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = ctx->predictedDisplayTime;
    endInfo.environmentBlendMode = (ctx->passthrough && ctx->alphaBlendSupported)
            ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerEquirect2KHR backgroundLayer;
    XrCompositionLayerQuad quadLayers[2];
    XrCompositionLayerCylinderKHR cylLayers[2];
    XrCompositionLayerQuad overlayLayer;
    XrCompositionLayerQuad beamLayer;
    XrCompositionLayerQuad dotLayer;
    XrCompositionLayerQuad handleLayer;
    XrCompositionLayerQuad envButtonLayer;
    XrCompositionLayerQuad lockLayer;
    XrCompositionLayerQuad pickerLayer;
    XrCompositionLayerQuad outlineLayers[2];
    const XrCompositionLayerBaseHeader* layers[16];
    uint32_t layerCount = 0;

    // Submitted first so everything else sits in front of it. Passthrough wants
    // the room instead, so the two are mutually exclusive.
    if (ctx->backgroundReady && ctx->backgroundEnabled && !ctx->passthrough) {
        memset(&backgroundLayer, 0, sizeof(backgroundLayer));
        backgroundLayer.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR;
        backgroundLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        backgroundLayer.subImage.swapchain = ctx->backgroundSwapchain;
        backgroundLayer.subImage.imageRect.offset.x = 0;
        backgroundLayer.subImage.imageRect.offset.y = 0;
        backgroundLayer.subImage.imageRect.extent.width = ctx->backgroundWidth;
        backgroundLayer.subImage.imageRect.extent.height = ctx->backgroundHeight;
        backgroundLayer.subImage.imageArrayIndex = 0;
        // World locked, even when the screen is head locked, or the environment
        // would swing about with the viewer
        backgroundLayer.space = ctx->localSpace;
        backgroundLayer.pose.orientation.w = 1.0f;
        // A finite sphere is what gives the room a size. At zero the layer is
        // infinitely far, so leaning about moves nothing and the eye reads it
        // as vast. Bring it in and the parallax says how big it really is.
        backgroundLayer.radius = ctx->envRadius;
        backgroundLayer.centralHorizontalAngle = 6.2831853f;
        // Width covers the full turn, so the vertical reach follows the aspect
        // ratio. A 2:1 image fills the sphere, anything wider leaves the zenith
        // and nadir empty rather than stretching to cover them.
        float halfV = (float)ctx->backgroundHeight / (float)ctx->backgroundWidth * 3.1415927f;
        if (halfV > 1.5707963f) {
            halfV = 1.5707963f;
        }
        backgroundLayer.upperVerticalAngle = halfV;
        backgroundLayer.lowerVerticalAngle = -halfV;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&backgroundLayer;
    }

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
                float radius = ctx->screenRadius;
                cyl->eyeVisibility = visibility;
                cyl->subImage = subImage;
                cyl->space = space;
                cyl->pose.orientation = screenPose.orientation;
                // The layer pose is the axis, which sits a radius behind the
                // surface the placement tracks
                Vec3 axisLocal = { 0.0f, 0.0f, radius };
                Vec3 axis = quatRotate(screenPose.orientation, axisLocal);
                cyl->pose.position.x = screenPose.position.x + axis.x;
                cyl->pose.position.y = screenPose.position.y + axis.y;
                cyl->pose.position.z = screenPose.position.z + axis.z;
                cyl->radius = radius;
                cyl->centralAngle = screenWidth / radius;
                cyl->aspectRatio = 1.0f / aspect;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)cyl;
            }
            else {
                XrCompositionLayerQuad* quad = &quadLayers[eye];
                memset(quad, 0, sizeof(*quad));
                quad->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                quad->eyeVisibility = visibility;
                quad->subImage = subImage;
                quad->space = space;
                quad->pose = screenPose;
                quad->size.width = screenWidth;
                quad->size.height = screenHeight;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)quad;
            }
        }

        // Stats sit in the top left corner of the screen, same space and
        // distance, both eyes, so they read at screen depth with no disparity
        if (ctx->overlayHasContent && ctx->overlayVisible
                && ctx->overlaySwapchain != XR_NULL_HANDLE) {
            float overlayW = screenWidth * 0.30f;
            float overlayH = overlayW * (float)OVERLAY_HEIGHT / (float)OVERLAY_WIDTH;
            float margin = screenWidth * 0.02f;

            memset(&overlayLayer, 0, sizeof(overlayLayer));
            overlayLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            overlayLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            overlayLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            overlayLayer.subImage.swapchain = ctx->overlaySwapchain;
            overlayLayer.subImage.imageRect.offset.x = 0;
            overlayLayer.subImage.imageRect.offset.y = 0;
            overlayLayer.subImage.imageRect.extent.width = OVERLAY_WIDTH;
            overlayLayer.subImage.imageRect.extent.height = OVERLAY_HEIGHT;
            overlayLayer.subImage.imageArrayIndex = 0;
            overlayLayer.space = space;
            // Pinned to the top left of the screen in the screen's own frame,
            // so it follows wherever the screen has been moved to
            Vec3 statsLocal = { -screenWidth * 0.5f + overlayW * 0.5f + margin,
                                screenHeight * 0.5f - overlayH * 0.5f - margin,
                                // A little in front so the two never z fight
                                0.01f };
            Vec3 stats = quatRotate(screenPose.orientation, statsLocal);
            overlayLayer.pose.orientation = screenPose.orientation;
            overlayLayer.pose.position.x = screenPose.position.x + stats.x;
            overlayLayer.pose.position.y = screenPose.position.y + stats.y;
            overlayLayer.pose.position.z = screenPose.position.z + stats.z;
            overlayLayer.size.width = overlayW;
            overlayLayer.size.height = overlayH;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&overlayLayer;
        }

        // The bar and the environment button share a hover area, so reaching
        // for one keeps the other on screen rather than swapping them
        int barArea = ctx->hoverKind == HOVER_BAR || ctx->hoverKind == HOVER_ENVBUTTON;

        // Move bar and resize corner, shown only while the ray is over them.
        // Both live in the screen's own frame, so they travel with it.
        if (ctx->handleArtReady && (barArea || ctx->hoverKind == HOVER_CORNER)) {
            int isBar = barArea;
            Vec3 local;
            float sizeW, sizeH;
            float roll = 0.0f;

            if (isBar) {
                sizeW = screenWidth * BAR_WIDTH_FRAC;
                sizeH = screenWidth * BAR_HEIGHT_FRAC;
                local.x = 0.0f;
                local.y = -(screenHeight * 0.5f + screenWidth * BAR_GAP_FRAC + sizeH * 0.5f);
            }
            else {
                sizeW = sizeH = screenWidth * CORNER_FRAC;
                int right = ctx->hoverCorner == 1 || ctx->hoverCorner == 3;
                int bottom = ctx->hoverCorner >= 2;
                local.x = (right ? 0.5f : -0.5f) * screenWidth;
                local.y = (bottom ? -0.5f : 0.5f) * screenHeight;
                // The art is a top left bracket, so the other three are the
                // same picture rolled about the screen normal
                if (ctx->hoverCorner == 1) roll = -1.5707963f;
                else if (ctx->hoverCorner == 2) roll = 1.5707963f;
                else if (ctx->hoverCorner == 3) roll = 3.1415927f;
            }
            // Just off the surface so it never z fights the picture
            local.z = 0.005f;

            XrQuaternionf rollQ = { 0.0f, 0.0f, sinf(roll * 0.5f), cosf(roll * 0.5f) };
            Vec3 offset = quatRotate(screenPose.orientation, local);

            memset(&handleLayer, 0, sizeof(handleLayer));
            handleLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            handleLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            handleLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            handleLayer.subImage.swapchain = isBar ? ctx->barSwapchain : ctx->cornerSwapchain;
            handleLayer.subImage.imageRect.offset.x = 0;
            handleLayer.subImage.imageRect.offset.y = 0;
            handleLayer.subImage.imageRect.extent.width = isBar ? BAR_TEX_W : CORNER_TEX_W;
            handleLayer.subImage.imageRect.extent.height = isBar ? BAR_TEX_H : CORNER_TEX_H;
            handleLayer.subImage.imageArrayIndex = 0;
            handleLayer.space = space;
            handleLayer.pose.orientation = quatNorm(quatMul(screenPose.orientation, rollQ));
            handleLayer.pose.position.x = screenPose.position.x + offset.x;
            handleLayer.pose.position.y = screenPose.position.y + offset.y;
            handleLayer.pose.position.z = screenPose.position.z + offset.z;
            handleLayer.size.width = sizeW;
            handleLayer.size.height = sizeH;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&handleLayer;
        }

        // The button that opens the environment grid, left of the move bar.
        // Stays up while the grid is open so it reads as the thing that
        // opened it.
        if (ctx->envButtonReady && (barArea || ctx->pickerOpen)) {
            Vec3 local;
            float side;
            envButtonPlacement(ctx, screenHeight, &local, &side);
            Vec3 offset = quatRotate(screenPose.orientation, local);

            memset(&envButtonLayer, 0, sizeof(envButtonLayer));
            envButtonLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            envButtonLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            envButtonLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            envButtonLayer.subImage.swapchain = ctx->envButtonSwapchain;
            envButtonLayer.subImage.imageRect.offset.x = 0;
            envButtonLayer.subImage.imageRect.offset.y = 0;
            envButtonLayer.subImage.imageRect.extent.width = OUTLINE_TEX;
            envButtonLayer.subImage.imageRect.extent.height = OUTLINE_TEX;
            envButtonLayer.subImage.imageArrayIndex = 0;
            envButtonLayer.space = space;
            envButtonLayer.pose.orientation = screenPose.orientation;
            envButtonLayer.pose.position.x = screenPose.position.x + offset.x;
            envButtonLayer.pose.position.y = screenPose.position.y + offset.y;
            envButtonLayer.pose.position.z = screenPose.position.z + offset.z;
            // Grows a little when the ray is on it, which is the only feedback
            // a quad layer can give without a second texture
            float scale = ctx->envButtonHot ? 1.18f : 1.0f;
            envButtonLayer.size.width = side * scale;
            envButtonLayer.size.height = side * scale;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&envButtonLayer;
        }

        // The padlock. Comes and goes like the rest of the furniture rather
        // than sitting there permanently, so it costs nothing to look at while
        // playing. Reaching for the bar shows it too, since that is where
        // people go looking when they want to change something.
        if (ctx->handsEnabled && ctx->lockArtReady
                && (ctx->hoverKind == HOVER_LOCK || barArea)) {
            Vec3 local;
            float side;
            lockButtonPlacement(ctx, &local, &side);
            Vec3 offset = quatRotate(screenPose.orientation, local);

            memset(&lockLayer, 0, sizeof(lockLayer));
            lockLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            lockLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            lockLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            lockLayer.subImage.swapchain = ctx->handsLocked ? ctx->lockSwapchain
                                                            : ctx->unlockSwapchain;
            lockLayer.subImage.imageRect.offset.x = 0;
            lockLayer.subImage.imageRect.offset.y = 0;
            lockLayer.subImage.imageRect.extent.width = LOCK_TEX;
            lockLayer.subImage.imageRect.extent.height = LOCK_TEX;
            lockLayer.subImage.imageArrayIndex = 0;
            lockLayer.space = space;
            lockLayer.pose.orientation = screenPose.orientation;
            lockLayer.pose.position.x = screenPose.position.x + offset.x;
            lockLayer.pose.position.y = screenPose.position.y + offset.y;
            lockLayer.pose.position.z = screenPose.position.z + offset.z;
            float lockScale = ctx->lockHot ? 1.18f : 1.0f;
            lockLayer.size.width = side * lockScale;
            lockLayer.size.height = side * lockScale;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&lockLayer;
        }

        // The environment grid, floating in front of the screen, with the
        // hovered and the chosen cell ringed
        if (ctx->pickerOpen && ctx->pickerReady) {
            float pickW, pickH;
            XrPosef pickPose = pickerPose(ctx, &pickW, &pickH);

            memset(&pickerLayer, 0, sizeof(pickerLayer));
            pickerLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            pickerLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            pickerLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            pickerLayer.subImage.swapchain = ctx->pickerSwapchain;
            pickerLayer.subImage.imageRect.offset.x = 0;
            pickerLayer.subImage.imageRect.offset.y = 0;
            pickerLayer.subImage.imageRect.extent.width = PICKER_TEX_W;
            pickerLayer.subImage.imageRect.extent.height = PICKER_TEX_H;
            pickerLayer.subImage.imageArrayIndex = 0;
            pickerLayer.space = space;
            pickerLayer.pose = pickPose;
            pickerLayer.size.width = pickW;
            pickerLayer.size.height = pickH;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&pickerLayer;

            if (ctx->outlineReady) {
                float cellW = pickW / (float)PICKER_COLS;
                float cellH = pickH / (float)PICKER_ROWS;
                // Hover rings the cell, the choice sits inside it, so both
                // read at once when the ray is over what is already selected
                int marks[2] = { ctx->pickerHover, ctx->pickerChoice };
                float scales[2] = { 1.0f, 0.84f };

                for (int m = 0; m < 2; m++) {
                    int cell = marks[m];
                    if (cell < 0 || cell >= PICKER_CELLS) {
                        continue;
                    }
                    int col = cell % PICKER_COLS;
                    int row = cell / PICKER_COLS;
                    Vec3 local;
                    local.x = ((col + 0.5f) / PICKER_COLS - 0.5f) * pickW;
                    local.y = (0.5f - (row + 0.5f) / PICKER_ROWS) * pickH;
                    local.z = 0.004f;
                    Vec3 offset = quatRotate(pickPose.orientation, local);

                    XrCompositionLayerQuad* mark = &outlineLayers[m];
                    memset(mark, 0, sizeof(*mark));
                    mark->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                    mark->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    mark->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    mark->subImage.swapchain = ctx->outlineSwapchain;
                    mark->subImage.imageRect.offset.x = 0;
                    mark->subImage.imageRect.offset.y = 0;
                    mark->subImage.imageRect.extent.width = OUTLINE_TEX;
                    mark->subImage.imageRect.extent.height = OUTLINE_TEX;
                    mark->subImage.imageArrayIndex = 0;
                    mark->space = space;
                    mark->pose.orientation = pickPose.orientation;
                    mark->pose.position.x = pickPose.position.x + offset.x;
                    mark->pose.position.y = pickPose.position.y + offset.y;
                    mark->pose.position.z = pickPose.position.z + offset.z;
                    mark->size.width = cellW * scales[m];
                    mark->size.height = cellH * scales[m];
                    layers[layerCount++] = (const XrCompositionLayerBaseHeader*)mark;
                }
            }
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

            if (length > 0.10f) {
                beamX = vecNorm(beamX);
                Vec3 beamZ = vecCross(beamX, beamY);

                memset(&beamLayer, 0, sizeof(beamLayer));
                beamLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                beamLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                beamLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                beamLayer.subImage.swapchain = ctx->pointerSwapchain;
                beamLayer.subImage.imageRect.offset.x = 0;
                beamLayer.subImage.imageRect.offset.y = 0;
                beamLayer.subImage.imageRect.extent.width = PTR_TEX_W;
                beamLayer.subImage.imageRect.extent.height = PTR_BEAM_H;
                beamLayer.subImage.imageArrayIndex = 0;
                beamLayer.space = space;
                beamLayer.pose.orientation = quatFromBasis(beamX, beamY, beamZ);
                beamLayer.pose.position.x = mid.x;
                beamLayer.pose.position.y = mid.y;
                beamLayer.pose.position.z = mid.z;
                beamLayer.size.width = ctx->beamWidth;
                beamLayer.size.height = length;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&beamLayer;

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

                memset(&dotLayer, 0, sizeof(dotLayer));
                dotLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                dotLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                dotLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                dotLayer.subImage.swapchain = ctx->pointerSwapchain;
                dotLayer.subImage.imageRect.offset.x = 0;
                dotLayer.subImage.imageRect.offset.y = PTR_BEAM_H;
                dotLayer.subImage.imageRect.extent.width = PTR_TEX_W;
                dotLayer.subImage.imageRect.extent.height = PTR_DOT_H;
                dotLayer.subImage.imageArrayIndex = 0;
                dotLayer.space = space;
                dotLayer.pose.orientation = quatFromBasis(dotX, dotY, dotZ);
                dotLayer.pose.position.x = end.x + dotZ.x * 0.012f;
                dotLayer.pose.position.y = end.y + dotZ.y * 0.012f;
                dotLayer.pose.position.z = end.z + dotZ.z * 0.012f;
                dotLayer.size.width = 0.022f;
                dotLayer.size.height = 0.022f;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&dotLayer;
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
