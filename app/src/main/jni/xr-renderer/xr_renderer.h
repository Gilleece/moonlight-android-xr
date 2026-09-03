
// OpenXR presentation for the decoded video stream. The decoder feeds a
// SurfaceTexture whose OES texture lives in the EGL context created here.
// Each new video frame is drawn into a single swapchain that the compositor
// shows on a quad (or cylinder) visible to both eyes, so the compositor does
// all the reprojection work. The 3d room is the one exception: it is real
// geometry drawn per eye into a projection layer, in place of the environment.

#ifndef XR_RENDERER_H
#define XR_RENDERER_H

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
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

#include "xr_math.h"
#include "xr_shared.h"

#define TAG "moonlight-xr"

// Not an Android priority, only ever seen inside xrLog: a key event, which
// logcat gets as info while the file keeps it at the quiet level
#define PRIO_EVENT 90

void xrLog(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGI(...) xrLog(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) xrLog(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) xrLog(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGEV(...) xrLog(PRIO_EVENT, __VA_ARGS__)

static inline long nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

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
// A gap this long means the pointer left the screen or changed hands, and
// filtering across it would slide the cursor in from where it used to be
#define POINTER_RESET_NS 250000000L

// The aim pose gets its own filter so the hover zones, grabs and beam
// origin settle along with the cursor. Position and rotation share the
// tunables since their derivatives are the same order of magnitude.
#define AIM_MIN_CUTOFF 1.0f
#define AIM_BETA 20.0f

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

// Widened along with the grid so a cell stays about the size it was at three
// columns
#define PICKER_WIDTH_FRAC 0.73f
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

#define COG_WIDTH_FRAC 0.36f
// The button that opens it, sitting to the right of the move bar, the same
// size as the environment button on the left
#define COG_BUTTON_FRAC 0.048f
#define COG_THUMB_TEX 64

// Metres. Deliberately well under the settings slider's 1 m floor, so the
// screen can be brought right up to the face.
#define COG_DIST_MIN 0.2f
#define COG_DIST_MAX 8.0f
// Metres either side of the reference space's eye level
#define COG_HEIGHT_MIN -2.0f
#define COG_HEIGHT_MAX 2.0f
// Radians, 40 degrees each way
#define COG_TILT_MAX 0.6981f
// The same fraction of the track the roll snap covers, TILT_MAX * ROLL_SNAP /
// ROLL_MAX, so both rows clip to level over the same distance under the ray
#define COG_TILT_SNAP 0.0388f
// Radians, 90 degrees each way, so the picture can go fully on its side for
// watching while lying down
#define COG_ROLL_MAX 1.5708f
// Anything inside 5 degrees of level clips to exactly level. Ninety degrees
// spread over a short track is far too coarse to land on zero by hand, and
// getting the picture properly level is most of what this row is for.
#define COG_ROLL_SNAP 0.0873f

#define KB_WIDTH_FRAC 0.55f
#define KB_MAX_KEYS 64

#define EXIT_WIDTH_FRAC 0.30f

#define HOVER_ENVBUTTON 4
#define HOVER_PICKER    5
// Nothing under the ray, but close enough to the screen to keep drawing it
#define HOVER_HALO      6
#define HOVER_LOCK      7
#define HOVER_COGBUTTON 8
#define HOVER_COGPANEL  9
#define HOVER_KBBUTTON  10
#define HOVER_KBPANEL   11
#define HOVER_EXITBUTTON 12
#define HOVER_EXITPROMPT 13
// How far past each edge that reaches, as a fraction of the screen
#define HALO_FRAC 0.5f
// How far the ray runs when it is aimed at nothing at all, in metres
#define FREE_BEAM_M 4.0f

// Radius of the environment sphere in metres. Finite, so leaning gives the
// room a size instead of it sitting infinitely far off.
#define ENV_RADIUS_M 12.0f

// Ambilight. The frame is boiled down to a tiny colour texture once a frame,
// and a soft quad behind the screen is filled from it, so whatever the picture
// is sitting in front of picks up the colours on it.
#define AMBI_SAMPLE_TEX 32
#define GLOW_TEX 256
// How much larger than the screen the glow quad is, and how far behind it
// sits in metres
#define GLOW_SCALE 1.7f
#define GLOW_BEHIND_M 0.05f

// Letterbox and pillarbox. A movie arrives with black bars baked into the
// frame, and sampling the frame's true edges then washes the room in black.
// A second copy of the sample pass is read back now and then, the bars are
// counted off each edge, and the pass that feeds the glow is cropped to the
// picture inside them.
// How many frames of the sample pass between readbacks, so this costs 4 KB a
// couple of times a second
#define AMBI_BAR_PERIOD 30
// Average of max(r,g,b) below which a row or column counts as bar. High enough
// to cover limited range black arriving unexpanded at 16/255.
#define AMBI_BAR_LUMA 0.09f
// Most texels of 32 one edge may eat. Wider than any real aspect ratio needs,
// and it keeps a content region that cannot collapse.
#define AMBI_BAR_MAX 10
// Ticks a larger bar must hold before the crop grows, and ticks a smaller one
// must hold before it shrinks. Growing slowly keeps a dark scene from being
// mistaken for a bar; shrinking sooner gets the picture back quickly when the
// bars really do go.
#define AMBI_BAR_APPLY 4
#define AMBI_BAR_RELEASE 2
// Edge order used by every bar array below, in the plain quad's uv space
#define AMBI_EDGE_LEFT 0
#define AMBI_EDGE_RIGHT 1
#define AMBI_EDGE_BOTTOM 2
#define AMBI_EDGE_TOP 3
#define AMBI_EDGES 4

// The 3d room. A dark interior, drawn per eye into the one projection layer
// this renderer has, instead of the environment sphere. Which room, 0 for none:
// 1 is generated here, 2 is the baked model that ships in the assets.
#define ROOM_STYLE_MINIMAL 1
#define ROOM_STYLE_PSX 2
#define ROOM_EYES 2
// How big the room renders per eye, picked by the Environment Res setting.
// Half of what the runtime recommends was soft enough against the video layer
// beside it to see, so low is only for headsets that cannot hold more, and the
// three tiers above it all take the recommendation and cap it. Ultra is the
// exception and the reason it is marked experimental: it ignores the
// recommendation for a fixed size, so a runtime that asks for much less than
// this ends up drawing a room several times the area it sized itself for.
#define ROOM_MAX_EYE_FULL 2560     // high
#define ROOM_MAX_EYE_STANDARD 1760 // standard
#define ROOM_MAX_EYE 1280          // low, on half the recommendation
#define ROOM_ULTRA_EYE 2800        // ultra, taken rather than capped
// Ten floats a vertex: position, colour, spill weight, texture coordinate and
// one spare
#define ROOM_VERTEX_FLOATS 10
#define ROOM_FACES 6
// Which of the six faces a vertex came off, since each is coloured its own way
#define ROOM_SURF_WALL    0
#define ROOM_SURF_FLOOR   1
#define ROOM_SURF_CEILING 2
// Sixteen bit indices, so this is as many vertices as one room can hold
#define ROOM_MAX_VERTS 65535
// Floats a vertex in the baked model file: position, normal, texture coordinate
#define ROOM_MODEL_FLOATS 8
// Where the viewer stands in the model's own space. The geometry is built as
// (model - anchor) * scale, so the room arrives around the origin the way the
// generated one is built around it. Only x and z are fixed: the height of the
// anchor follows the scale, so the tier below stays underfoot.
#define ROOM_MODEL_ANCHOR_X 0.0f
#define ROOM_MODEL_ANCHOR_Z (-12.0f)
// The seating tier the viewer stands on, in the model's own space, and how far
// above it the eye sits whatever the room is scaled to
#define ROOM_MODEL_TIER_Y (-2.55f)
#define ROOM_EYE_HEIGHT_M 1.25f
// How large the baked room is drawn. Full size measured about a fifth too big
// in the headset, and the property below moves it between these two.
#define ROOM_PSX_SCALE 0.8f
#define ROOM_SCALE_MIN 0.25f
#define ROOM_SCALE_MAX 4.0f
// What the brightness property can ask for, either side of the atlas going on
// exactly as it was baked
#define ROOM_DIM_MIN 0.10f
#define ROOM_DIM_MAX 2.0f

// Pinned headers may predate the extension, values from the OpenXR registry
#ifndef XR_FB_composition_layer_settings
#define XR_FB_composition_layer_settings 1
#define XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME "XR_FB_composition_layer_settings"
#define XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB ((XrStructureType)1000204000)
typedef XrFlags64 XrCompositionLayerSettingsFlagsFB;
#define XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000004)
#define XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000008)
typedef struct XrCompositionLayerSettingsFB {
    XrStructureType type;
    const void* XR_MAY_ALIAS next;
    XrCompositionLayerSettingsFlagsFB layerFlags;
} XrCompositionLayerSettingsFB;
#endif

// Only the name, since this one is probed and reported rather than enabled.
// A runtime keyboard would be worth having on the headsets that offer it, and
// the log line is how we find out which those are.
#ifndef XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME
#define XR_META_VIRTUAL_KEYBOARD_EXTENSION_NAME "XR_META_virtual_keyboard"
#endif

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

#define PROP_OCCLUSION "debug.moonlight.occlusion"
#define PROP_SEPARATION "debug.moonlight.separation"
#define PROP_DISTANCE "debug.moonlight.distance"
#define PROP_SCREEN "debug.moonlight.screen"
#define PROP_CONVERGENCE "debug.moonlight.convergence"
#define PROP_DEPTH_GLOBAL "debug.moonlight.depthglobal"
#define PROP_DEPTH_LOCAL "debug.moonlight.depthlocal"
#define PROP_POINTER_CUTOFF "debug.moonlight.pointercutoff"
#define PROP_POINTER_BETA "debug.moonlight.pointerbeta"
#define PROP_AIM_CUTOFF "debug.moonlight.aimcutoff"
#define PROP_AIM_BETA "debug.moonlight.aimbeta"
#define PROP_BEAM_WIDTH "debug.moonlight.beamwidth"
#define PROP_POINTER_WAKE "debug.moonlight.pointerwake"
#define PROP_POINTER_SLEEP "debug.moonlight.pointersleep"
#define PROP_ENV_RADIUS "debug.moonlight.envradius"
#define PROP_SHARPEN "debug.moonlight.sharpen"
#define PROP_AMBILIGHT "debug.moonlight.ambilight"
#define PROP_AMBI_SMOOTH "debug.moonlight.ambismooth"
#define PROP_LETTERBOX "debug.moonlight.letterbox"
#define PROP_ROOM "debug.moonlight.room"
#define PROP_ROOM_SCALE "debug.moonlight.roomscale"
#define PROP_ROOM_DIM "debug.moonlight.roomdim"
#define PROP_TB_SWAP "debug.moonlight.tbswap"

// Radius of the low pass that splits the depth map into an overall shape and
// the local detail on top of it. About a tenth of the frame.
#define DEPTH_LOWPASS_RADIUS 11

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

    // 0 off, 1 normal, 2 quality, compositor sharpening on the screen layers
    int sharpenMode;

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

    // Ambilight. The frame is sampled down to a tiny colour texture, which the
    // glow quad behind the screen is then drawn from. Kept apart from the depth
    // passes: the glow works with the depth model off.
    GLuint ambiProgram;
    GLint ambiTexMatrixUniform;
    GLint ambiCropUniform;
    GLuint ambiTexture;
    GLuint ambiFbo;
    // Whether there is anything in that texture yet, since the first frame has
    // nothing to smooth against
    int ambiSeeded;
    float ambiSmooth;

    // Letterbox detection. The same program draws the frame uncropped and
    // unsmoothed into a second target that is read back on the CPU, so the
    // bars are counted from a current frame rather than from the smoothed one
    // the glow is looking at.
    GLuint ambiDetectTexture;
    GLuint ambiDetectFbo;
    // 1 detects and crops, 0 leaves the whole frame alone
    int ambiBarDetect;
    // Frames of the sample pass since the last readback
    int ambiBarCounter;
    // Per edge, in AMBI_EDGE_* order. The applied count is what the crop is
    // built from; the streaks are how long a larger or smaller count has held.
    int ambiBarApplied[AMBI_EDGES];
    int ambiBarGrowTicks[AMBI_EDGES];
    int ambiBarGrowMin[AMBI_EDGES];
    int ambiBarShrinkTicks[AMBI_EDGES];
    int ambiBarShrinkMax[AMBI_EDGES];
    // x0, y0, w, h in the plain quad's uv space, 0 0 1 1 for the whole frame
    float ambiCrop[4];
    GLuint glowProgram;
    GLint glowIntensityUniform;
    GLuint glowFbo;
    XrSwapchain glowSwapchain;
    uint32_t glowImageCount;
    XrSwapchainImageOpenGLESKHR* glowImages;
    int glowRendered;
    int ambilightOn;
    float ambiIntensity;
    // What the debug property asked for, or -1 while the panel still owns it
    int ambiOverride;

    // Which room the picker is on: 0 none, 1 the minimal room, 2 the cinema.
    // Same arrangement as the glow, with a debug property that can force it.
    int roomStyle;
    int roomOverride;
    // What the scale and brightness properties asked for, or -1 while the
    // params the style ships with own them
    float roomScaleOverride;
    float roomDimOverride;
    // Whether the picture washes its light over the room. Its own option, since
    // the wash runs whether the glow is on or not.
    int roomLightOn;
    // Everything the room is drawn with, built the first frame a style asks
    // for it rather than at startup, the way the background photo arrives.
    // One side by side image, a half of it per eye.
    XrSwapchain roomSwapchain;
    uint32_t roomImageCount;
    XrSwapchainImageOpenGLESKHR* roomImages;
    GLuint roomFbo;
    GLuint roomDepthBuffer;
    GLuint roomProgram;
    GLint roomViewProjUniform;
    GLint roomSpillGainUniform;
    GLint roomTexMixUniform;
    GLint roomDimUniform;
    GLuint roomVertexBuffer;
    GLuint roomIndexBuffer;
    int roomVertexCount;
    int roomIndexCount;
    // The baked model a textured style is built from, kept in its own copy so a
    // rebuild does not need the assets read again. Held exactly as the file has
    // it: the anchor and the scale go on as the geometry is built, so a scale
    // change is a rebuild rather than another read.
    float* roomModelVerts;
    unsigned short* roomModelIndices;
    int roomModelVertexCount;
    int roomModelIndexCount;
    int roomModelReady;
    // Its atlas, and a 1x1 white stand in so the sampler always has something
    // complete bound while the generated room is up
    GLuint roomTexture;
    int roomTextureReady;
    GLuint roomWhiteTexture;
    float roomTexMix;
    float roomDim;
    // Which style the buffers hold, so the picker moving between rooms
    // rebuilds them. 0 until the first build.
    int roomBuiltStyle;
    // What the picker last asked for, whether the assets had landed then, and
    // the scale it was asking at, so neither a style still waiting for them nor
    // a build that failed is retried every frame
    int roomWantedStyle;
    int roomAssetsSeen;
    float roomWantedScale;
    int roomEyeWidth;
    int roomEyeHeight;
    int roomReady;
    // One failed attempt is enough: nothing about it will be different next
    // frame, and retrying every frame would only fill the log
    int roomFailed;
    int roomRendered;
    float roomSpillGain;
    float roomClear[3];
    // Both eyes as the room was last drawn from them, which is what the
    // projection layer has to be submitted with. Nothing else in here locates
    // a view, since every other layer is placed by the compositor.
    XrView roomViews[ROOM_EYES];
    int roomViewsValid;
    // The room hangs the picture on its far wall, so where the user had it is
    // put aside for as long as one is on and handed back on the way out
    int roomHoldingScreen;
    XrPosef savedScreenPose;
    float savedScreenWidth;
    float savedScreenRadius;
    // What the runtime asks for per eye, and the most it will accept, read once
    // at startup. Only the room has any use for either.
    int recommendedEyeWidth;
    int recommendedEyeHeight;
    int maxEyeWidth;
    int maxEyeHeight;
    // Which Environment Res tier the room draws at, chosen on the Java side
    int envResTier;

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
    // Frames submitted while focused. Passthrough waits for the first one, see
    // the blend mode choice in nativeEndFrame.
    int focusedFrames;

    int cylinderSupported;
    int equirectSupported;
    int layerSettingsSupported;
    // Probed and logged only. Ours is drawn here, but knowing which runtimes
    // offer one of their own is worth a line.
    int virtualKeyboardSupported;
    // How many composition layers this runtime will take in one frame, asked
    // for rather than assumed, and whether a frame has already been caught
    // crowding it
    int maxLayerCount;
    int layerLimitWarned;
    // Whether a frame has already been caught outgrowing the layer array
    int layerDropWarned;

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
    // Which half of a top/bottom stereo photo goes to which eye, tradeable
    // over debug.moonlight.tbswap for a photo that packs them the other way
    int tbSwap;
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
    // A press the grid, the panel or the keyboard took for itself, held until
    // the trigger is released. The host's left button is level based, so
    // without this the press that picks something goes on to click whatever the
    // modal was covering as soon as it closes.
    int triggerSwallowed[SRC_COUNT];
    // Diagnostics for the click path, written but never acted on. The analog
    // value is kept as the action reported it, and the low water mark and the
    // dip count run for the length of one press, which is what says whether a
    // fast pair of taps merged into a single one at the hysteresis.
    float triggerValue[HAND_COUNT];
    float triggerHoldMin[HAND_COUNT];
    int triggerDipFrames[HAND_COUNT];
    // Dips that climbed back over the press threshold without ever reaching
    // the release one. Frames alone cannot tell those from the ramp of an
    // ordinary slow release, and these are the taps that went missing.
    int triggerRetaps[HAND_COUNT];
    int triggerDipping[HAND_COUNT];
    int trigLogDown[HAND_COUNT];
    int trigLogRaw;
    int trigLogLeft;
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

    // One euro filter state for the aim pose, per hand
    EuroState aimFilterPos[HAND_COUNT][3];
    EuroQuatState aimFilterRot[HAND_COUNT];
    float aimMinCutoff;
    float aimBeta;

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
    // The tilt and roll the screen was picked up with. A move holds these for
    // the whole drag and recomputes only the yaw, so the picture keeps the
    // attitude it had and just turns to stay square to the viewer.
    float grabPitch;
    float grabRoll;
    // Resize scales about the centre, which stays put. These are the corner
    // opposite the one being dragged, and their signs say which corner is held.
    float grabOppX, grabOppY;
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
    // The choice the last environment line was written for, so reapplying the
    // same one after a photo decode does not repeat it
    int loggedChoice;

    // One swapchain per sheet, all filled at startup, so changing tab is a
    // different handle in the layer rather than an upload
    XrSwapchain cogPanelSwapchains[COG_ART_COUNT];
    XrSwapchain cogButtonSwapchain;
    XrSwapchain cogThumbSwapchain;
    uint32_t cogPanelImageCounts[COG_ART_COUNT];
    uint32_t cogButtonImageCount;
    uint32_t cogThumbImageCount;
    XrSwapchainImageOpenGLESKHR* cogPanelImages[COG_ART_COUNT];
    XrSwapchainImageOpenGLESKHR* cogButtonImages;
    XrSwapchainImageOpenGLESKHR* cogThumbImages;
    int cogPanelReady[COG_ART_COUNT];
    int cogButtonReady;
    int cogThumbReady;
    int cogOpen;
    int cogTab;
    int cogButtonHot;
    // Which slider is being dragged and by which hand, -1 for none. The drag
    // keeps its hand, so the other one resting on the panel cannot steal it.
    int cogDragSlider;
    int cogDragHand;
    // The row under the ray, and on the display tab the cell within it
    int cogHoverSlider;
    int cogHoverCell;
    // Frozen when the panel opens rather than followed every frame. The
    // distance slider moves the screen, and a panel anchored to the screen
    // would drag the thumb out from under the ray mid drag.
    XrPosef cogPose;
    float cogW, cogH;
    // The keyboard. One swapchain per state, all three filled at startup, so
    // shift is a different handle in the layer rather than an upload.
    XrSwapchain kbPanelSwapchains[KB_STATE_COUNT];
    XrSwapchain kbButtonSwapchain;
    uint32_t kbPanelImageCounts[KB_STATE_COUNT];
    uint32_t kbButtonImageCount;
    XrSwapchainImageOpenGLESKHR* kbPanelImages[KB_STATE_COUNT];
    XrSwapchainImageOpenGLESKHR* kbButtonImages;
    int kbPanelReady[KB_STATE_COUNT];
    int kbButtonReady;
    int kbOpen;
    int kbButtonHot;
    int kbState;
    // The key under the ray, or -1, and whether it is being held down
    int kbHoverKey;
    int kbKeyDown;
    // The layout, as it arrived from Java. Four fractions of the panel per key,
    // left top right bottom, and a code per key per state.
    int kbKeyCount;
    float kbKeyRects[KB_MAX_KEYS * 4];
    int kbCodes[KB_STATE_COUNT][KB_MAX_KEYS];
    // Frozen when it opens, for the same reason the settings panel freezes
    // its own: the screen can be moved while it is up
    XrPosef kbPose;
    float kbW, kbH;

    // The exit button and its prompt. One sheet per lit button, all filled at
    // startup, so hovering one costs a handle rather than an upload.
    XrSwapchain exitButtonSwapchain;
    XrSwapchain exitPromptSwapchains[EXIT_ART_COUNT];
    uint32_t exitButtonImageCount;
    uint32_t exitPromptImageCounts[EXIT_ART_COUNT];
    XrSwapchainImageOpenGLESKHR* exitButtonImages;
    XrSwapchainImageOpenGLESKHR* exitPromptImages[EXIT_ART_COUNT];
    int exitButtonReady;
    int exitPromptReady[EXIT_ART_COUNT];
    int exitConfirmOpen;
    int exitButtonHot;
    // Which of the prompt's buttons the ray is on, which is also the sheet
    // to show
    int exitHoverZone;
    // Frozen when the prompt opens, like the settings panel: the screen stays
    // draggable behind it and the buttons must not move under the ray
    XrPosef exitPose;
    float exitW, exitH;

    // Curvature the panel asked for, or -1 while the preference still owns it,
    // alongside the preference itself so both are readable away from the JNI
    // entry points that carry it
    float panelCurve;
    float prefCurvature;
    // Separation the panel asked for, or -1 while the preference still owns it,
    // and whatever is actually in force this frame, which is what the 3D tab's
    // thumb reads back
    float panelSeparation;
    float separationCurrent;

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
    // Samples the plausibility filter refused, and the last raw value it saw,
    // so a starved window can say what the driver was returning
    long gpuDropped;
    GLuint64 gpuLastDroppedNs;

    // The room is a projection sized pass, and with it inside the window above
    // this driver wraps nearly every query it hands back. It gets a pair of its
    // own instead, opened and closed before the main one begins.
    GLuint roomTimerQueries[2];
    int roomTimerSlot;
    int roomTimerPending[2];
    int roomTimerPendingFrames[2];
    long roomGpuTotalNs;
    long roomGpuSamples;
    long roomGpuDropped;

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
typedef void (*PFNDELETEQUERIESEXT)(GLsizei, const GLuint*);

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif

// xr_session.c: instance, session, swapchain and lifecycle
int checkXr(XrResult res, const char* what);

// xr_gl.c: GL setup, the warp passes and the GPU timer
extern const float VERTEX_DATA[16];
extern PFNGENQUERIESEXT pfnGenQueries;
extern PFNBEGINQUERYEXT pfnBeginQuery;
extern PFNENDQUERYEXT pfnEndQuery;
extern PFNGETQUERYOBJECTUIVEXT pfnGetQueryObjectuiv;
extern PFNGETQUERYOBJECTUI64VEXT pfnGetQueryObjectui64v;
extern PFNDELETEQUERIESEXT pfnDeleteQueries;
GLuint compileShader(GLenum type, const char* src);
int linkProgram(GLuint* out, const char* fragmentSrc, const char* what);
int initGl(XrCtx* ctx);
void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation);

// xr_depth.c: the depth model staging and the depth thread's uploads
int initDepthModel(XrCtx* ctx);

// xr_ambilight.c: the frame colour sample, letterbox detection and the glow
int initAmbilight(XrCtx* ctx);
void ambiEffective(XrCtx* ctx, int* on, float* level);
void runAmbiBarDetect(XrCtx* ctx, const float* texMatrix);
void runFrameColorSample(XrCtx* ctx, const float* texMatrix);
void runGlowRender(XrCtx* ctx);

// xr_room.c: the 3d rooms
int roomEffective(XrCtx* ctx);
void applyRoomPlacement(XrCtx* ctx, int style, float aspect, int reseeded);
void prepareRoom(XrCtx* ctx);
void renderRoom(XrCtx* ctx);

// xr_input.c: actions, hands, the ray and the per frame input pass
int initXrInput(XrCtx* ctx);
void destroyXrInput(XrCtx* ctx);
void refreshInputSource(XrCtx* ctx);
int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature);

