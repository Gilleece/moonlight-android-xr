
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
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

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
#include "xr_depthmap.h"
#include "xr_roommesh.h"
#include "xr_layout.h"
#include "xr_rate.h"
#include "xr_gate.h"
#include "xr_pinch.h"
#include "xr_notice.h"
#include "xr_glow.h"
#include "xr_grade.h"
#include "xr_keys.h"
#include "xr_controller.h"
#include "xr_headaim.h"
#include "xr_gamepad.h"

#define TAG "moonlight-xr"

// Not an Android priority, only ever seen inside xrLog: a key event, which
// logcat gets as info while the file keeps it at the quiet level
#define PRIO_EVENT 90

void xrLog(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGI(...) xrLog(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) xrLog(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) xrLog(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGEV(...) xrLog(PRIO_EVENT, __VA_ARGS__)

// 64 bit on every ABI: a long is 32 on armeabi-v7a and would wrap in seconds
static inline int64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
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

// What the ray is over, and the handles' sizes and hover zones, are in
// xr_layout.h with the hover test that reads them

#define GRAB_NONE   0
#define GRAB_MOVE   1
#define GRAB_RESIZE 2

// Widened along with the grid so a cell stays about the size it was at three
// columns: five of them now
#define PICKER_WIDTH_FRAC 0.91f
#define OUTLINE_TEX 128
// The buttons along the bar, the picker's to the left of the move bar and the
// cog's to the right, are sized in xr_layout.h with their placement

#define COG_WIDTH_FRAC 0.36f
#define COG_THUMB_TEX 64
// Which rows the panel is showing: one of the tabs, or the Room tab, which is
// what the first tab is while a room is up. Numbered past the tabs, since it
// is not a tab of its own and nothing else can reach it.
#define COG_FACE_ROOM COG_TAB_COUNT

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

// The report sheet, centred over the bottom of the picture with the keyboard
// under it
#define REPORT_WIDTH_FRAC 0.40f

// The hand lock hint, over the middle of the picture, as wide as the report
// sheet. It waits for a hand to have pointed this long, so a hand that flickers
// in as a controller is put down does not bring it up.
#define HINT_WIDTH_FRAC 0.40f
#define HINT_POINTING_NS 500000000L

// The Ko-fi sheet, over the middle of the picture like the hint and as wide
#define KOFI_WIDTH_FRAC 0.40f

// The panels that fade in and out, each with a fade of its own
#define FADE_COG 0
#define FADE_PICKER 1
#define FADE_KB 2
#define FADE_EXIT 3
#define FADE_REPORT 4
#define FADE_HINT 5
#define FADE_KOFI 6
#define FADE_PANELS 7
// And the layers a colour scale is chained onto: those seven, then the splash
// and the toast
#define FADE_SLOT_SPLASH FADE_PANELS
#define FADE_SLOT_TOAST (FADE_PANELS + 1)
#define FADE_SLOTS (FADE_PANELS + 2)

// The toast hangs off the eyes, ahead and a little below where they look,
// in metres
#define TOAST_W_M 0.56f
#define TOAST_DISTANCE_M 0.80f
#define TOAST_DROP_M 0.14f

// The launch splash, locked to the head: a black quad wider than any view
// with the sheet a little in front of it, in metres
#define SPLASH_BLACK_M 6.0f
#define SPLASH_BLACK_DISTANCE_M 1.5f
#define SPLASH_SHEET_W_M 0.9f
#define SPLASH_SHEET_DISTANCE_M 1.45f

// How far the ray runs when it is aimed at nothing at all, in metres
#define FREE_BEAM_M 4.0f

// How long head aim drops frames after the local space is recentred, past the
// last frame, however soon the runtime says the change lands
#define HEAD_AIM_RECENTRE_HOLD_NS 100000000LL
// How often the log says how far head aim has moved the mouse
#define HEAD_AIM_COUNT_NS 10000000000L

// Ambilight. The frame is boiled down to a tiny colour texture once a frame,
// and a soft quad behind the screen is filled from it, so whatever the picture
// is sitting in front of picks up the colours on it. The sizes, the glow's
// shape and its colour arithmetic are in xr_glow.h.

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

// The 3d room. A baked model that ships in the assets, drawn per eye into the
// one projection layer this renderer has. Which room, 0 for none, each with a
// row of its own in xr_room.c.
#define ROOM_STYLE_THEATER 1
#define ROOM_STYLE_GRAND_CINEMA 2
#define ROOM_STYLE_SYNTHWAVE 3
#define ROOM_STYLE_FIRST ROOM_STYLE_THEATER
#define ROOM_STYLE_LAST ROOM_STYLE_SYNTHWAVE
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
// Floats a vertex in the baked model file: position, normal, texture
// coordinate and colour
#define ROOM_MODEL_FLOATS ROOM_MESH_VERTEX_FLOATS
// The room's clip planes: near enough to walk into a wall, and a far plane
// from the room's own reach with this much to spare, never under the floor
// every room was once drawn with
#define ROOM_NEAR_M 0.05f
#define ROOM_FAR_MIN_M 60.0f
#define ROOM_FAR_MARGIN 1.5f
// What the scale property can ask for. Every room is drawn at the size it was
// built unless it does.
#define ROOM_SCALE_MIN 0.25f
#define ROOM_SCALE_MAX 4.0f
// What the brightness property can ask for, either side of the room going on
// exactly as it was baked
#define ROOM_DIM_MIN 0.10f
#define ROOM_DIM_MAX 2.0f
// A floor or a wall seen at a glancing angle keeps its detail with this much
// anisotropic filtering on a compressed atlas, where the driver offers it
#define ROOM_ANISOTROPY_MAX 4.0f

// The longest the warp waits flat for a fresh map once the 3D is switched back
// on. Java captures the frame in hand at the switch, so a map is one inference
// away, about 55 ms on the slowest route; this only matters if none comes.
#define STEREO_WAIT_NS 400000000L

// Three depth textures rather than two: the stage thread advances through
// them in a fixed rotation, so a slot it is about to overwrite was last handed
// to the frame loop a full rotation ago rather than one. One inference dwarfs
// anything the frame loop could still have in flight from that slot by then.
#define DEPTH_TEX_COUNT 3

// The least time between two scene cut lines in the log, so a stream that
// cuts on every beat cannot flood it. The next line says how many went unsaid.
#define DEPTH_CUT_LOG_NS 2000000000L

// Generous on purpose: the stage thread has no per frame deadline, and a
// short timeout here would only trade a hung capture for a torn one
#define CAPTURE_FENCE_TIMEOUT_NS 500000000ull

// Pinned headers may predate the extension, values from the OpenXR registry
#ifndef XR_FB_composition_layer_settings
#define XR_FB_composition_layer_settings 1
#define XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME "XR_FB_composition_layer_settings"
#define XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB ((XrStructureType)1000204000)
typedef XrFlags64 XrCompositionLayerSettingsFlagsFB;
#define XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000001)
#define XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB ((XrCompositionLayerSettingsFlagsFB)0x00000002)
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
// without a rebuild. Each is an integer percent of the real value. Read by
// debug builds only: a release build never polls the property store, so a
// knob left set from a test session cannot override the panel in one.
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
#define PROP_SHARPEN "debug.moonlight.sharpen"
#define PROP_SUPERSAMPLE "debug.moonlight.supersample"
#define PROP_AMBILIGHT "debug.moonlight.ambilight"
#define PROP_AMBI_SMOOTH "debug.moonlight.ambismooth"
#define PROP_LETTERBOX "debug.moonlight.letterbox"
// 1 lifts the glow's colours to a steady luma and rolls a dark edge off, as
// shipped, and 0 draws the glow straight from the sample the way it was before
#define PROP_GLOW_NORM "debug.moonlight.glownorm"
#define PROP_ROOM "debug.moonlight.room"
#define PROP_ROOM_SCALE "debug.moonlight.roomscale"
#define PROP_ROOM_DIM "debug.moonlight.roomdim"
// Milliseconds, the time constants of the per texel depth average and of the
// range the map is normalised against. 0 turns either off, so each map
// replaces the last or is normalised against its own range.
#define PROP_DEPTH_TAU "debug.moonlight.depth_tau"
#define PROP_RANGE_TAU "debug.moonlight.range_tau"
// The scene cut detector: 0 off, 1 on as every build ships, 2 on with a line
// in the log for every capture
#define PROP_DEPTH_CUT "debug.moonlight.depth_cut"
// The warp's four edge fixes: the shifted sample held inside the frame (0/1),
// the band the shift fades out over at the left and right edges (pixels, 0
// off), the cubic read of the depth map (0/1), and the texel each eye's
// rectangle keeps clear of the seam with the other (0/1). All on as shipped
// but the cubic read, which is off for its cost.
#define PROP_SRC_INSET "debug.moonlight.srcinset"
#define PROP_EDGE_FADE "debug.moonlight.edgefade"
#define PROP_DEPTH_CUBIC "debug.moonlight.depthcubic"
#define PROP_SEAM_INSET "debug.moonlight.seaminset"
// Hz to force the display to, 0 to hand it back to the stream's rate and the
// frame budget. Read at session start and live.
#define PROP_REFRESH "debug.moonlight.refresh"
// Depth maps a second, standing in for a change of the setting mid session,
// which forgets what the governor learnt; 0 goes back to the setting. Live.
#define PROP_DEPTH_RATE "debug.moonlight.depthrate"
// The CPU and GPU levels asked of the runtime: 0 none, 1 sustained high as
// shipped, 2 boost. Read at session start and live, though a level once asked
// for cannot be taken back, so 0 only means nothing at the next session.
#define PROP_PERF_LEVEL "debug.moonlight.perflevel"
// Milliseconds a panel takes to fade, 0 for the shipped 150, and the splash
// twice that. Long ones make a fade something a screenshot can catch.
#define PROP_FADE_MS "debug.moonlight.fadems"
#define FADE_KNOB_MAX_MS 10000
// A start step to make fail for real, so the failure report can be seen on a
// headset whose runtime works: instance, system or session. Read at start.
// exit lets the start through and asks the runtime to end the session
// EXIT_KNOB_DELAY_NS after it is first focused, as the system menu's quit does.
#define PROP_START_FAIL "debug.moonlight.xrfail"
#define EXIT_KNOB_DELAY_NS 5000000000LL

// What the session asks the runtime's performance levels for
#define PERF_LEVEL_NONE 0
#define PERF_LEVEL_SUSTAINED_HIGH 1
#define PERF_LEVEL_BOOST 2

// How long after the runtime moves the display off the rate asked for before
// it is asked once more, how many times a session does that, and how long a
// request may take to land before the display is checked
#define RATE_REASK_MS 10000
#define RATE_REASK_MAX 3
#define RATE_CONFIRM_MS 5000
// Windows of the frame budget between two summary lines in the log
#define RATE_LOG_WINDOWS 5

// Pixels of the frame the shift fades to nothing over at each side edge, and
// the most the property can ask for
#define EDGE_FADE_PX 8
#define EDGE_FADE_MAX_PX 64
// Texels each eye's rectangle comes in by on the side by side axis
#define SEAM_INSET_TEXELS 1

// Radius of the low pass that splits the depth map into an overall shape and
// the local detail on top of it. About a tenth of the frame, in texels of a
// 256 square map, and scaled with the map on each axis.
#define DEPTH_LOWPASS_RADIUS 11

typedef struct {
    JavaVM* vm;
    jobject activity;

    EGLDisplay eglDisplay;
    EGLConfig eglConfig;
    EGLContext eglContext;
    EGLSurface eglPbuffer;

    XrInstance instance;
    // The runtime's name and version as the log line gives them, empty until
    // the instance answers
    char runtimeLabel[XR_MAX_RUNTIME_NAME_SIZE + 32];
    // What the start has got through so far, kept until init is over in case
    // it has to say where it stopped
    struct StartReport* start;
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
    // The same values for the compositor's supersampling on the same layers
    int supersampleMode;
    // The pair last logged, plus one so zero means nothing logged yet
    int layerFlagsLogged;

    int videoWidth;
    int videoHeight;

    // Stereo test path. When stereoMode is not OFF the swapchain is double
    // wide and each eye gets its own warped copy of the frame
    int stereoMode;
    int depthDebug;
    // The 3D switch on the bar and the 3D tab, for this session only. Off, the
    // frame is drawn once and flat into the left half, both eyes are shown
    // that, and Java stops feeding the model. Starts on in a session with
    // stereo and means nothing in one without.
    int stereoLive;
    // Just switched back on with a model running: still drawn flat until a
    // map made since then lands, or STEREO_WAIT_NS has gone by, so the warp
    // does not pick up with the depth of whatever was showing when it went
    // off. The map that was live at the switch, and when it was pressed.
    int stereoWaiting;
    int stereoWaitIndex;
    int64_t stereoWaitNs;
    // Draws the frame again with nothing new from the decoder, so the switch
    // shows at once on a picture that is standing still
    int warpRedraw;
    // How many eyes the last draw left in the swapchain, which the video
    // layers follow: both halves, or the left one to both eyes
    int drawnEyes;
    // The depth map's size for this session, which is the model input's.
    // Set by nativeInit before any GL init and fixed from then on.
    int depthTexW;
    int depthTexH;
    // Triple buffered: the frame loop samples one slot while the stage thread
    // rotates through the rest, so neither ever blocks on the other. Which
    // slot the frame loop reads is its own to write.
    GLuint depthTextures[DEPTH_TEX_COUNT];
    int depthReadIndex;
    // The slot the stage thread's next upload lands in. Its own to read and
    // write, so nothing guards it.
    int depthWriteIndex;
    // Published by the stage thread together with the fence for that slot,
    // and adopted by the frame loop once it notices a new value. The release
    // and acquire on this one word are what keep the frame loop from seeing
    // the index before the fence that has to come with it.
    atomic_int depthStagedIndex;
    // One fence per slot, set right after the upload it guards and cleared by
    // whichever frame loop site first samples that slot
    GLsync depthFences[DEPTH_TEX_COUNT];

    // Second context in the same share group for the depth thread. Inference
    // takes longer than a display frame, so it cannot run on the frame loop.
    // Only the model runs on it.
    EGLContext depthContext;
    EGLSurface depthPbuffer;
    // Third, for the stage thread, which turns each readback into a model
    // input and uploads each map while the model runs on the other pair
    EGLContext depthStageContext;
    EGLSurface depthStagePbuffer;

    // Depth model staging. The frame is downscaled to depthTexW by depthTexH
    // on the GPU, read back, run through the model in Java, and the result
    // goes back up into the depth texture
    GLuint downscaleProgram;
    GLint downscaleTexMatrixUniform;
    GLuint downscaleTexture;
    GLuint downscaleFbo;
    // One set per pair. The frame loop queues a readback into a pair's pixel
    // buffer and leaves a fence, the stage thread waits on it and maps the
    // buffer into the pair's model input, and the model writes the pair's
    // output. Java hands the pairs out, so each has one owner at a time.
    GLuint depthPbos[DEPTH_PAIRS];
    GLsync captureFences[DEPTH_PAIRS];
    float* modelInput[DEPTH_PAIRS];
    float* modelOutput[DEPTH_PAIRS];
    // The pair the live map was made from, for the frame capture only
    atomic_int depthLastPair;
    unsigned char* depthUploadBuf;

    // The map normalised to 0..1, and the range it was normalised against,
    // smoothed on its own over real time: a single outlier pixel moving the
    // min or max used to shift the whole mapping, which pumps the entire
    // image. The range and the time of the map it last took belong to the
    // stage thread.
    float* depthNorm;
    float* depthLow;
    float* depthScratch;
    float* depthColSums;
    float depthGlobal;
    float depthLocal;
    DepthRange depthRange;
    int64_t rangeNs;
    // The model output averaged per texel over real time, ahead of the range
    // and the normalisation, so raw model flicker does not reach the eyes.
    // The stage thread's too.
    float* depthTau;
    int depthTauValid;
    int64_t depthTauNs;
    // Time constants in milliseconds, 0 for none. Set at init and only moved
    // by the debug knobs.
    int depthTauMs;
    int rangeTauMs;
    // Scene cuts, found on the model input before the model runs. A jump
    // starts the per texel average again and a confirmed cut the range, at
    // the map made from the capture it was found on (DepthResets). All but
    // the level belong to the stage thread. The level is 0 off, 1 on, and 2
    // on with a line per capture, which only a debug build can ask for.
    int depthCutLevel;
    DepthCut depthCut;
    DepthResets depthResets;
    long depthCutChecks;
    int64_t depthCutLogNs;
    int depthCutUnlogged;

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
    // The edge fixes, on apart from the cubic read unless a debug property
    // says otherwise. Pixels for the fade, 0 or 1 for the rest.
    int srcInsetOn;
    int edgeFadePx;
    int depthCubic;
    int seamInset;
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
    GLint ambiGradeOnUniform;
    GLint ambiGradeUniform;
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
    // Its readback, also through a pixel buffer collected a frame later
    GLuint ambiDetectPbo;
    int ambiDetectPending;
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
    GLint glowBlurUniform;
    GLuint glowFbo;
    // The glow's own copy of the sample, lifted to a steady luma with a dark
    // edge rolled off, which the glow is drawn from rather than the sample
    // itself. The room's light keeps reading the sample as it is.
    GLuint glowEdgeProgram;
    GLuint glowEdgeTexture;
    GLuint glowEdgeFbo;
    // 1 draws the glow from that copy, 0 from the sample, the way it was
    int glowNorm;
    XrSwapchain glowSwapchain;
    uint32_t glowImageCount;
    XrSwapchainImageOpenGLESKHR* glowImages;
    int glowRendered;
    // The switch and the level the glow image was last drawn at, so moving
    // either redraws it on a picture standing still
    int glowDrawnOn;
    float glowDrawnLevel;
    int ambilightOn;
    float ambiIntensity;
    // What the debug property asked for, or -1 while the panel still owns it
    int ambiOverride;

    // The picture grade, in the preferences' whole units in PICTURE_ order,
    // and the sums they come to. Off while all four are at their defaults,
    // which the shaders branch on, so the picture as streamed costs nothing.
    int pictureUnits[PICTURE_VALUES];
    int gradeOn;
    PictureGrade grade;

    // Which room the picker is on: 0 none, else one of the ROOM_STYLE_ values
    // Same arrangement as the glow, with a debug property that can force it.
    int roomStyle;
    int roomOverride;
    // What the scale and brightness properties asked for, or -1 while the
    // params the style ships with own them
    float roomScaleOverride;
    float roomDimOverride;
    // Whether the picture washes its light over the room. Its own option, since
    // the wash runs whether the glow is on or not. One value for every room.
    int roomLightOn;
    // Each baked room's own values for the Room tab's rows, by style, in the
    // units the preferences hold: brightness and light level in hundredths,
    // the glow 1 or 0, the size in whole percent of the room's screen. Seeded
    // from the room table, then handed down from the preferences.
    int roomBrightness[ROOM_STYLE_LAST + 1];
    int roomGlow[ROOM_STYLE_LAST + 1];
    int roomLightLevel[ROOM_STYLE_LAST + 1];
    int roomScreen[ROOM_STYLE_LAST + 1];
    // Everything the room is drawn with, built the first frame a style asks
    // for it rather than at startup. One side by side image, a half of it per
    // eye.
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
    // The parts the buffers were built with, each drawn from its own atlas or
    // its vertex colours. Copied at the build, so a model arriving for the next
    // room cannot change what the buffers in use are drawn as.
    RoomMeshPart roomParts[ROOM_MESH_PARTS_MAX];
    int roomPartCount;
    // The baked model a style is built from, kept in its own copy so a rebuild
    // does not need the assets read again. Held exactly as the file has it:
    // the anchor and the scale go on as the geometry is built, so a scale
    // change is a rebuild rather than another read.
    float* roomModelVerts;
    uint32_t* roomModelIndices;
    int roomModelVertexCount;
    int roomModelIndexCount;
    RoomMeshPart roomModelParts[ROOM_MESH_PARTS_MAX];
    int roomModelPartCount;
    // How many atlases its parts are painted from, one slot each
    int roomAtlasCount;
    int roomModelReady;
    // The atlases, a slot each, and a 1x1 white stand in so the sampler always
    // has something complete bound whatever a part is painted from
    GLuint roomTextures[ROOM_MESH_ATLASES_MAX];
    int roomTextureReady[ROOM_MESH_ATLASES_MAX];
    // Which baked room the model and each atlas belong to. Only one room is
    // resident at a time, so a style is only built once all of them are its
    // own, and a model arriving for another room drops the atlases it replaces.
    int roomModelStyle;
    int roomTextureStyle[ROOM_MESH_ATLASES_MAX];
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
    // How far the room in the buffers reaches, as the far plane it is drawn
    // with
    float roomFarZ;
    float roomClear[3];
    // Both eyes as the room was last drawn from them, which is what the
    // projection layer has to be submitted with. Nothing else in here locates
    // a view, since every other layer is placed by the compositor.
    XrView roomViews[ROOM_EYES];
    int roomViewsValid;
    // The room hangs the picture on its far wall, so where the user had it is
    // put aside for as long as one is on and handed back on the way out
    int roomHoldingScreen;
    // Where the room last hung it, for the log line that says so
    int roomPlacedStyle;
    float roomPlacedWidth;
    XrPosef savedScreenPose;
    float savedScreenWidth;
    float savedScreenRadius;
    // A recentre turned the placement put aside, which is saved once the room
    // hands it back
    int recentredInRoom;
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
    GLint srcInsetUniform;
    GLint edgeFadeUniform;
    GLint depthCubicUniform;
    GLint gradeOnUniform;
    GLint gradeUniform;
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
    // Why the runtime ended the frame loop, which Java hands to the activity,
    // empty until it has
    char exitReason[96];
    // The fail knob's exit: the session is asked to end once, this long after
    // it was first focused
    int exitKnob;
    int64_t exitKnobAtNs;
    // Whether the session has ever been focused, which Java reads to know the
    // launch is through, and since when the runtime has kept it from running,
    // for the line that says it is still waiting
    int everFocused;
    int64_t waitingSinceNs;
    int64_t waitingLoggedNs;
    XrTime predictedDisplayTime;
    int shouldRender;
    int everRendered;
    // Frames submitted while focused. Passthrough waits for the first one, see
    // the blend mode choice in nativeEndFrame.
    int focusedFrames;
    // The blend mode has gone over to passthrough once, which is said once
    int passthroughBlendSaid;

    int cylinderSupported;
    // The cursor dot's width as last said in the log
    float dotSizeSaid;
    // The screen's cylinder is held under a full turn, as last said in the log
    int cylinderClampSaid;
    // Where the bar's row hung as last said in the log: its frame's width and
    // the picture's bottom edge it hangs from
    float barSaidWidth;
    Vec3 barSaidEdge;
    int layerSettingsSupported;
    // Layer colour scale (XR_KHR_composition_layer_color_scale_bias), which is
    // what fades a layer without drawing anything. Without it the panels and
    // the splash come and go at once, as they always did.
    int colorScaleSupported;
    // How long a panel's fade takes, which the splash's is twice
    int64_t fadeNs;
    int fadeKnobMs;
    // The settings panel, the picker, the keyboard and the exit prompt, each
    // fading on its own, and whether one is still on its way out, which keeps
    // the bar furniture down until it has gone
    Fade panelFades[FADE_PANELS];
    int panelFadingOut;

    // The launch splash. Up from the session's first frame until the panels,
    // the room and the depth model are ready, so their loading is not a black
    // stall with nothing on it. When each was ready, in ms from the first
    // frame, -1 while not yet, for the line that says why it went.
    Splash splash;
    int splashReadyMs[3];
    XrSwapchain splashSwapchain;
    uint32_t splashImageCount;
    XrSwapchainImageOpenGLESKHR* splashImages;
    int splashArtReady;
    // The last of the panels' art reaching the frame loop, whether or not it
    // uploaded, which is when the splash stops waiting on the panels
    int panelArtArrived;
    // Maps the stage thread has published, and the model giving up before it
    // made one, which are the two ways the splash stops waiting on the depth
    atomic_int depthMapsStaged;
    atomic_int depthGaveUp;

    // The toast: what is waiting to be said and what is up, raised here or
    // handed down by Java, held back while the splash is up. Java draws the
    // words into the sheet, which shows only while it says what is up now.
    NoticeBoard notices;
    XrSwapchain toastSwapchain;
    uint32_t toastImageCount;
    XrSwapchainImageOpenGLESKHR* toastImages;
    int toastArtReady;
    int toastDrawnKind;
    int toastDrawnArg;
    Fade toastFade;

    // The display refresh rate. The runtime keeps whatever rate it starts on
    // unless asked, so a stream faster than that loses frames before they are
    // ever shown. Asked for to match the stream, and stepped down while the
    // 3D warp runs if the frame loop cannot hold it. All of it the frame
    // loop's own, bar the two rates the stats read.
    int refreshRateSupported;
    PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateDisplayRefreshRates;
    PFN_xrGetDisplayRefreshRateFB pfnGetDisplayRefreshRate;
    PFN_xrRequestDisplayRefreshRateFB pfnRequestDisplayRefreshRate;
    float displayRates[RATE_MAX];
    int displayRateCount;
    // The stream's frame rate as the preferences asked for it
    int streamFps;
    // What was last asked for and what the display is on, 0 for none yet
    float rateAsked;
    float displayRate;
    int64_t rateAskedNs;
    int rateConfirmed;
    // The session's first rate has landed with focus, so a change from here
    // on goes on the toast; the one it starts on never does
    int rateSettled;
    // The rate the warp was stepped down to, 0 while it has not been, kept
    // for the session so switching the 3D off and on does not try again
    float warpRateHeld;
    int rateWarpOn;
    // The rate the budget last said it could go no lower from
    float rateFloorSaid;
    // The runtime moved the display off the rate asked for at this time, 0
    // for not, and how many times this session the rate was asked for again
    int64_t rateMovedNs;
    int rateReasks;
    // debug.moonlight.refresh, 0 for automatic
    int refreshKnob;
    RateBudget rateBudget;
    int rateLogWindows;
    float rateLogFrameMs;
    float rateLogWorstMs;
    long rateLogMissed;
    // The frame loop's own clock for the budget: when this frame began, the
    // display time the last one was predicted for, and the period between
    // refreshes, which is also the rate on a runtime without the extension
    int64_t frameBeganNs;
    XrTime lastDisplayTime;
    int64_t displayPeriodNs;

    // The depth model's rate in maps a second: the governor that spends it
    // before the display rate when the budget is missed or the runtime
    // throttles, and the gate that times the captures from it. Frame loop
    // only, bar the target the stats read.
    DepthGovernor depthGov;
    DepthGate depthGate;
    // The preference as Java handed it over, and debug.moonlight.depthrate,
    // 0 for the preference
    int depthRateSetting;
    int depthRateKnob;
    // The runtime's last notice level for each CPU and GPU sub domain
    int perfNoticeLevels[2][3];

    // The CPU and GPU levels (XR_EXT_performance_settings). Decoding, the
    // warp and the depth model all want the clocks to stay put rather than be
    // renegotiated around every scene, so a sustained level is asked for once
    // the session exists. A runtime without it carries on as it would have.
    int perfSettingsSupported;
    PFN_xrPerfSettingsSetPerformanceLevelEXT pfnPerfSettingsSetPerformanceLevel;
    int perfLevel;

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

    int srgbWriteControl;
    // Whether a room's atlas can go up as it ships, ASTC compressed, and how
    // much anisotropic filtering it gets: 1 without the extension, else the
    // driver's most up to ROOM_ANISOTROPY_MAX
    int astcSupported;
    float roomAnisotropy;
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
    // The EXT profile bound its pinch value, which is the whole of a hand's
    // press wherever that profile is the one on the hand, and which hands it
    // is on. The runtime has decided by then, so its value is not held back.
    int extHandClick;
    int onExtHands[HAND_COUNT];
    // Looking at something instead of pointing at it. While the eyes point,
    // tracked hands only pinch, and a controller points once it is in use.
    int eyeGaze;
    int gazeEnabled;
    XrAction gazeAction;
    int lastSnapshot;
    // Reading the joints directly, because a pinch is not always offered as an
    // input. Thumb to fingertip is the whole of it.
    int jointTracking;
    XrHandTrackerEXT handTrackers[HAND_COUNT];
    // The pinch the joints read, close and closing fast, and since when a
    // pinch has been wanted on each hand, which it has to be for
    // PINCH_HOLD_NS before it is a press
    PinchGate pinchGate[HAND_COUNT];
    int64_t pinchWantNs[HAND_COUNT];
    // Where the pinch is, which is what a drag the eyes started follows
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
    // What the runtime has on each hand's path, a PROFILE_ value. Only a
    // controller may take the pointing off the eyes: a hand whose tracking
    // has dropped reports no profile at all, and must not read as one.
    int profileKind[HAND_COUNT];
    // A controller's aim tracked in position and orientation with its action
    // live, read once a frame
    int aimTracked[HAND_COUNT];
    // Each controller's own rest clock, on the pointer's two times. The shared
    // clock below is held on by the other hand and by the eyes.
    ControllerClock aimClock[HAND_COUNT];
    // A controller in use, settled at the top of the frame so every reader in
    // it gets the same answer about who points
    int controllerAwake;
    // The hands pointing for eyes that have gone missing
    GazeBridge gazeBridge;
    // A pinch that woke the pointer is not also a click, so it is swallowed
    // until the hand opens again
    int pinchSwallowed[SRC_COUNT];
    // Hands locked out for the session, so a gamepad can be used without a
    // stray pinch clicking the desktop or dragging the screen around. The
    // triple pinch is the way back. Controllers are never affected, and it
    // starts off every session.
    int handsLocked;
    // The triple pinch that turns the lock, per hand, read off the press
    // whether the hands are locked or not, since it is the way back
    TriplePinch triplePinch[HAND_COUNT];
    // The sheet that says how the gesture works, once a session the first
    // time a hand points. Wanted unless it was put away for good or the hands
    // are off, as Java says at the start; shown once a session at most. Its
    // art, since when a hand has been pointing, the button under the ray and
    // the pose frozen when it opened.
    int hintWanted;
    int hintShown;
    int hintOpen;
    int64_t hintPointingNs;
    XrSwapchain hintSwapchain;
    uint32_t hintImageCount;
    XrSwapchainImageOpenGLESKHR* hintImages;
    int hintReady;
    int hintHoverZone;
    XrPosef hintPose;
    float hintW, hintH;

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
    // The left menu button, only for gamepad mode: Start, and with the left
    // grip the switch between the pad and the pointer. Whether a profile took
    // it, since one that will not loses only this.
    XrAction menuAction;
    int menuBound;
    // Each controller's vibration, only for the host's rumble in gamepad mode,
    // and whether any profile took it
    XrAction hapticAction;
    int hapticBound;
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
    int64_t lastInputNs;

    // One euro filter state for the hit point, per axis
    EuroState filterU;
    EuroState filterV;
    float pointerMinCutoff;
    float pointerBeta;
    int64_t lastHitNs;
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
    // Whether a still controller's pointer pauses at all, the setting the
    // Display tab's row also writes. Arrives with every frame.
    int pointerSleepOn;

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
    // The ray's switch: the setting a session starts from, and whether the
    // bar button or the Display tab's row has turned it over since. Off, the
    // beam is not drawn unless a panel is up; the dot and the hit test stay.
    // The hands' rays follow it too.
    int raySetting;
    int rayFlipped;
    // Whether the beam went up last frame, -1 before the first, for the line
    // that says when that changes
    int rayDrawnSaid;
    // Head aim: the switch, the setting a session starts from and whether the
    // bar button or the Display tab's row has turned it over since; pixels a
    // degree and the dead zone in degrees a second, in the preferences' whole
    // units; the head as last measured; and the controller's pointer as last
    // fed while head aim is on. Only acts with the screen locked to the head.
    int headAimSetting;
    int headAimFlipped;
    int headAimSensitivity;
    int headAimDeadZone;
    HeadAim headAim;
    PointerNudge headAimNudge;
    // On and able to act this frame, which makes the pointer relative
    int headAimActive;
    // A recentre still landing: frames are dropped until one is displayed at
    // or after this time
    int headAimRecentring;
    XrTime headAimRecentreAt;
    // What the log last said it was doing, -1 before the first frame, and the
    // pixels sent since the last line that counted them
    int headAimSaid;
    long headAimSentX;
    long headAimSentY;
    XrTime headAimCountNs;
    // Counts the input passes, so the pointer can tell when it missed one
    long inputFrames;
    // Gamepad mode: whether the controllers are one pad on the host rather
    // than the pointer, for the rest of the session; the sticks' dead zone;
    // which shortcut switches it, the menu and grip one and the chords, and
    // each grip as a bumper; each controller as last read and the pad as it
    // last went to Java; whether it is plugged in, whether it is resting under
    // a panel or for want of focus, and what is held back until let go;
    // panels to be put away now the controllers have gone to the pad; and
    // what the log last said about it
    int padMode;
    float padDeadzone;
    int padShortcut;
    PadToggle padToggle;
    PadChord padChord;
    int padGrip[HAND_COUNT];
    PadHand padRead[HAND_COUNT];
    PadState pad;
    int padAttached;
    int padResting;
    int padHeld;
    int padPutAway;
    int padButtonsSaid;
    // The host's rumble on the pad, and for the log: whether it is on, the
    // pulses armed since it came on, and whether each controller's first
    // pulse has said how the runtime took it
    PadRumble rumble;
    int rumbleOn;
    long rumblePulses;
    int rumbleResultSaid[HAND_COUNT];
    // The bundled controller model, drawn at each hand's grip into a
    // projection layer of its own over the picture: whether it is wanted, the
    // setting the Display tab's row also writes, and its buffers once Java has
    // handed it over
    int modelOn;
    int modelReady;
    GLuint modelVertexBuffer;
    GLuint modelIndexBuffer;
    int modelIndexCount;
    // What its pass draws with, made the first time a model shows: the
    // program, a side by side swapchain cleared to nothing round the models,
    // a depth buffer so the two hands hide each other properly, and the eyes
    // the image was last drawn from. One failure is enough to stop trying.
    GLuint modelProgram;
    GLint modelViewProjUniform;
    GLint modelMatrixUniform;
    XrSwapchain modelSwapchain;
    uint32_t modelImageCount;
    XrSwapchainImageOpenGLESKHR* modelImages;
    GLuint modelFbo;
    GLuint modelDepthBuffer;
    int modelEyeWidth;
    int modelEyeHeight;
    int modelPassReady;
    int modelPassFailed;
    XrView modelViews[ROOM_EYES];
    int modelRendered;
    // Its own pair of timer queries, as the room has
    GLuint modelTimerQueries[2];
    int modelTimerSlot;
    int modelTimerPending[2];
    int modelTimerPendingFrames[2];
    int64_t modelGpuTotalNs;
    long modelGpuSamples;
    long modelGpuDropped;
    // Each hand's grip this frame where its model shows, whether any does,
    // and what the log last said
    XrAction gripAction;
    XrSpace gripSpaces[HAND_COUNT];
    int modelShown[HAND_COUNT];
    XrPosef modelGrip[HAND_COUNT];
    int modelsShowing;
    int modelsSaid;
    // The input pass synced the actions this frame, which the grips are read
    // through, so they are not synced twice
    int actionsSynced;
    XrVector3f beamStart;
    XrVector3f beamEnd;
    XrVector3f headPos;
    // How fast the head is turning in the world, degrees a second, and the
    // orientation it was at last frame. A drag the eyes started holds still
    // while this is high.
    float headTurnRate;
    XrQuaternionf headTurnLast;
    int headTurnLastValid;
    // A drag the eyes started is being held for it, so the log says so once
    int dragHeldByHead;
    XrQuaternionf screenOrientation;
    float beamWidth;
    // The head's yaw against the screen as last located, for IN_HEAD_YAW
    float audioYaw;

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
    // In a room a resize moves the share of the room's screen the picture
    // fills rather than its width, from the share it was picked up at and from
    // how far along the half diagonal the ray was then, and the room it was
    // left in owes that share to the preference until a frame has the setting
    // slot free to carry it
    int grabRoomPercent;
    float grabRoomReach;
    // A grab the eyes picked up and a hand is carrying: where that hand was
    // when it pinched, how much its travel is geared up by, and the ramp
    int grabByGaze;
    Vec3 grabHandStart;
    float grabScale;
    DragRamp grabRamp;
    int roomScreenUnsaved;
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
    // How many cells of the grid have something behind them. The rest are
    // blank tiles, which neither hover nor take a press.
    int pickerCells;
    int envButtonHot;
    // The choice the last environment line was written for, so reapplying the
    // same one does not repeat it
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
    // And the face it was started on, since the first tab's rows are another
    // set entirely once a room is up.
    int cogDragSlider;
    int cogDragHand;
    int cogDragFace;
    // The same for a slider the eyes picked: the hand runs the thumb along
    // from where the press put it
    int cogDragByGaze;
    Vec3 cogDragHandStart;
    float cogDragScale;
    float cogDragStartU;
    DragRamp cogDragRamp;
    // The strip of percents beside the Room tab's tracks, and the values it
    // was last drawn with, so it only shows once it says what the rows do
    XrSwapchain cogReadoutSwapchain;
    uint32_t cogReadoutImageCount;
    XrSwapchainImageOpenGLESKHR* cogReadoutImages;
    int cogReadoutReady;
    int cogReadoutDrawn[READOUT_VALUES];
    // The marks on the display tab's cells, one strip for all its rows,
    // redrawn in Java whenever one of them moves
    XrSwapchain cogMarksSwapchain;
    uint32_t cogMarksImageCount;
    XrSwapchainImageOpenGLESKHR* cogMarksImages;
    int cogMarksReady;
    // The clock line over the panel
    XrSwapchain cogClockSwapchain;
    uint32_t cogClockImageCount;
    XrSwapchainImageOpenGLESKHR* cogClockImages;
    int cogClockReady;
    // Whether a press on a panel ticks, the setting the display tab's row
    // also writes, and a press this frame for Java to tick for
    int clickSoundOn;
    int clickPending;
    // The head lock preference as the last frame handed it down, for the
    // display tab's marks
    int headLockedPref;
    // The row under the ray, and on the display tab the cell within it, and
    // on a track which step button, -1 or 1, or 0 for the run or none
    int cogHoverSlider;
    int cogHoverCell;
    int cogHoverStep;
    // Frozen when the panel opens rather than followed every frame. The
    // distance slider moves the screen, and a panel anchored to the screen
    // would drag the thumb out from under the ray mid drag.
    XrPosef cogPose;
    float cogW, cogH;
    // The keyboard. One swapchain per state, all filled at startup, so shift
    // is a different handle in the layer rather than an upload. A sheet is
    // drawn again only when the modifiers lit on it change.
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
    // Ctrl, Alt and Win as lit on the keyboard, KB_MOD_ bits, which Java holds
    // down on the host for as long as they stay lit
    int kbMods;
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

    // The 3D switch on the bar, its art off and on, one swapchain each. Only
    // made in a session with stereo to switch.
    XrSwapchain stereoButtonSwapchains[2];
    uint32_t stereoButtonImageCounts[2];
    XrSwapchainImageOpenGLESKHR* stereoButtonImages[2];
    int stereoButtonReady;
    int stereoButtonHot;

    // The ray's switch on the bar, its art off and on, the same arrangement
    XrSwapchain rayButtonSwapchains[2];
    uint32_t rayButtonImageCounts[2];
    XrSwapchainImageOpenGLESKHR* rayButtonImages[2];
    int rayButtonReady;
    int rayButtonHot;

    // Head aim's switch on the bar, the same again, only shown while head
    // aim can act
    XrSwapchain aimButtonSwapchains[2];
    uint32_t aimButtonImageCounts[2];
    XrSwapchainImageOpenGLESKHR* aimButtonImages[2];
    int aimButtonReady;
    int aimButtonHot;

    // Gamepad mode's switch on the bar, pointer and gamepad, the same again
    XrSwapchain padButtonSwapchains[2];
    uint32_t padButtonImageCounts[2];
    XrSwapchainImageOpenGLESKHR* padButtonImages[2];
    int padButtonReady;
    int padButtonHot;

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

    // Report a problem. One sheet, drawn again in Java whenever what it shows
    // changes, and not shown until Java has drawn it for this opening. Up
    // with the keyboard under it, which types into it rather than the host
    // while it is. The part under the ray, whether Send can go as Java last
    // said, the About tab's button being under the ray, and the pose frozen
    // when it opened.
    XrSwapchain reportSwapchain;
    uint32_t reportImageCount;
    XrSwapchainImageOpenGLESKHR* reportImages;
    int reportReady;
    int reportOpen;
    int reportHoverZone;
    int reportSendReady;
    int cogReportHot;
    XrPosef reportPose;
    float reportW, reportH;
    // The About tab's Ko-fi button being under the ray
    int cogKofiHot;
    // The sheet that button opens: its art, whether it is up, whether its
    // Close button is under the ray, and the pose frozen when it opened
    int kofiOpen;
    XrSwapchain kofiSwapchain;
    uint32_t kofiImageCount;
    XrSwapchainImageOpenGLESKHR* kofiImages;
    int kofiReady;
    int kofiHoverZone;
    XrPosef kofiPose;
    float kofiW, kofiH;

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
    // The running model's own pair, as the warp takes them, which the 3D tab's
    // reset goes back to, and the separations its presets write, in the
    // preference's units and cell order. Handed down by Java before the first
    // frame.
    float defaultSeparation;
    float defaultConvergence;
    int presetUnits[COG_PRESET_CELLS];

    long statFrames;
    int64_t statTotalNs;
    int64_t statMaxNs;

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
    int64_t gpuTotalNs;
    int64_t gpuMaxNs;
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
    int64_t roomGpuTotalNs;
    long roomGpuSamples;
    long roomGpuDropped;

    // Separate accumulator so reading the number for the overlay does not
    // disturb the logcat cadence
    int64_t overlayGpuTotalNs;
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
const char* xrResultName(XrResult res);

// xr_gl.c: GL setup, the warp passes and the GPU timer
extern const float VERTEX_DATA[16];
extern PFNGENQUERIESEXT pfnGenQueries;
extern PFNBEGINQUERYEXT pfnBeginQuery;
extern PFNENDQUERYEXT pfnEndQuery;
extern PFNGETQUERYOBJECTUIVEXT pfnGetQueryObjectuiv;
extern PFNGETQUERYOBJECTUI64VEXT pfnGetQueryObjectui64v;
extern PFNDELETEQUERIESEXT pfnDeleteQueries;
GLuint compileShader(GLenum type, const char* src);
void setGradeUniforms(XrCtx* ctx, GLint onUniform, GLint gradeUniform, int on);
int linkProgram(GLuint* out, const char* fragmentSrc, const char* what);
int initGl(XrCtx* ctx);
void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation);

// xr_depth.c: the depth model staging and the stage thread's uploads
int initDepthModel(XrCtx* ctx);
void waitForDepthSlot(XrCtx* ctx);

// xr_ambilight.c: the frame colour sample, letterbox detection and the glow
int initAmbilight(XrCtx* ctx);
void ambiEffective(XrCtx* ctx, int* on, float* level);
void runAmbiBarDetect(XrCtx* ctx, const float* texMatrix);
void finishAmbiBarDetect(XrCtx* ctx);
void runFrameColorSample(XrCtx* ctx, const float* texMatrix);
void runGlowRender(XrCtx* ctx);
// The glow's switch or level moved since it was last drawn, and the frame
// already latched drawn into it again
int glowStale(XrCtx* ctx);
void redrawGlow(XrCtx* ctx, const float* texMatrix);

// xr_room.c: the 3d rooms
int roomEffective(XrCtx* ctx);
int roomResizable(int style);
void roomLevelsFromTable(XrCtx* ctx);
int roomScreenPercent(XrCtx* ctx, int style);
int roomGlowOn(XrCtx* ctx, int style);
void applyRoomPlacement(XrCtx* ctx, int style, float aspect, int reseeded);
void prepareRoom(XrCtx* ctx);
void renderRoom(XrCtx* ctx);
void worldEyeSize(XrCtx* ctx, int* outW, int* outH);

// xr_model.c: the bundled controller model
void updateControllerModels(XrCtx* ctx);
void renderControllerModels(XrCtx* ctx);

// xr_input.c: actions, hands, the ray and the per frame input pass
int initXrInput(XrCtx* ctx);
void destroyXrInput(XrCtx* ctx);
void refreshInputSource(XrCtx* ctx);
int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature);
void recentreScreen(XrCtx* ctx);

// xr_ui.c: where the furniture and the panels sit, and what the ray is over
int furnitureOnStandIn(XrCtx* ctx);
XrPosef furniturePose(XrCtx* ctx);
float furnitureWidth(XrCtx* ctx);
float furnitureHeight(XrCtx* ctx);
BarFrame barFrame(XrCtx* ctx);
int barButtonHit(XrCtx* ctx, int slot, float u, float v, const BarFrame* frame);
float cornerSide(XrCtx* ctx);
float effectiveCurvature(XrCtx* ctx);
int cogFace(XrCtx* ctx);
int cogArt(XrCtx* ctx);
float screenPitch(XrCtx* ctx);
XrQuaternionf screenOrient(float yaw, float pitch, float roll);
float screenRoll(XrCtx* ctx);
XrPosef pickerPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef cogPanelPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef kbPanelPose(XrCtx* ctx, float* outWidth, float* outHeight);
int kbKeyAt(XrCtx* ctx, float u, float v);
int headAimCanAct(XrCtx* ctx);
void setHeadAimOn(XrCtx* ctx, int on, const char* from);
void setPadMode(XrCtx* ctx, int on, const char* from);
const char* padShortcutName(int shortcut);
void setStereoLive(XrCtx* ctx, int on, const char* from);
void setRayOn(XrCtx* ctx, int on, const char* from);
int panelUp(XrCtx* ctx);
XrPosef exitPromptPose(XrCtx* ctx, float* outWidth, float* outHeight);
int exitPromptZone(float u, float v);
XrPosef reportSheetPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef handHintPose(XrCtx* ctx, float* outWidth, float* outHeight);
XrPosef kofiSheetPose(XrCtx* ctx, float* outWidth, float* outHeight);
int cogTabRowCount(int face);
int cogRowIsTrack(int face, int row);
int cogRowLive(XrCtx* ctx, int face, int row);
float cogSliderValue(XrCtx* ctx, int face, int slider);
void cogApplySlider(XrCtx* ctx, int face, int slider, float pu);
int cogOptionCells(int option);
int cogOptionValue(XrCtx* ctx, int option, int headLocked);
int cogApplyOption(XrCtx* ctx, int option, int cell);
int cogRowCells(int face, int row);
int cogRoomCellValue(XrCtx* ctx, int row);
void cogApplyRoomCell(XrCtx* ctx, int row, int cell, float* out);
int cogCellInForce(XrCtx* ctx, int face, int row);
void cogApplyCell(XrCtx* ctx, int face, int row, int cell, float* out);
void cogDragEnded(XrCtx* ctx, float* out);
void cogStepTrack(XrCtx* ctx, int face, int row, int dir, float* out);
int cogCellAt(float pu, int cells);
void cogReadouts(XrCtx* ctx, int* values);
void pictureSet(XrCtx* ctx, int row, int units);
void pictureReset(XrCtx* ctx);

// xr_assets.c: the swapchains the art goes into and the uploads that fill them
int createArtSwapchain(XrCtx* ctx, int width, int height, const char* what,
                       XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images,
                       uint32_t* count);
void destroyArtSwapchain(XrSwapchain* chain, XrSwapchainImageOpenGLESKHR** images);
int createPointerSwapchain(XrCtx* ctx);
int uploadPointerArt(XrCtx* ctx);
int roomStyleForCell(int cell);
int roomCellForStyle(int style);

// xr_display.c: the display refresh rate and the performance levels
void probeDisplayExtensions(XrCtx* ctx);
void startDisplay(XrCtx* ctx);
void startPerfLevels(XrCtx* ctx);
void setPerfLevel(XrCtx* ctx, int level);
void perfNotice(XrCtx* ctx, const XrEventDataPerfSettingsEXT* notice);
void displaySessionBegun(XrCtx* ctx);
void displayFocused(XrCtx* ctx);
void displayRateChanged(XrCtx* ctx, float from, float to);
void displayFrameBegun(XrCtx* ctx, const XrFrameState* state);
void displayFrameEnded(XrCtx* ctx);
void setRefreshKnob(XrCtx* ctx, int hz);
void setDepthRate(XrCtx* ctx, int perSecond, const char* why);

// xr_debug.c: setprop knobs and frame capture
void readStartKnobs(XrCtx* ctx);
int startFailKnob(const char* step);
void propFlag(const char* name, int* target);
void pollCaptureRequest(XrCtx* ctx);
void writeCapture(XrCtx* ctx, const char* what, const void* data, size_t bytes);
void writeCaptureDepthTexture(XrCtx* ctx);

#endif