// xr_ui.c: where the furniture and the panels sit, and what the ray is over
int hoverTest(float u, float v, float width, float height, int cornersLive, int* corner);
float effectiveCurvature(XrCtx* ctx);
int cogScreenLocked(XrCtx* ctx);
float screenPitch(XrCtx* ctx);
XrQuaternionf screenOrient(float yaw, float pitch, float roll);
float screenRoll(XrCtx* ctx);
XrPosef pickerPose(XrCtx* ctx, float* outWidth, float* outHeight);
void envButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide);
int envButtonHit(XrCtx* ctx, float u, float v, float height);
void cogButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide);
int cogButtonHit(XrCtx* ctx, float u, float v, float height);
XrPosef cogPanelPose(XrCtx* ctx, float* outWidth, float* outHeight);
void kbButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide);
int kbButtonHit(XrCtx* ctx, float u, float v, float height);
XrPosef kbPanelPose(XrCtx* ctx, float* outWidth, float* outHeight);
int kbKeyAt(XrCtx* ctx, float u, float v);
void exitButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide);
int exitButtonHit(XrCtx* ctx, float u, float v, float height);
XrPosef exitPromptPose(XrCtx* ctx, float* outWidth, float* outHeight);
int exitPromptZone(float u, float v);
int cogTabRowCount(int tab);
float cogSliderValue(XrCtx* ctx, int tab, int slider);
void cogApplySlider(XrCtx* ctx, int tab, int slider, float pu);
int cogOptionCells(int option);
int cogOptionValue(XrCtx* ctx, int option, int headLocked);
int cogApplyOption(XrCtx* ctx, int option, int cell);
void cogDragEnded(XrCtx* ctx, float* out);
int cogCellAt(float pu, int cells);
void lockButtonPlacement(XrCtx* ctx, Vec3* outLocal, float* outSide);
int lockButtonHit(XrCtx* ctx, float u, float v, float height);

// xr_assets.c: the swapchains the art goes into and the uploads that fill them
int createArtSwapchain(XrCtx* ctx, int width, int height, const char* what,
                       XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images,
                       uint32_t* count);
void destroyArtSwapchain(XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images);
int createPointerSwapchain(XrCtx* ctx);
int uploadPointerArt(XrCtx* ctx);

// xr_debug.c: setprop knobs and frame capture
void propFlag(const char* name, int* target);
void pollCaptureRequest(XrCtx* ctx);
void writeCapture(XrCtx* ctx, const char* what, const void* data, size_t bytes);
void writeCaptureDepthTexture(XrCtx* ctx);

#endif
