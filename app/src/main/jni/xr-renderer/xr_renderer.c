// OpenXR presentation for the decoded video stream. The decoder feeds a
// SurfaceTexture whose OES texture lives in the EGL context created here.
// Each new video frame is drawn into a single swapchain that the compositor
// shows on a quad (or cylinder) visible to both eyes, so the compositor does
// all the reprojection work. The 3d room is the one exception: it is real
// geometry drawn per eye into a projection layer, in place of the environment.

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

#define TAG "moonlight-xr"

// Matches the levels in FileLog on the Java side
#define FILE_LOG_OFF 0
#define FILE_LOG_BASIC 1
#define FILE_LOG_VERBOSE 2

// Not an Android priority, only ever seen inside xrLog: a key event, which
// logcat gets as info while the file keeps it at the quiet level
#define PRIO_EVENT 90

static void xrLog(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGI(...) xrLog(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) xrLog(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) xrLog(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGEV(...) xrLog(PRIO_EVENT, __VA_ARGS__)

// Where the Java side put the log, if the user turned it on. Globals rather
// than context fields, since this is configured before any context exists.
static char fileLogPath[512];
static int fileLogLevel = FILE_LOG_OFF;

// The log is held open rather than reopened per line. In the Download folder
// every open goes through the media provider, which measured 339 us on
// average and 10 ms at worst, and this writes from the render and depth
// threads with nothing between it and the frame.
static int logFd = -1;
static ino_t logIno;
static pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Appends one line to the file the Java logger is also writing, in a single
 * write so the two writers cannot interleave halfway through a line.
 *
 * The fd is kept across calls, so it has to be checked before each append:
 * when the log fills, the Java side deletes it out from under us, and the fd
 * would go on filling an inode nothing can reach any more. Comparing the
 * inode at the path against the one we opened catches that, and a user
 * deleting the file by hand, for one stat rather than one open.
 */
static void writeFileLog(int prio, const char* fmt, va_list args) {
    char level;
    if (prio == ANDROID_LOG_ERROR) {
        level = 'E';
    }
    else if (prio == ANDROID_LOG_WARN) {
        level = 'W';
    }
    else if (prio == PRIO_EVENT) {
        level = 'I';
    }
    else if (fileLogLevel >= FILE_LOG_VERBOSE) {
        level = 'V';
    }
    else {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm parts;
    localtime_r(&ts.tv_sec, &parts);

    char line[1024];
    int n = snprintf(line, sizeof(line), "%02d-%02d %02d:%02d:%02d.%03d %c xr: ",
                     parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                     parts.tm_sec, (int)(ts.tv_nsec / 1000000), level);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return;
    }

    size_t room = sizeof(line) - (size_t)n - 1;
    int m = vsnprintf(line + n, room, fmt, args);
    if (m < 0) {
        return;
    }
    if ((size_t)m >= room) {
        m = (int)room - 1;
    }
    n += m;
    line[n++] = '\n';

    pthread_mutex_lock(&logMutex);

    if (logFd >= 0) {
        struct stat current;
        if (stat(fileLogPath, &current) != 0 || current.st_ino != logIno) {
            close(logFd);
            logFd = -1;
        }
    }

    if (logFd < 0) {
        struct stat opened;
        logFd = open(fileLogPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (logFd < 0 || fstat(logFd, &opened) != 0) {
            if (logFd >= 0) {
                close(logFd);
                logFd = -1;
            }
            // The next line tries again
            pthread_mutex_unlock(&logMutex);
            return;
        }
        logIno = opened.st_ino;
    }

    ssize_t ignored = write(logFd, line, (size_t)n);
    (void)ignored;

    pthread_mutex_unlock(&logMutex);
}

static void xrLog(int prio, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(prio == PRIO_EVENT ? ANDROID_LOG_INFO : prio, TAG, fmt, args);
    va_end(args);

    if (fileLogLevel == FILE_LOG_OFF || fileLogPath[0] == '\0') {
        return;
    }
    va_start(args, fmt);
    writeFileLog(prio, fmt, args);
    va_end(args);
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
#define POINTER_D_CUTOFF 1.0f
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

// Slots in the float array handed back to Java each frame
#define IN_HIT      0
#define IN_U        1
#define IN_V        2
#define IN_BUTTONS  3
#define IN_SCROLL   4
#define IN_POINTER  5
#define IN_POSE_DIRTY 6
// Which setting the panel just changed, or -1. Zero is a real id, so this one
// has to be said explicitly rather than left at the memset.
#define IN_SETTING  7
// x y z, then the orientation quaternion, then width, cylinder radius and the
// curvature the panel asked for, ten in all
#define IN_POSE     8
// The cell just chosen in the environment grid, or -1
#define IN_PICKER_PICK 18
#define IN_SETTING_VALUE 19
// The key the in world keyboard just typed, or -1. Unicode with the shift
// already applied, plus the four control codes below 32.
#define IN_KEY      20
#define IN_SLOTS    21

// Settings the panel can hand back to Java to be applied and stored
#define SETTING_SHARPEN 0
#define SETTING_STATS   1
#define SETTING_SEPARATION 2
#define SETTING_CONVERGENCE 3
#define SETTING_RESET_3D 4
#define SETTING_AMBILIGHT 5
#define SETTING_AMBI_LEVEL 6
#define SETTING_ROOM_LIGHT 7

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
// pointing around the grid never costs an upload. One band per category: a
// header strip carrying its name, then a row of cells under it. Must match the
// PICKER_ constants in XrRenderer.java.
#define PICKER_COLS 4
#define PICKER_ROWS 2
#define PICKER_CELLS (PICKER_COLS * PICKER_ROWS)
#define PICKER_TEX_W 1024
#define PICKER_HEADER_PX 40
#define PICKER_CELL_PX 256
#define PICKER_BAND_PX (PICKER_HEADER_PX + PICKER_CELL_PX)
#define PICKER_TEX_H (PICKER_BAND_PX * PICKER_ROWS)
// Widened along with the grid so a cell stays about the size it was at three
// columns
#define PICKER_WIDTH_FRAC 0.73f
#define OUTLINE_TEX 128
// The room cells in the grid. Must match CELL_MINIMAL_ROOM and CELL_PSX_CINEMA
// in XrRenderer.java.
#define ENV_CELL_MINIMAL_ROOM 2
#define ENV_CELL_PSX_CINEMA 3
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
#define LOCK_TEX 384

// Settings panel. Same shape as the picker: the art is drawn in Java and shown
// on one quad, the thumbs are separate little quads so dragging one never costs
// an upload.
#define COG_TEX_W 768
#define COG_TEX_H 640
#define COG_WIDTH_FRAC 0.36f
// The button that opens it, sitting to the right of the move bar, the same
// size as the environment button on the left
#define COG_BUTTON_FRAC 0.048f
#define COG_THUMB_TEX 64

// Where the tabs, rows and tracks sit in the panel texture. These must match
// the COG_ constants in XrRenderer.java, which is what draws them.
#define COG_TRACK_L 0.42f
#define COG_TRACK_R 0.93f
// Anything above this is the tab bar, split evenly between the tabs
#define COG_TAB_BAR_B 0.16f
// Six rows on the screen tab, so they start a little higher and sit closer
// together than they did at five
#define COG_ROW_V0 0.25f
#define COG_ROW_STEP 0.11f
// Half height of a row's hit band. Under half the pitch, so neighbouring
// bands stay disjoint.
#define COG_ROW_HALF 0.05f
#define COG_RESET_L 0.35f
#define COG_RESET_R 0.65f
// Clear of the last row, which reaches 0.80 plus the half band
#define COG_RESET_T 0.87f
#define COG_RESET_B 0.97f
// Half height of an option cell, so the ring drawn over one matches the art
#define COG_CELL_HALF 0.045f

// One texture per tab, all uploaded once, so switching costs a swapchain
// handle rather than an upload
#define COG_TAB_SCREEN  0
#define COG_TAB_DISPLAY 1
#define COG_TAB_3D      2
#define COG_TAB_COUNT   3
// And one more sheet than there are tabs: the screen tab has a second face for
// when a room hangs the picture and none of its rows can do anything
#define COG_ART_ROOM_SCREEN 3
#define COG_ART_COUNT       4

// Screen tab rows, in the order they are drawn
#define COG_SLIDER_DISTANCE 0
#define COG_SLIDER_HEIGHT   1
#define COG_SLIDER_TILT     2
#define COG_SLIDER_ROTATE   3
#define COG_SLIDER_CURVE    4
#define COG_SLIDER_SIZE     5
#define COG_SLIDER_COUNT    6

// 3D tab rows, sliders like the screen tab's. Only values that take effect the
// moment they move belong here: the depth source itself is settled when the
// session starts, so it stays in the 2d settings.
#define COG_ROW3D_SEPARATION 0
#define COG_ROW3D_CONVERGENCE 1
#define COG_ROW3D_COUNT 2
// Right hand end of the separation track, as a fraction of frame width. Three
// times the 0.5 percent that phase 6 measured as the useful maximum: past
// there depth stops growing and only the strain does, so the far end of the
// track is drawn marked rather than left off.
#define COG_SEP_MAX 0.015f
// Steps along that track, so a dragged value lands exactly on one of the
// tenths of a percent the preference is stored in
#define COG_SEP_STEPS 15

// Display tab rows. Cells rather than a track, so a press picks one instead of
// dragging a value.
#define COG_OPTION_SHARPEN 0
#define COG_OPTION_STATS   1
#define COG_OPTION_AMBILIGHT 2
#define COG_OPTION_ROOM_LIGHT 3
#define COG_OPTION_COUNT   4
#define COG_SHARPEN_CELLS 3
#define COG_STATS_CELLS   2
#define COG_AMBI_CELLS    2
#define COG_ROOM_LIGHT_CELLS 2
// The one row on this tab that is a track rather than cells, under the option
// rows, so the glow can be turned down without leaving the tab it lives on
#define COG_DISPLAY_SLIDER_ROW 4
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

// In world keyboard, for the login boxes and chat windows that turn up mid
// stream. One sheet of art per state, drawn in Java like the other panels, and
// the layout arrives with it: this side is handed rectangles and codes and
// knows nothing else about what the keys say.
#define KB_TEX_W 1120
#define KB_TEX_H 448
#define KB_WIDTH_FRAC 0.55f
#define KB_MAX_KEYS 64
#define KB_STATE_LOWER   0
#define KB_STATE_UPPER   1
#define KB_STATE_SYMBOLS 2
#define KB_STATE_COUNT   3
// Codes under zero change the keyboard instead of typing. Everything at or
// above 8 is sent on as it stands.
#define KB_CODE_SHIFT   -2
#define KB_CODE_SYMBOLS -3
#define KB_CODE_HIDE    -4

#define HOVER_ENVBUTTON 4
#define HOVER_PICKER    5
// Nothing under the ray, but close enough to the screen to keep drawing it
#define HOVER_HALO      6
#define HOVER_LOCK      7
#define HOVER_COGBUTTON 8
#define HOVER_COGPANEL  9
#define HOVER_KBBUTTON  10
#define HOVER_KBPANEL   11
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

// The 3d room. A dark interior, drawn per eye into the one projection layer
// this renderer has, instead of the environment sphere. Which room, 0 for none:
// 1 is generated here, 2 is the baked model that ships in the assets.
#define ROOM_STYLE_MINIMAL 1
#define ROOM_STYLE_PSX 2
#define ROOM_EYES 2
// The whole of what the runtime recommends per eye, capped here. Half of it
// was soft enough against the video layer beside it to see, and this is what
// the room costs to keep its edges as sharp. A Gen 1 headset stays on half,
// capped at the smaller number, since it has neither the memory nor the fill
// rate for the rest.
#define ROOM_MAX_EYE_FULL 2560
#define ROOM_MAX_EYE 1280
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

// Pinned headers may predate the extension, values from the OpenXR registry
#ifndef XR_FB_composition_layer_settings
#define XR_FB_composition_layer_settings 1
#define XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME "XR_FB_composition_layer_settings"
#define XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB ((XrStructureType)1000204000)
typedef XrFlags64 XrCompositionLayerSettingsFlagsFB;
static const XrCompositionLayerSettingsFlagsFB XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB = 0x00000004;
static const XrCompositionLayerSettingsFlagsFB XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB = 0x00000008;
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
#define PROP_AIM_CUTOFF "debug.moonlight.aimcutoff"
#define PROP_AIM_BETA "debug.moonlight.aimbeta"
#define PROP_BEAM_WIDTH "debug.moonlight.beamwidth"
#define PROP_POINTER_WAKE "debug.moonlight.pointerwake"
#define PROP_POINTER_SLEEP "debug.moonlight.pointersleep"
#define PROP_ENV_RADIUS "debug.moonlight.envradius"
#define PROP_SHARPEN "debug.moonlight.sharpen"
#define PROP_AMBILIGHT "debug.moonlight.ambilight"
#define PROP_AMBI_SMOOTH "debug.moonlight.ambismooth"
#define PROP_ROOM "debug.moonlight.room"
#define PROP_ROOM_SCALE "debug.moonlight.roomscale"
#define PROP_ROOM_DIM "debug.moonlight.roomdim"

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

// One euro on a rotation: angular speed drives the cutoff and the blend
// is a lerp toward the new sample, which is fine at per frame angles
typedef struct {
    int valid;
    XrQuaternionf q;
    float dAngle;
} EuroQuatState;

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
    GLuint ambiTexture;
    GLuint ambiFbo;
    // Whether there is anything in that texture yet, since the first frame has
    // nothing to smooth against
    int ambiSeeded;
    float ambiSmooth;
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
    // What the runtime asks for per eye, read once at startup. Only the room
    // has any use for it.
    int recommendedEyeWidth;
    int recommendedEyeHeight;
    // Whether this is one of the XR2 Gen 1 headsets, decided on the Java side.
    // Only the room reads it, to draw itself smaller there.
    int gen1Headset;

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
    // A press the grid, the panel or the keyboard took for itself, held until
    // the trigger is released. The host's left button is level based, so
    // without this the press that picks something goes on to click whatever the
    // modal was covering as soon as it closes.
    int triggerSwallowed[SRC_COUNT];
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

// Feeds the ambilight. Thirty two square is coarse enough to read as a wash
// rather than as a blurred copy of the picture, and the same 4x4 box the depth
// downscale uses is what keeps it steady: one tap per output texel and the
// colours crawl as the sample points cross detail in the frame.
static const char* AMBI_FRAGMENT_SRC =
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
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 / 32.0);\n"
    "            vec2 tc = v_plain + off;\n"
    "            sum += texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
    "}\n";

// The glow itself. The quad is larger than the screen, so the middle of it
// covers the picture and only the border is ever seen. Sampling the colour
// texture over that middle and letting the clamp carry the edge texels outward
// is what spreads the frame's colours into the space around it.
static const char* GLOW_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_intensity;\n"
    "out vec4 fragColor;\n"
    // 1.7 is GLOW_SCALE and 32.0 is AMBI_SAMPLE_TEX, both kept in step by hand
    "const float scale = 1.7;\n"
    "const float size = 32.0;\n"
    "void main() {\n"
    "    vec2 uv = v_plain;\n"
    "    vec2 fuv = (uv - 0.5) * scale + 0.5;\n"
    // A cubic B spline over the colour texture, done as four bilinear fetches.
    // Plain bilinear puts a crease at every texel boundary, and magnified this
    // far those creases are the lines that showed across the glow. This kernel
    // approximates rather than interpolates, so it smooths the texel to texel
    // steps on the way as well.
    "    vec2 tc = fuv * size - 0.5;\n"
    "    vec2 base = floor(tc);\n"
    "    vec2 f = tc - base;\n"
    "    vec2 f2 = f * f;\n"
    "    vec2 f3 = f2 * f;\n"
    "    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;\n"
    "    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
    "    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;\n"
    "    vec2 w3 = f3 / 6.0;\n"
    // Each pair of taps folds into one bilinear fetch placed between them, so
    // sixteen texel reads come out of four
    "    vec2 g0 = w0 + w1;\n"
    "    vec2 g1 = w2 + w3;\n"
    "    vec2 h0 = (base - 0.5 + w1 / g0) / size;\n"
    "    vec2 h1 = (base + 1.5 + w3 / g1) / size;\n"
    "    vec3 c00 = texture(u_texture, vec2(h0.x, h0.y)).rgb;\n"
    "    vec3 c10 = texture(u_texture, vec2(h1.x, h0.y)).rgb;\n"
    "    vec3 c01 = texture(u_texture, vec2(h0.x, h1.y)).rgb;\n"
    "    vec3 c11 = texture(u_texture, vec2(h1.x, h1.y)).rgb;\n"
    "    vec3 color = mix(mix(c11, c01, g0.x), mix(c10, c00, g0.x), g0.y);\n"
    // Distance out into the border, 0 at the screen edge and 1 at the rim
    "    vec2 d = max(abs(uv - 0.5) - 0.5 / scale, 0.0) / (0.5 - 0.5 / scale);\n"
    "    float t = min(length(d), 1.0);\n"
    // Flat at both ends, so neither the start of the fade nor the rim draws a
    // line of its own. Squared to keep about the strength the plain curve had.
    "    float s = t * t * (3.0 - 2.0 * t);\n"
    "    float fall = (1.0 - s) * (1.0 - s);\n"
    "    float a = fall * u_intensity;\n"
    // Premultiplied, which is what the runtime composites the panel art as
    "    fragColor = vec4(color * a, a);\n"
    "}\n";

// The 3d room. Both the colouring of the generated room and the light the
// picture throws are worked out per vertex: the geometry is a few hundred
// vertices, and the only thing that changes frame to frame is how much of the
// screen's light lands on each of them. All the fragment side does is pick
// between that vertex colour and the atlas a baked room is textured with, which
// is what keeps a full screen projection layer affordable.
static const char* ROOM_VERTEX_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec3 a_position;\n"
    "in vec3 a_color;\n"
    "in float a_spill;\n"
    "in vec2 a_uv;\n"
    "uniform mat4 u_viewproj;\n"
    "uniform sampler2D u_ambi;\n"
    "uniform float u_spillGain;\n"
    "out vec3 v_color;\n"
    "out vec3 v_wash;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    // Five taps over the frame's colour texture, centre and the middle of each
    // edge. A wash of light on a wall carries no more detail than that.
    "    vec3 lit = texture(u_ambi, vec2(0.5, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.15, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.85, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.5, 0.15)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.5, 0.85)).rgb;\n"
    "    vec3 wash = lit * 0.2;\n"
    "    v_color = a_color;\n"
    "    v_wash = wash * (a_spill * u_spillGain);\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_viewproj * vec4(a_position, 1.0);\n"
    "}\n";

static const char* ROOM_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 v_color;\n"
    "in vec3 v_wash;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_room;\n"
    "uniform float u_texMix;\n"
    "uniform float u_dim;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    // A generated room is at mix 0 and a textured one at 1. The sample happens
    // either way, so a white 1x1 stands in while nothing else is loaded.
    "    vec3 base = mix(v_color, texture(u_room, v_uv).rgb * u_dim, u_texMix);\n"
    "    fragColor = vec4(base + v_wash, 1.0);\n"
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

    const char* enabledExts[10];
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
    if (ctx->layerSettingsSupported) {
        enabledExts[enabledCount++] = XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME;
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
    if (blendModeCount > 0) {
        XrEnvironmentBlendMode* modes = calloc(blendModeCount, sizeof(XrEnvironmentBlendMode));
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
    if (configViewCount > 0) {
        XrViewConfigurationView* configViews =
                calloc(configViewCount, sizeof(XrViewConfigurationView));
        for (uint32_t i = 0; i < configViewCount; i++) {
            configViews[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        }
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
                ctx->instance, ctx->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                configViewCount, &configViewCount, configViews))) {
            ctx->recommendedEyeWidth = (int)configViews[0].recommendedImageRectWidth;
            ctx->recommendedEyeHeight = (int)configViews[0].recommendedImageRectHeight;
            LOGI("recommended render size %dx%d per eye, %u views",
                 ctx->recommendedEyeWidth, ctx->recommendedEyeHeight, configViewCount);
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

    LOGEV("swapchain %dx%d format %lld, %u images (stereo mode %d)", chainWidth, ctx->videoHeight,
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

// GL side of the ambilight: the colour texture the frame is sampled into and
// the program that spreads it over the glow quad. Set up whatever the depth
// settings are, since the glow needs no depth of any kind.
static int initAmbilight(XrCtx* ctx) {
    if (!linkProgram(&ctx->ambiProgram, AMBI_FRAGMENT_SRC, "frame colour sample")) {
        return 0;
    }
    ctx->ambiTexMatrixUniform = glGetUniformLocation(ctx->ambiProgram, "u_texmatrix");
    glUseProgram(ctx->ambiProgram);
    glUniform1i(glGetUniformLocation(ctx->ambiProgram, "u_texture"), 0);

    glGenTextures(1, &ctx->ambiTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // The glow reads past both ends of this on purpose, and the clamp is what
    // carries the frame's edge colours out to the rim of the quad
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &ctx->ambiFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->ambiTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("ambilight framebuffer incomplete: 0x%x", status);
        return 0;
    }

    if (!linkProgram(&ctx->glowProgram, GLOW_FRAGMENT_SRC, "glow")) {
        return 0;
    }
    ctx->glowIntensityUniform = glGetUniformLocation(ctx->glowProgram, "u_intensity");
    glUseProgram(ctx->glowProgram);
    glUniform1i(glGetUniformLocation(ctx->glowProgram, "u_texture"), 0);

    // Nothing attached yet: the target is whichever swapchain image the frame
    // acquires
    glGenFramebuffers(1, &ctx->glowFbo);

    LOGI("ambilight ready, sampling %dx%d into a %dx%d glow",
         AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX, GLOW_TEX, GLOW_TEX);
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
            pfnGenQueries(2, ctx->roomTimerQueries);
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

    if (!initAmbilight(ctx)) {
        return 0;
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

// Axis assumed normalised, which anything that came out of quatRotate on a
// unit vector already is
static XrQuaternionf axisAngleQuat(Vec3 axis, float angle) {
    float half = angle * 0.5f;
    float s = sinf(half);
    XrQuaternionf q = { axis.x * s, axis.y * s, axis.z * s, cosf(half) };
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

static XrQuaternionf euroFilterQuat(EuroQuatState* s, XrQuaternionf q, float dt,
                                    float minCutoff, float beta) {
    if (!s->valid || dt <= 0.0f) {
        s->valid = 1;
        s->q = q;
        s->dAngle = 0.0f;
        return q;
    }
    // Quaternions cover every rotation twice, so take the near side
    float dot = s->q.x * q.x + s->q.y * q.y + s->q.z * q.z + s->q.w * q.w;
    if (dot < 0.0f) {
        q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w;
        dot = -dot;
    }
    if (dot > 1.0f) {
        dot = 1.0f;
    }
    float speed = 2.0f * acosf(dot) / dt;
    s->dAngle += euroAlpha(POINTER_D_CUTOFF, dt) * (speed - s->dAngle);
    float cutoff = minCutoff + beta * s->dAngle;
    float a = euroAlpha(cutoff, dt);
    XrQuaternionf r = {
        s->q.x + a * (q.x - s->q.x),
        s->q.y + a * (q.y - s->q.y),
        s->q.z + a * (q.z - s->q.z),
        s->q.w + a * (q.w - s->q.w),
    };
    float len = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len > 0.0f) {
        r.x /= len; r.y /= len; r.z /= len; r.w /= len;
    }
    s->q = r;
    return r;
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

static float vecDot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Matrices are column major, the order GL takes them in, and out = a * b
static void matMul(float* out, const float* a, const float* b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] = a[row] * b[col * 4]
                    + a[4 + row] * b[col * 4 + 1]
                    + a[8 + row] * b[col * 4 + 2]
                    + a[12 + row] * b[col * 4 + 3];
        }
    }
}

// The runtime hands out four half angles rather than one field of view, since
// the two halves of an eye's frustum are not the same size on these headsets
static void projectionFromFov(float* m, XrFovf fov, float nearZ, float farZ) {
    float left = tanf(fov.angleLeft);
    float right = tanf(fov.angleRight);
    float down = tanf(fov.angleDown);
    float up = tanf(fov.angleUp);
    float width = right - left;
    float height = up - down;

    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / width;
    m[5] = 2.0f / height;
    m[8] = (right + left) / width;
    m[9] = (up + down) / height;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

// The inverse of a pose that is only a rotation and a translation, which is
// what turns where an eye is into where the world is relative to it
static void viewFromPose(float* m, XrPosef pose) {
    XrQuaternionf inv = quatConj(pose.orientation);
    Vec3 ex = { 1.0f, 0.0f, 0.0f };
    Vec3 ey = { 0.0f, 1.0f, 0.0f };
    Vec3 ez = { 0.0f, 0.0f, 1.0f };
    Vec3 rx = quatRotate(inv, ex);
    Vec3 ry = quatRotate(inv, ey);
    Vec3 rz = quatRotate(inv, ez);
    Vec3 eye = { pose.position.x, pose.position.y, pose.position.z };
    Vec3 t = quatRotate(inv, eye);

    m[0] = rx.x;  m[1] = rx.y;  m[2] = rx.z;  m[3] = 0.0f;
    m[4] = ry.x;  m[5] = ry.y;  m[6] = ry.z;  m[7] = 0.0f;
    m[8] = rz.x;  m[9] = rz.y;  m[10] = rz.z; m[11] = 0.0f;
    m[12] = -t.x; m[13] = -t.y; m[14] = -t.z; m[15] = 1.0f;
}

// Everything the room's shape and colouring is made of, gathered in one place
// so the look can be changed without reading the generator
typedef struct {
    float halfWidth;
    float floorY;
    // The floor under the picture, which is the one it must not hang through.
    // The same as floorY in a room with one level, lower in a raked one, where
    // floorY is the tier the viewer stands on.
    float screenFloorY;
    float ceilingY;
    // The wall the picture hangs on, and the one behind the viewer
    float screenZ;
    float backZ;
    // Quads per side on each face, so a face carries this squared of them
    int subdiv;
    // What each kind of surface is painted before the gradients go on
    float wallLevel;
    float floorLevel;
    float ceilingLevel;
    // Where the picture hangs, which is what the light is baked from
    Vec3 screenAt;
    // How high on the wall the picture is mounted, and how far off the wall it
    // stands so the two never fight for the same pixels
    float screenMountY;
    float screenProud;
    // How wide it is hung. The room sizes its own picture rather than taking
    // the size slider's, since the wall it goes on is a known size.
    float screenWidth;
    // Distance at which the screen's light is down to half
    float spillRadius;
    // How much of that light a fully lit vertex takes
    float spillGain;
    // 0 for a room painted by the generator, 1 for one taking its colour off a
    // texture atlas, and how far down that atlas is turned on the way in
    float texMix;
    float dim;
    unsigned seed;
} RoomParams;

// A dither of about one 255th, from the seed and the vertex number. Without it
// the wall gradients are shallow enough over enough pixels to band.
static float roomDither(unsigned seed, unsigned index) {
    unsigned h = seed + index * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return ((float)(h & 0xffffu) / 65535.0f - 0.5f) * (2.0f / 255.0f);
}

// What a point on the room is painted, before any of the screen's light lands
// on it. Near black throughout: everything here is a gradient between shades
// of almost nothing, and the picture is what the eye should be adapting to.
static void roomVertexColor(const RoomParams* p, int surface, Vec3 pos, float* rgb) {
    // 0 at the screen wall, 1 at the back of the room
    float back = (pos.z - p->screenZ) / (p->backZ - p->screenZ);
    // 0 on the floor, 1 at the ceiling
    float up = (pos.y - p->floorY) / (p->ceilingY - p->floorY);
    // 0 down the middle, 1 at either side wall
    float side = fabsf(pos.x) / p->halfWidth;

    if (surface == ROOM_SURF_FLOOR) {
        float level = p->floorLevel * (1.0f - 0.35f * back * back);
        // A shade warmer and a shade lighter than the walls
        rgb[0] = level * 1.00f;
        rgb[1] = level * 0.93f;
        rgb[2] = level * 0.84f;
        return;
    }

    float level;
    if (surface == ROOM_SURF_CEILING) {
        level = p->ceilingLevel * (1.0f - 0.30f * back);
    }
    else {
        // Darker toward the ceiling and darker again into the rear corners,
        // where a real room has nothing lighting it at all
        level = p->wallLevel * (1.0f - 0.45f * up)
                * (1.0f - 0.30f * back * back * (0.4f + 0.6f * side));
    }
    rgb[0] = level;
    rgb[1] = level;
    rgb[2] = level;
}

// How much of the picture's light reaches a point on the room. Baked from a
// single point where the screen sits by default: a distance and a facing term
// is as much of a light that size as a dark wall ever shows.
static float roomSpillWeight(const RoomParams* p, Vec3 pos, Vec3 normal) {
    Vec3 toScreen = vecSub(p->screenAt, pos);
    float dist = sqrtf(vecDot(toScreen, toScreen));
    if (dist < 1e-4f) {
        return 1.0f;
    }
    Vec3 dir = { toScreen.x / dist, toScreen.y / dist, toScreen.z / dist };
    float facing = vecDot(normal, dir);
    if (facing <= 0.0f) {
        return 0.0f;
    }

    float ratio = dist / p->spillRadius;
    float weight = facing / (1.0f + ratio * ratio);

    // Nothing behind the viewer catches any of it, faded in over the back half
    // of the room so the falloff has no edge in it
    if (pos.z > 0.0f && p->backZ > 0.0f) {
        float behind = 1.0f - pos.z / p->backZ;
        weight *= behind > 0.0f ? behind : 0.0f;
    }
    return weight;
}

// Writes one vertex in the layout the room's buffer is in. A generated room has
// no atlas behind it, so it passes 0,0 for the texture coordinate and the
// shader mixes it out.
static void roomWriteVertex(const RoomParams* p, float* verts, int index, Vec3 pos,
                            const float* rgb, float spill, float u, float v) {
    float dither = roomDither(p->seed, (unsigned)index);
    float* out = verts + (size_t)index * ROOM_VERTEX_FLOATS;
    out[0] = pos.x;
    out[1] = pos.y;
    out[2] = pos.z;
    out[3] = rgb[0] + dither;
    out[4] = rgb[1] + dither;
    out[5] = rgb[2] + dither;
    out[6] = spill;
    out[7] = u;
    out[8] = v;
    out[9] = 0.0f;
}

// The most a room can ask for, so a caller can size its buffers without
// knowing how the room is put together
static void roomMaxCounts(const RoomParams* p, int* maxVerts, int* maxIndices) {
    *maxVerts = ROOM_FACES * (p->subdiv + 1) * (p->subdiv + 1);
    *maxIndices = ROOM_FACES * p->subdiv * p->subdiv * 6;
}

// Builds the whole room, vertices and indices, into buffers the caller owns.
// The generated styles only, which is the bare shell: a baked room comes out of
// its own model file instead.
//
// Triangles are wound counter clockwise seen from inside the box, so the side
// the viewer is on faces front. Culling is left off all the same, since nothing
// in here is ever seen from behind and there is nothing for a cull to save.
static int buildRoomGeometry(const RoomParams* p, int style, float* verts, int maxVerts,
                             unsigned short* indices, int maxIndices,
                             int* vertexCount, int* indexCount) {
    int n = p->subdiv;
    if (n < 1 || style < ROOM_STYLE_MINIMAL) {
        return 0;
    }
    int needVerts = 0;
    int needIndices = 0;
    roomMaxCounts(p, &needVerts, &needIndices);
    // Past the index type is a badly chosen parameter rather than a room worth
    // drawing, so it fails here the same way a short buffer does
    if (needVerts > ROOM_MAX_VERTS || needVerts > maxVerts || needIndices > maxIndices) {
        return 0;
    }

    float width = p->halfWidth * 2.0f;
    float height = p->ceilingY - p->floorY;
    float depth = p->backZ - p->screenZ;

    // Origin and the two edges each face is swept along, ordered so that the
    // cross product of the two points into the room
    struct {
        Vec3 origin;
        Vec3 edgeU;
        Vec3 edgeV;
        int surface;
    } faces[ROOM_FACES] = {
        // The wall the picture hangs on
        { { -p->halfWidth, p->floorY, p->screenZ },
          { width, 0.0f, 0.0f }, { 0.0f, height, 0.0f }, ROOM_SURF_WALL },
        // Behind the viewer
        { { p->halfWidth, p->floorY, p->backZ },
          { -width, 0.0f, 0.0f }, { 0.0f, height, 0.0f }, ROOM_SURF_WALL },
        { { -p->halfWidth, p->floorY, p->screenZ },
          { 0.0f, height, 0.0f }, { 0.0f, 0.0f, depth }, ROOM_SURF_WALL },
        { { p->halfWidth, p->floorY, p->screenZ },
          { 0.0f, 0.0f, depth }, { 0.0f, height, 0.0f }, ROOM_SURF_WALL },
        { { -p->halfWidth, p->floorY, p->screenZ },
          { 0.0f, 0.0f, depth }, { width, 0.0f, 0.0f }, ROOM_SURF_FLOOR },
        { { -p->halfWidth, p->ceilingY, p->screenZ },
          { width, 0.0f, 0.0f }, { 0.0f, 0.0f, depth }, ROOM_SURF_CEILING },
    };

    int written = 0;
    int used = 0;
    for (int f = 0; f < ROOM_FACES; f++) {
        Vec3 normal = vecNorm(vecCross(faces[f].edgeU, faces[f].edgeV));
        int base = written;

        for (int j = 0; j <= n; j++) {
            for (int i = 0; i <= n; i++) {
                float u = (float)i / (float)n;
                float v = (float)j / (float)n;
                Vec3 pos = {
                    faces[f].origin.x + faces[f].edgeU.x * u + faces[f].edgeV.x * v,
                    faces[f].origin.y + faces[f].edgeU.y * u + faces[f].edgeV.y * v,
                    faces[f].origin.z + faces[f].edgeU.z * u + faces[f].edgeV.z * v,
                };

                float rgb[3];
                roomVertexColor(p, faces[f].surface, pos, rgb);
                roomWriteVertex(p, verts, written, pos, rgb,
                                roomSpillWeight(p, pos, normal), 0.0f, 0.0f);
                written++;
            }
        }

        for (int j = 0; j < n; j++) {
            for (int i = 0; i < n; i++) {
                unsigned short a = (unsigned short)(base + j * (n + 1) + i);
                unsigned short b = (unsigned short)(a + 1);
                unsigned short c = (unsigned short)(a + n + 1);
                unsigned short d = (unsigned short)(c + 1);
                indices[used++] = a;
                indices[used++] = b;
                indices[used++] = d;
                indices[used++] = a;
                indices[used++] = d;
                indices[used++] = c;
            }
        }
    }

    *vertexCount = written;
    *indexCount = used;
    return 1;
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
// right, 2 bottom left, 3 bottom right, and are skipped where they are not
// drawn so the ray falls through to what is behind them.
static int hoverTest(float u, float v, float width, float height, int cornersLive,
                     int* corner) {
    if (cornersLive) {
        // Centred on the corner, reaching as far outside the picture as
        // inside, because that is where the bracket is drawn
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

    info.width = COG_TEX_W;
    info.height = COG_TEX_H;
    for (int tab = 0; tab < COG_ART_COUNT; tab++) {
        if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->cogPanelSwapchains[tab]),
                    "create cog panel swapchain")) {
            xrEnumerateSwapchainImages(ctx->cogPanelSwapchains[tab], 0,
                                       &ctx->cogPanelImageCounts[tab], NULL);
            ctx->cogPanelImages[tab] = calloc(ctx->cogPanelImageCounts[tab],
                                              sizeof(XrSwapchainImageOpenGLESKHR));
            for (uint32_t i = 0; i < ctx->cogPanelImageCounts[tab]; i++) {
                ctx->cogPanelImages[tab][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
            }
            xrEnumerateSwapchainImages(ctx->cogPanelSwapchains[tab],
                                       ctx->cogPanelImageCounts[tab],
                                       &ctx->cogPanelImageCounts[tab],
                                       (XrSwapchainImageBaseHeader*)ctx->cogPanelImages[tab]);
        }
        else {
            ctx->cogPanelSwapchains[tab] = XR_NULL_HANDLE;
        }
    }

    info.width = COG_THUMB_TEX;
    info.height = COG_THUMB_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->cogThumbSwapchain),
                "create cog thumb swapchain")) {
        xrEnumerateSwapchainImages(ctx->cogThumbSwapchain, 0, &ctx->cogThumbImageCount, NULL);
        ctx->cogThumbImages = calloc(ctx->cogThumbImageCount,
                                     sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->cogThumbImageCount; i++) {
            ctx->cogThumbImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->cogThumbSwapchain, ctx->cogThumbImageCount,
                                   &ctx->cogThumbImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->cogThumbImages);
    }
    else {
        ctx->cogThumbSwapchain = XR_NULL_HANDLE;
    }

    info.width = KB_TEX_W;
    info.height = KB_TEX_H;
    for (int state = 0; state < KB_STATE_COUNT; state++) {
        if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->kbPanelSwapchains[state]),
                    "create keyboard swapchain")) {
            xrEnumerateSwapchainImages(ctx->kbPanelSwapchains[state], 0,
                                       &ctx->kbPanelImageCounts[state], NULL);
            ctx->kbPanelImages[state] = calloc(ctx->kbPanelImageCounts[state],
                                               sizeof(XrSwapchainImageOpenGLESKHR));
            for (uint32_t i = 0; i < ctx->kbPanelImageCounts[state]; i++) {
                ctx->kbPanelImages[state][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
            }
            xrEnumerateSwapchainImages(ctx->kbPanelSwapchains[state],
                                       ctx->kbPanelImageCounts[state],
                                       &ctx->kbPanelImageCounts[state],
                                       (XrSwapchainImageBaseHeader*)ctx->kbPanelImages[state]);
        }
        else {
            ctx->kbPanelSwapchains[state] = XR_NULL_HANDLE;
        }
    }

    info.width = OUTLINE_TEX;
    info.height = OUTLINE_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->kbButtonSwapchain),
                "create keyboard button swapchain")) {
        xrEnumerateSwapchainImages(ctx->kbButtonSwapchain, 0, &ctx->kbButtonImageCount, NULL);
        ctx->kbButtonImages = calloc(ctx->kbButtonImageCount,
                                     sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->kbButtonImageCount; i++) {
            ctx->kbButtonImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->kbButtonSwapchain, ctx->kbButtonImageCount,
                                   &ctx->kbButtonImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->kbButtonImages);
    }
    else {
        ctx->kbButtonSwapchain = XR_NULL_HANDLE;
    }

    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->cogButtonSwapchain),
                "create cog button swapchain")) {
        xrEnumerateSwapchainImages(ctx->cogButtonSwapchain, 0, &ctx->cogButtonImageCount, NULL);
        ctx->cogButtonImages = calloc(ctx->cogButtonImageCount,
                                      sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->cogButtonImageCount; i++) {
            ctx->cogButtonImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->cogButtonSwapchain, ctx->cogButtonImageCount,
                                   &ctx->cogButtonImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->cogButtonImages);
    }
    else {
        ctx->cogButtonSwapchain = XR_NULL_HANDLE;
    }

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

    // The one chain here that is redrawn every frame rather than filled once,
    // since it is made out of whatever the picture is showing
    info.width = GLOW_TEX;
    info.height = GLOW_TEX;
    if (checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->glowSwapchain),
                "create glow swapchain")) {
        xrEnumerateSwapchainImages(ctx->glowSwapchain, 0, &ctx->glowImageCount, NULL);
        ctx->glowImages = calloc(ctx->glowImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t i = 0; i < ctx->glowImageCount; i++) {
            ctx->glowImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        xrEnumerateSwapchainImages(ctx->glowSwapchain, ctx->glowImageCount, &ctx->glowImageCount,
                                   (XrSwapchainImageBaseHeader*)ctx->glowImages);
    }
    else {
        ctx->glowSwapchain = XR_NULL_HANDLE;
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

// The curve in force. The panel takes over from the preference the moment it
// is touched, and hands it back when the reset button clears it.
static float effectiveCurvature(XrCtx* ctx) {
    return ctx->panelCurve >= 0.0f ? ctx->panelCurve : ctx->prefCurvature;
}

// Which room is in force, the picker's unless the debug property has taken it
// over. 0 is no room at all, which is every other environment.
static int roomEffective(XrCtx* ctx) {
    return ctx->roomOverride >= 0 ? ctx->roomOverride : ctx->roomStyle;
}

// A room places and sizes its own picture, so every row on the screen tab is
// dead while one is on. The panel shows a note in their place, and the input
// side has to agree with what is drawn.
static int cogScreenLocked(XrCtx* ctx) {
    return ctx->cogTab == COG_TAB_SCREEN && roomEffective(ctx) > 0;
}

// The sliders place the screen, the grab moves it from there. Moving either
// slider is taken as the user asking for the placement back. Says whether it
// reseeded, since a room holding the screen has to know the placement waiting
// behind it has changed.
static int updatePlacement(XrCtx* ctx, float distance, float quadWidth, float curvature) {
    int sliderMoved = ctx->sliderSeen
            && (fabsf(distance - ctx->lastDistance) > 1e-4f
                || fabsf(quadWidth - ctx->lastQuadWidth) > 1e-4f);
    int reseeded = !ctx->placementValid || sliderMoved;

    if (reseeded) {
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
    return reseeded;
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
    out[IN_POSE + 9] = ctx->panelCurve;
}

// How far the screen's face is tipped up or down, in radians. Positive is
// looking up at it.
static float screenPitch(XrCtx* ctx) {
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
    return atan2f(fwd.y, sqrtf(fwd.x * fwd.x + fwd.z * fwd.z));
}

// The three angles the screen is described by, built back into an orientation:
// yaw about world up, then pitch, then roll about the screen's own forward
// axis. Roll goes innermost on purpose. A turn about the forward axis leaves
// that axis where it is, so the pitch and yaw read back untouched however far
// the picture is rolled, which is what lets a drag hold one while recomputing
// the other. Pitch is negated for the same reason the tilt slider negates it:
// turning by +theta about local x takes the face downward.
static XrQuaternionf screenOrient(float yaw, float pitch, float roll) {
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    Vec3 fwd = { 0.0f, 0.0f, 1.0f };
    XrQuaternionf q = quatMul(axisAngleQuat(up, yaw), axisAngleQuat(right, -pitch));
    return quatNorm(quatMul(q, axisAngleQuat(fwd, roll)));
}

// How far the screen is twisted about the axis it faces along, in radians.
// Positive raises its right edge. Measured by undoing the yaw and pitch rather
// than by reading how high the right edge sits, since those two only agree
// while the screen is level, and this one comes back out of screenOrient
// exactly at any pitch.
static float screenRoll(XrCtx* ctx) {
    XrQuaternionf q = ctx->screenPose.orientation;
    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 fwd = quatRotate(q, back);
    float yaw = atan2f(fwd.x, fwd.z);
    XrQuaternionf level = screenOrient(yaw, screenPitch(ctx), 0.0f);
    XrQuaternionf twist = quatMul(quatConj(level), q);
    // A quaternion and its negation are the same rotation, and with the screen
    // turned to face behind the viewer the rebuilt yaw lands on the other one.
    // Without this the answer comes back a full turn out.
    if (twist.w < 0.0f) {
        twist.z = -twist.z;
        twist.w = -twist.w;
    }
    return 2.0f * atan2f(twist.z, twist.w);
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
        // A 3d room holds the picture on its wall and forces the pose every
        // frame, so a drag could only fight it. Neither handle is drawn there,
        // and the corners are not even hovered.
        if (roomEffective(ctx) > 0) {
            return;
        }
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
            // Read once here rather than every frame of the drag. grabScreen
            // is the pose these come off, and it was just set from screenPose.
            // Re-extracting each frame would let the rounding walk the tilt
            // away over a long move.
            ctx->grabPitch = screenPitch(ctx);
            ctx->grabRoll = screenRoll(ctx);
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
        // Where it goes is still the rigid attach: the offset from the hand is
        // carried round by the full hand turn, so the screen swings with the
        // same leverage it always did rather than sliding flat.
        XrQuaternionf turn = quatMul(aims[h].orientation, quatConj(ctx->grabAim.orientation));
        Vec3 offset = { ctx->grabScreen.position.x - ctx->grabAim.position.x,
                        ctx->grabScreen.position.y - ctx->grabAim.position.y,
                        ctx->grabScreen.position.z - ctx->grabAim.position.z };
        Vec3 moved = quatRotate(turn, offset);

        ctx->screenPose.position.x = aims[h].position.x + moved.x;
        ctx->screenPose.position.y = aims[h].position.y + moved.y;
        ctx->screenPose.position.z = aims[h].position.z + moved.z;

        // Which way it faces does not. Inheriting the wrist tumbled the
        // picture on all three axes, so instead it keeps the tilt and roll it
        // was picked up with and turns to face the viewer from wherever it has
        // been dragged to.
        float hx = ctx->headPos.x - ctx->screenPose.position.x;
        float hz = ctx->headPos.z - ctx->screenPose.position.z;
        // Dragged directly over or under the head there is no sensible way to
        // face, and the yaw would spin on noise. Keep last frame's.
        if (hx * hx + hz * hz > 0.0025f) {
            ctx->screenPose.orientation = screenOrient(atan2f(hx, hz), ctx->grabPitch,
                                                       ctx->grabRoll);
        }
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

// The cog is the same button on the other side of the bar
static void cogButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float side = ctx->screenWidth * COG_BUTTON_FRAC;
    float barW = ctx->screenWidth * BAR_WIDTH_FRAC;
    float barH = ctx->screenWidth * BAR_HEIGHT_FRAC;
    outLocal->x = barW * 0.5f + ctx->screenWidth * ENV_GAP_FRAC + side * 0.5f;
    outLocal->y = -(height * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

static int cogButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    cogButtonPlacement(ctx, height, &local, &side);

    float cu = 0.5f + local.x / ctx->screenWidth;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / ctx->screenWidth;
    float halfV = side * HOVER_MARGIN * 0.5f / height;
    return fabsf(u - cu) < halfU && fabsf(v - cv) < halfV;
}

// The settings panel stands on top of the cog button that opens it, so it
// reads as belonging to that button and leaves the picture clear. The caller
// freezes what this returns for as long as the panel is open: the distance
// slider moves the screen, and a panel that followed it would drag the thumb
// out from under the ray halfway through a drag.
static XrPosef cogPanelPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = ctx->screenWidth * COG_WIDTH_FRAC;
    float height = width * (float)COG_TEX_H / (float)COG_TEX_W;
    *outWidth = width;
    *outHeight = height;

    // The button hangs below the screen, so the panel is placed off it rather
    // than off the screen. Same height the other placements are given.
    float screenHeight = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    Vec3 button;
    float side;
    cogButtonPlacement(ctx, screenHeight, &button, &side);

    Vec3 local;
    local.x = button.x;
    local.y = button.y + side * 0.5f + ctx->screenWidth * ENV_GAP_FRAC + height * 0.5f;
    local.z = 0.05f;

    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose = ctx->screenPose;
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// The keyboard button is the same button again, one place further out along
// the bar than the cog
static void kbButtonPlacement(XrCtx* ctx, float height, Vec3* outLocal, float* outSide) {
    float side = ctx->screenWidth * COG_BUTTON_FRAC;
    float barW = ctx->screenWidth * BAR_WIDTH_FRAC;
    float barH = ctx->screenWidth * BAR_HEIGHT_FRAC;
    float gap = ctx->screenWidth * ENV_GAP_FRAC;
    outLocal->x = barW * 0.5f + gap + side * 1.5f + gap;
    outLocal->y = -(height * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + barH * 0.5f);
    outLocal->z = 0.005f;
    *outSide = side;
}

static int kbButtonHit(XrCtx* ctx, float u, float v, float height) {
    Vec3 local;
    float side;
    kbButtonPlacement(ctx, height, &local, &side);

    float cu = 0.5f + local.x / ctx->screenWidth;
    float cv = 0.5f - local.y / height;
    float halfU = side * HOVER_MARGIN * 0.5f / ctx->screenWidth;
    float halfV = side * HOVER_MARGIN * 0.5f / height;
    return fabsf(u - cu) < halfU && fabsf(v - cv) < halfV;
}

// The keyboard hangs under the screen, centred on it, in the band the move bar
// lives in. Wider than the settings panel and squarer, so it wants the middle
// rather than a corner. Frozen while it is open, like the settings panel: the
// screen stays draggable behind it and the keys must not move under the ray.
static XrPosef kbPanelPose(XrCtx* ctx, float* outWidth, float* outHeight) {
    float width = ctx->screenWidth * KB_WIDTH_FRAC;
    float height = width * (float)KB_TEX_H / (float)KB_TEX_W;
    *outWidth = width;
    *outHeight = height;

    float screenHeight = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    Vec3 local;
    local.x = 0.0f;
    // Top edge the same distance under the picture that the bar sits at
    local.y = -(screenHeight * 0.5f + ctx->screenWidth * BAR_GAP_FRAC + height * 0.5f);
    local.z = 0.05f;

    Vec3 offset = quatRotate(ctx->screenPose.orientation, local);
    XrPosef pose = ctx->screenPose;
    pose.position.x += offset.x;
    pose.position.y += offset.y;
    pose.position.z += offset.z;
    return pose;
}

// Which key a point on the panel is inside, or -1. The rectangles are the
// whole of what this side knows about the layout, so a row of them is all
// there is to search.
static int kbKeyAt(XrCtx* ctx, float u, float v) {
    int found = -1;
    for (int i = 0; i < ctx->kbKeyCount; i++) {
        const float* r = &ctx->kbKeyRects[i * 4];
        if (u >= r[0] && u <= r[2] && v >= r[1] && v <= r[3]) {
            found = i;
        }
    }
    return found;
}

// How many rows a tab has, whatever kind they are
static int cogTabRowCount(int tab) {
    if (tab == COG_TAB_SCREEN) {
        return COG_SLIDER_COUNT;
    }
    if (tab == COG_TAB_3D) {
        return COG_ROW3D_COUNT;
    }
    // The option rows, then the glow level track under them
    return COG_DISPLAY_SLIDER_ROW + 1;
}

// Where a slider's thumb sits along its track, 0 at the left end and 1 at the
// right. Read back from the thing the slider controls rather than stored, so
// dragging the screen about cannot leave the panel disagreeing with it.
static float cogSliderValue(XrCtx* ctx, int tab, int slider) {
    XrVector3f p = ctx->screenPose.position;
    float t = 0.0f;

    if (tab == COG_TAB_DISPLAY) {
        // Only one row on this tab has a thumb, so which one it is does not
        // need asking
        t = ctx->ambiIntensity;
    }
    else if (tab == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            t = ctx->separationCurrent / COG_SEP_MAX;
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            t = ctx->convergence;
        }
    }
    else if (slider == COG_SLIDER_DISTANCE) {
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        t = (d - COG_DIST_MIN) / (COG_DIST_MAX - COG_DIST_MIN);
    }
    else if (slider == COG_SLIDER_HEIGHT) {
        t = (p.y - COG_HEIGHT_MIN) / (COG_HEIGHT_MAX - COG_HEIGHT_MIN);
    }
    else if (slider == COG_SLIDER_TILT) {
        // Level sits at the middle of the track
        t = (screenPitch(ctx) + COG_TILT_MAX) / (2.0f * COG_TILT_MAX);
    }
    else if (slider == COG_SLIDER_ROTATE) {
        // Reversed against the others: the right hand end turns the picture
        // clockwise, which lowers the right edge that screenRoll counts as
        // positive. Level sits at the middle either way.
        t = (COG_ROLL_MAX - screenRoll(ctx)) / (2.0f * COG_ROLL_MAX);
    }
    else if (slider == COG_SLIDER_CURVE) {
        t = effectiveCurvature(ctx);
    }
    else if (slider == COG_SLIDER_SIZE) {
        t = (ctx->screenWidth - SCREEN_MIN_WIDTH) / (SCREEN_MAX_WIDTH - SCREEN_MIN_WIDTH);
    }

    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Applies a point on the track to whatever the row controls
static void cogApplySlider(XrCtx* ctx, int tab, int slider, float pu) {
    float t = (pu - COG_TRACK_L) / (COG_TRACK_R - COG_TRACK_L);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    if (tab == COG_TAB_DISPLAY) {
        // Five percent steps, so the thumb shows exactly what the preference
        // will be written with when the drag ends
        int units = (int)roundf(t * 20.0f) * 5;
        ctx->ambiIntensity = units / 100.0f;
        return;
    }

    if (tab == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            // Snapped to the units the preference is stored in, so what the
            // thumb shows is exactly what gets written when the drag ends
            int units = (int)roundf(t * COG_SEP_STEPS);
            ctx->panelSeparation = units * 0.001f;
            ctx->separationCurrent = ctx->panelSeparation;
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            // Whole percent, same reason
            int units = (int)roundf(t * 100.0f);
            ctx->convergence = units / 100.0f;
        }
        return;
    }

    if (slider == COG_SLIDER_DISTANCE) {
        XrVector3f p = ctx->screenPose.position;
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        float wanted = COG_DIST_MIN + t * (COG_DIST_MAX - COG_DIST_MIN);
        if (d > 0.01f) {
            // Straight out along the line it already sits on, so only how far
            // away it is changes
            float scale = wanted / d;
            ctx->screenPose.position.x = p.x * scale;
            ctx->screenPose.position.y = p.y * scale;
            ctx->screenPose.position.z = p.z * scale;
            // Keeping the arc the same shape as it moves, same reason as the
            // resize path
            ctx->screenRadius *= scale;
        }
        else {
            // Sitting on top of the viewer, so there is no line to follow and
            // straight ahead is the only sensible answer
            ctx->screenPose.position.x = 0.0f;
            ctx->screenPose.position.y = 0.0f;
            ctx->screenPose.position.z = -wanted;
        }
    }
    else if (slider == COG_SLIDER_HEIGHT) {
        // Straight up and down, so raising the screen does not also bring it
        // nearer the way an arc about the viewer would
        ctx->screenPose.position.y = COG_HEIGHT_MIN + t * (COG_HEIGHT_MAX - COG_HEIGHT_MIN);
    }
    else if (slider == COG_SLIDER_TILT) {
        float target = -COG_TILT_MAX + t * (2.0f * COG_TILT_MAX);
        // Level is worth being able to land on exactly, same as the roll row
        if (fabsf(target) < COG_TILT_SNAP) {
            target = 0.0f;
        }
        // Rebuilt from the three angles rather than turned about the local x
        // axis, which stops being horizontal once the picture has been rolled
        // and would swing it round instead of tipping it. Identical result on
        // a level screen, and it lands on the target in one step.
        Vec3 back = { 0.0f, 0.0f, 1.0f };
        Vec3 fwd = quatRotate(ctx->screenPose.orientation, back);
        float yaw = atan2f(fwd.x, fwd.z);
        float roll = screenRoll(ctx);
        // Position untouched, so it tilts about its own centre rather than
        // swinging around the viewer
        ctx->screenPose.orientation = screenOrient(yaw, target, roll);
    }
    else if (slider == COG_SLIDER_ROTATE) {
        // Dragging right turns the picture clockwise as the viewer sees it,
        // the way a rotate right button does. The forward axis points out at
        // the viewer, so a right handed turn about it reads anticlockwise, and
        // the track runs from +max down to -max to match.
        float target = COG_ROLL_MAX - t * (2.0f * COG_ROLL_MAX);
        // Level is the whole point of the row and the track is far too coarse
        // to land on it by hand, so the middle of it clips to exactly zero
        if (fabsf(target) < COG_ROLL_SNAP) {
            target = 0.0f;
        }
        float delta = target - screenRoll(ctx);
        Vec3 fwd = { 0.0f, 0.0f, 1.0f };
        Vec3 axis = quatRotate(ctx->screenPose.orientation, fwd);
        XrQuaternionf turn = axisAngleQuat(axis, delta);
        // Turning about the axis it already faces along leaves the facing
        // alone, so this only rolls: the tilt and the yaw come back unchanged
        ctx->screenPose.orientation = quatNorm(quatMul(turn, ctx->screenPose.orientation));
    }
    else if (slider == COG_SLIDER_CURVE) {
        ctx->panelCurve = t;
        // The radius updatePlacement would have picked for this curve, so a
        // reseed later agrees with what is on screen now
        XrVector3f p = ctx->screenPose.position;
        float d = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        ctx->screenRadius = d * (1.0f + 3.0f * (1.0f - t));
    }
    else if (slider == COG_SLIDER_SIZE) {
        float wanted = SCREEN_MIN_WIDTH + t * (SCREEN_MAX_WIDTH - SCREEN_MIN_WIDTH);
        if (ctx->screenWidth > 0.01f) {
            // Keeping the arc the same shape rather than flattening as it
            // grows, same rule the corner resize follows
            ctx->screenRadius *= wanted / ctx->screenWidth;
        }
        // Centre and facing untouched, so it grows about the middle rather
        // than away from a corner
        ctx->screenWidth = wanted;
    }
}

// The option rows on the display tab are a row of cells rather than a track
static int cogOptionCells(int option) {
    if (option == COG_OPTION_SHARPEN) {
        return COG_SHARPEN_CELLS;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        return COG_AMBI_CELLS;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        return COG_ROOM_LIGHT_CELLS;
    }
    return COG_STATS_CELLS;
}

// Which cell of a row is the one in force
static int cogOptionValue(XrCtx* ctx, int option) {
    if (option == COG_OPTION_SHARPEN) {
        return ctx->sharpenMode;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        return ctx->ambilightOn ? 1 : 0;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        return ctx->roomLightOn ? 1 : 0;
    }
    return ctx->overlayVisible ? 1 : 0;
}

// Takes effect here and now, and hands back the setting id so the frame can
// tell Java to store it too. Both of these are read fresh every frame by the
// code that acts on them, so there is nothing to restart.
static int cogApplyOption(XrCtx* ctx, int option, int cell) {
    if (option == COG_OPTION_SHARPEN) {
        ctx->sharpenMode = cell;
        return SETTING_SHARPEN;
    }
    if (option == COG_OPTION_STATS) {
        ctx->overlayVisible = cell != 0;
        return SETTING_STATS;
    }
    if (option == COG_OPTION_AMBILIGHT) {
        ctx->ambilightOn = cell != 0;
        LOGEV("ambilight %s from the panel", ctx->ambilightOn ? "on" : "off");
        return SETTING_AMBILIGHT;
    }
    if (option == COG_OPTION_ROOM_LIGHT) {
        ctx->roomLightOn = cell != 0;
        LOGEV("room light %s from the panel", ctx->roomLightOn ? "on" : "off");
        return SETTING_ROOM_LIGHT;
    }
    return -1;
}

// Letting go of a slider, either on purpose or because focus went away mid
// drag. Persisting where it ended up rather than every frame on the way there
// is the same policy a grab uses, so this is where the writing happens.
static void cogDragEnded(XrCtx* ctx, float* out) {
    int slider = ctx->cogDragSlider;
    int tab = ctx->cogTab;
    ctx->cogDragSlider = -1;
    ctx->cogDragHand = -1;

    if (slider < 0) {
        return;
    }
    if (tab == COG_TAB_SCREEN) {
        // The placement is saved from the pose the frame hands back
        ctx->poseDirty = 1;
    }
    else if (tab == COG_TAB_3D) {
        if (slider == COG_ROW3D_SEPARATION) {
            out[IN_SETTING] = (float)SETTING_SEPARATION;
            // Tenths of a percent of frame width, the preference's units
            out[IN_SETTING_VALUE] = roundf(ctx->separationCurrent * 1000.0f);
        }
        else if (slider == COG_ROW3D_CONVERGENCE) {
            out[IN_SETTING] = (float)SETTING_CONVERGENCE;
            out[IN_SETTING_VALUE] = roundf(ctx->convergence * 100.0f);
        }
    }
    else if (tab == COG_TAB_DISPLAY) {
        out[IN_SETTING] = (float)SETTING_AMBI_LEVEL;
        // Whole percent, the preference's units
        out[IN_SETTING_VALUE] = roundf(ctx->ambiIntensity * 100.0f);
    }
}

// Which cell of a row the ray is on, or -1 off the ends
static int cogCellAt(float pu, int cells) {
    if (pu < COG_TRACK_L || pu > COG_TRACK_R) {
        return -1;
    }
    int cell = (int)((pu - COG_TRACK_L) / (COG_TRACK_R - COG_TRACK_L) * cells);
    if (cell < 0) cell = 0;
    if (cell >= cells) cell = cells - 1;
    return cell;
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

// The press was meant for the thing that is open, not for the host behind it.
// Gaze has no button of its own, so swallowing a gaze press means swallowing
// the pinch that stood in for it.
static void swallowTrigger(XrCtx* ctx, int src) {
    if (src < 0 || src >= SRC_COUNT) {
        return;
    }
    ctx->triggerSwallowed[src] = 1;
    if (src == SRC_GAZE) {
        for (int h = 0; h < HAND_COUNT; h++) {
            if (ctx->triggerDown[h]) {
                ctx->triggerSwallowed[h] = 1;
            }
        }
    }
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
    if (hover == HOVER_COGPANEL) {
        return screenPoint(u, v, ctx->cogPose, ctx->cogW, ctx->cogH, 0.0f, 0);
    }
    if (hover == HOVER_KBPANEL) {
        return screenPoint(u, v, ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0);
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

    // Destroying the context below would take these anyway. Said explicitly so
    // the glow's resources go together with the swapchain it draws into.
    glDeleteFramebuffers(1, &ctx->ambiFbo);
    glDeleteFramebuffers(1, &ctx->glowFbo);
    glDeleteTextures(1, &ctx->ambiTexture);
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
    for (int tab = 0; tab < COG_ART_COUNT; tab++) {
        if (ctx->cogPanelSwapchains[tab] != XR_NULL_HANDLE) {
            xrDestroySwapchain(ctx->cogPanelSwapchains[tab]);
        }
        free(ctx->cogPanelImages[tab]);
    }
    if (ctx->cogButtonSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->cogButtonSwapchain);
    }
    free(ctx->cogButtonImages);
    if (ctx->cogThumbSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->cogThumbSwapchain);
    }
    free(ctx->cogThumbImages);
    for (int state = 0; state < KB_STATE_COUNT; state++) {
        if (ctx->kbPanelSwapchains[state] != XR_NULL_HANDLE) {
            xrDestroySwapchain(ctx->kbPanelSwapchains[state]);
        }
        free(ctx->kbPanelImages[state]);
    }
    if (ctx->kbButtonSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->kbButtonSwapchain);
    }
    free(ctx->kbButtonImages);
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
    if (ctx->glowSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->glowSwapchain);
    }
    free(ctx->glowImages);
    if (ctx->roomSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(ctx->roomSwapchain);
    }
    free(ctx->roomImages);
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
                                                       jboolean handTracking, jint sharpenMode,
                                                       jboolean perfOverlay, jboolean ambilight,
                                                       jint ambiLevel, jboolean roomLight,
                                                       jboolean gen1Headset) {
    XrCtx* ctx = calloc(1, sizeof(XrCtx));
    ctx->handsEnabled = handTracking;
    ctx->gen1Headset = gen1Headset;
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

    LOGI("OpenXR init complete (cylinder=%d equirect=%d srgbWriteControl=%d maxLayers=%d)",
         ctx->cylinderSupported, ctx->equirectSupported, ctx->srgbWriteControl,
         ctx->maxLayerCount);
    return (jlong)(intptr_t)ctx;
}

// Points the native log lines at the file the Java side opened. Called before
// init, so there is no context to hang it off yet.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeSetFileLog(JNIEnv* env, jclass clazz,
                                                             jstring path, jint level) {
    const char* chars = NULL;
    if (path != NULL && level > FILE_LOG_OFF) {
        chars = (*env)->GetStringUTFChars(env, path, NULL);
    }

    pthread_mutex_lock(&logMutex);
    // Whatever is cached was opened against the old path, so it goes first
    if (logFd >= 0) {
        close(logFd);
        logFd = -1;
    }
    logIno = 0;

    if (chars == NULL) {
        fileLogLevel = FILE_LOG_OFF;
        fileLogPath[0] = '\0';
    }
    else {
        strncpy(fileLogPath, chars, sizeof(fileLogPath) - 1);
        fileLogPath[sizeof(fileLogPath) - 1] = '\0';
        fileLogLevel = level;
    }
    pthread_mutex_unlock(&logMutex);

    if (chars != NULL) {
        (*env)->ReleaseStringUTFChars(env, path, chars);
    }
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
        ctx->prefCurvature = curvature;
    }
    // Zero is a real cell, so "nothing picked" has to be said explicitly. Every
    // early return below would otherwise read as a press on the first one. Same
    // for the setting id, and nothing clears it again once a row has written
    // one, so the early returns carry it out too.
    out[IN_PICKER_PICK] = -1.0f;
    out[IN_SETTING] = -1.0f;
    // Backspace is 8, so a zeroed slot would type one every frame
    out[IN_KEY] = -1.0f;

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
            if (ctx->cogDragSlider >= 0) {
                // Same for a slider: a drag must not survive the trigger it
                // was being held with going unwatched. Ending it here still
                // writes the value, since out is flushed on the way out.
                cogDragEnded(ctx, out);
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

    // Both of these have to agree with what endFrame submits, or the ray lands
    // somewhere other than where the picture is drawn. A room world locks it
    // and flattens it whatever the preference and the panel say.
    int roomOn = roomEffective(ctx) > 0;
    XrSpace space = (headLocked && !roomOn) ? ctx->viewSpace : ctx->localSpace;
    float height = ctx->screenWidth * (float)ctx->videoHeight / (float)ctx->videoWidth;
    int curved = !roomOn && effectiveCurvature(ctx) > 0.01f && ctx->cylinderSupported;
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
                // Filtering across a tracking gap would sweep the ray in
                // from wherever the hand was last seen
                if (h < HAND_COUNT) {
                    ctx->aimFilterPos[h][0].valid = 0;
                    ctx->aimFilterPos[h][1].valid = 0;
                    ctx->aimFilterPos[h][2].valid = 0;
                    ctx->aimFilterRot[h].valid = 0;
                }
                continue;
            }
        }
        // Hands and controllers go through the pose filter. Gaze does not:
        // eyes move in saccades and the cursor is already smoothed downstream.
        if (h < HAND_COUNT) {
            XrPosef f = loc.pose;
            f.position.x = euroFilter(&ctx->aimFilterPos[h][0], f.position.x, dt,
                                      ctx->aimMinCutoff, ctx->aimBeta);
            f.position.y = euroFilter(&ctx->aimFilterPos[h][1], f.position.y, dt,
                                      ctx->aimMinCutoff, ctx->aimBeta);
            f.position.z = euroFilter(&ctx->aimFilterPos[h][2], f.position.z, dt,
                                      ctx->aimMinCutoff, ctx->aimBeta);
            f.orientation = euroFilterQuat(&ctx->aimFilterRot[h], f.orientation, dt,
                                           ctx->aimMinCutoff, ctx->aimBeta);
            aimPoses[h] = f;
        }
        else {
            aimPoses[h] = loc.pose;
        }
        aimValid[h] = 1;
        // The keyboard is not modal, but it does own the ground it covers: it
        // hangs in front of the bar, so a ray that lands on it must not reach
        // the picture or the furniture behind.
        float kbU, kbV;
        int onKeyboard = ctx->kbOpen
                && screenProject(aimPoses[h], ctx->kbPose, ctx->kbW, ctx->kbH, 0.0f, 0,
                                 &kbU, &kbV)
                && kbU >= 0.0f && kbU <= 1.0f && kbV >= 0.0f && kbV <= 1.0f;
        if (onKeyboard) {
            hovers[h] = HOVER_KBPANEL;
            hitU[h] = kbU;
            hitV[h] = kbV;
        }
        else if (screenProject(aimPoses[h], screenPose, ctx->screenWidth, height, radius, curved,
                               &hitU[h], &hitV[h])) {
            // No corner brackets in a room, so nothing there claims the ray
            hovers[h] = hoverTest(hitU[h], hitV[h], ctx->screenWidth, height, !roomOn,
                                  &corners[h]);
            // The button reaches past the left end of the bar's zone, so it is
            // tested here rather than after a hand has been picked. Otherwise
            // the part of it outside that zone belongs to no hand at all.
            if ((hovers[h] == HOVER_NONE || hovers[h] == HOVER_BAR)
                    && envButtonHit(ctx, hitU[h], hitV[h], height)) {
                hovers[h] = HOVER_ENVBUTTON;
            }
            // The cog is the same button on the other side of the bar, so it
            // is claimed the same way
            if ((hovers[h] == HOVER_NONE || hovers[h] == HOVER_BAR)
                    && cogButtonHit(ctx, hitU[h], hitV[h], height)) {
                hovers[h] = HOVER_COGBUTTON;
            }
            // And the keyboard is one further out again, far enough out that
            // it sits past the right end of the bar's zone entirely. That is
            // halo ground, so like the padlock on the left it has to claim the
            // halo back or the ray never reaches it.
            if ((hovers[h] == HOVER_NONE || hovers[h] == HOVER_BAR
                    || hovers[h] == HOVER_HALO)
                    && kbButtonHit(ctx, hitU[h], hitV[h], height)) {
                hovers[h] = HOVER_KBBUTTON;
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

    // A press taken by the grid, the panel or the keyboard stays taken for as
    // long as it is held, and letting go is what hands the trigger back
    for (int h = 0; h < SRC_COUNT; h++) {
        if (!ctx->triggerDown[h]) {
            ctx->triggerSwallowed[h] = 0;
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
    ctx->cogButtonHot = 0;
    ctx->cogHoverSlider = -1;
    ctx->cogHoverCell = -1;
    ctx->lockHot = 0;
    ctx->pickerPick = -1;
    ctx->kbButtonHot = 0;
    ctx->kbHoverKey = -1;
    ctx->kbKeyDown = 0;
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
            // Each band is a header strip over a row of cells. The strip is a
            // label and nothing else, so pointing at one is pointing at
            // nothing and a press there closes the grid like a press outside.
            int band = (int)(pv * PICKER_ROWS);
            if (band >= PICKER_ROWS) band = PICKER_ROWS - 1;
            float inBand = pv * PICKER_TEX_H - band * PICKER_BAND_PX;
            if (inBand < PICKER_HEADER_PX) {
                continue;
            }
            int col = (int)(pu * PICKER_COLS);
            if (col >= PICKER_COLS) col = PICKER_COLS - 1;
            ctx->pickerHover = band * PICKER_COLS + col;
            hand = h;
            hitU[h] = pu;
            hitV[h] = pv;

            if (ctx->triggerEdge[h]) {
                ctx->pickerPick = ctx->pickerHover;
                ctx->pickerChoice = ctx->pickerHover;
                ctx->pickerOpen = 0;
                swallowTrigger(ctx, h);
            }
            break;
        }

        // A press that lands on no cell, whether on a header strip or nowhere
        // near the grid at all, closes it
        if (ctx->pickerOpen && ctx->pickerHover < 0) {
            for (int h = 0; h < SRC_COUNT; h++) {
                if (ctx->triggerEdge[h]) {
                    ctx->pickerOpen = 0;
                    swallowTrigger(ctx, h);
                }
            }
        }
    }
    else if (ctx->cogOpen) {
        // Modal in the same way the grid is, and against the pose frozen when
        // it opened rather than wherever the screen has since been dragged to
        hover = HOVER_COGPANEL;
        hand = -1;

        // A drag keeps the hand that started it, and keeps it even once the
        // ray has wandered off the panel, so a slider can be run to either end
        // in one go. The display tab is cells apart from its one level row.
        if (ctx->cogDragSlider >= 0 && cogScreenLocked(ctx)) {
            // A room took the picture mid drag, which only a debug property
            // can do, and there is nothing left under the thumb to move
            cogDragEnded(ctx, out);
        }
        else if (ctx->cogDragSlider >= 0
                && (ctx->cogTab != COG_TAB_DISPLAY
                    || ctx->cogDragSlider == COG_DISPLAY_SLIDER_ROW)) {
            int h = ctx->cogDragHand;
            float pu, pv;
            if (h >= 0 && aimValid[h] && ctx->triggerDown[h]
                    && screenProject(aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                                     0.0f, 0, &pu, &pv)) {
                hand = h;
                hitU[h] = pu;
                hitV[h] = pv;
                ctx->cogHoverSlider = ctx->cogDragSlider;
                cogApplySlider(ctx, ctx->cogTab, ctx->cogDragSlider, pu);
            }
            else {
                cogDragEnded(ctx, out);
            }
        }

        for (int h = 0; hand < 0 && h < SRC_COUNT; h++) {
            float pu, pv;
            if (!aimValid[h] || !ctx->pointerAwake) {
                continue;
            }
            if (!screenProject(aimPoses[h], ctx->cogPose, ctx->cogW, ctx->cogH,
                               0.0f, 0, &pu, &pv)) {
                continue;
            }
            if (pu < 0.0f || pu > 1.0f || pv < 0.0f || pv > 1.0f) {
                continue;
            }
            hand = h;
            hitU[h] = pu;
            hitV[h] = pv;

            // The tab bar runs across the top, one even slot per tab. A press
            // up here changes tab and reaches nothing else.
            if (pv < COG_TAB_BAR_B) {
                if (ctx->triggerEdge[h]) {
                    int t = (int)(pu * COG_TAB_COUNT);
                    if (t >= COG_TAB_COUNT) t = COG_TAB_COUNT - 1;
                    ctx->cogTab = t;
                    ctx->cogDragSlider = -1;
                    ctx->cogDragHand = -1;
                }
                break;
            }

            // Below the tabs the screen tab is a note while a room is on, so
            // rows, tracks and the reset button are all out of reach. The
            // press is still swallowed, since it landed on the panel.
            if (cogScreenLocked(ctx)) {
                ctx->cogHoverSlider = -1;
                ctx->cogHoverCell = -1;
                break;
            }

            int rowCount = cogTabRowCount(ctx->cogTab);
            int row = -1;
            for (int s = 0; s < rowCount; s++) {
                // Curving needs a layer type this runtime may not have, and
                // the row is drawn greyed to say so
                if (ctx->cogTab == COG_TAB_SCREEN && s == COG_SLIDER_CURVE
                        && !ctx->cylinderSupported) {
                    continue;
                }
                // With stereo off there is nothing for either 3D row to move,
                // and both are drawn greyed to match
                if (ctx->cogTab == COG_TAB_3D && ctx->stereoMode == DEPTH_MODE_OFF) {
                    continue;
                }
                if (fabsf(pv - (COG_ROW_V0 + s * COG_ROW_STEP)) < COG_ROW_HALF) {
                    row = s;
                    break;
                }
            }

            if (ctx->cogTab == COG_TAB_DISPLAY) {
                if (row == COG_DISPLAY_SLIDER_ROW) {
                    // The one track on this tab, handled the way the other
                    // tabs' rows are, including the band reaching a little
                    // past both ends for the thumb hanging over them
                    if (pu <= COG_TRACK_L - 0.04f || pu >= COG_TRACK_R + 0.04f) {
                        row = -1;
                    }
                    ctx->cogHoverSlider = row;
                    ctx->cogHoverCell = -1;
                    if (row >= 0 && ctx->triggerEdge[h]) {
                        ctx->cogDragSlider = row;
                        ctx->cogDragHand = h;
                        // Jumps to where the press landed, same as the others
                        cogApplySlider(ctx, ctx->cogTab, row, pu);
                    }
                    break;
                }

                // Cells, so a press picks one rather than starting a drag
                int cell = row >= 0 ? cogCellAt(pu, cogOptionCells(row)) : -1;
                ctx->cogHoverSlider = cell >= 0 ? row : -1;
                ctx->cogHoverCell = cell;
                if (cell >= 0 && ctx->triggerEdge[h]) {
                    int id = cogApplyOption(ctx, row, cell);
                    if (id >= 0) {
                        out[IN_SETTING] = (float)id;
                        out[IN_SETTING_VALUE] = (float)cell;
                    }
                }
                break;
            }

            // Sliders. The band reaches a little past both ends of the track,
            // since the thumb hangs over them.
            if (row >= 0 && (pu <= COG_TRACK_L - 0.04f || pu >= COG_TRACK_R + 0.04f)) {
                row = -1;
            }
            ctx->cogHoverSlider = row;

            int onReset = pu >= COG_RESET_L && pu <= COG_RESET_R
                    && pv >= COG_RESET_T && pv <= COG_RESET_B;
            if (onReset && ctx->triggerEdge[h] && ctx->cogTab == COG_TAB_3D) {
                // The shipped defaults, 0.5 percent and half convergence, said
                // here rather than read back so the button works the same way
                // whatever the preferences were left on. Still allowed while
                // stereo is off, where it does no harm and keeps the button
                // from being a dead rectangle.
                ctx->panelSeparation = 0.005f;
                ctx->separationCurrent = 0.005f;
                ctx->convergence = 0.5f;
                out[IN_SETTING] = (float)SETTING_RESET_3D;
                out[IN_SETTING_VALUE] = 0.0f;
                LOGI("3d settings reset from the panel");
            }
            else if (onReset && ctx->triggerEdge[h]) {
                // Hands the curve back to the preference and drops the
                // placement, which is all it takes: updatePlacement reseeds
                // from the preferences on the next frame and marks the pose
                // dirty itself, so the reset persists with nothing else to do.
                // The panel stays open so the jump is visible.
                ctx->panelCurve = -1.0f;
                ctx->placementValid = 0;
                LOGI("screen placement reset from the panel");
            }
            else if (row >= 0 && ctx->triggerEdge[h]) {
                ctx->cogDragSlider = row;
                ctx->cogDragHand = h;
                // Jumps to where the press landed rather than waiting for the
                // first bit of movement
                cogApplySlider(ctx, ctx->cogTab, row, pu);
            }
        }

        // A press that lands off the panel closes it, which is also how the
        // cog button shuts what it opened. One inside that hits nothing is
        // swallowed, so a near miss does not put the panel away.
        if (hand < 0) {
            for (int h = 0; h < SRC_COUNT; h++) {
                if (ctx->triggerEdge[h]) {
                    ctx->cogOpen = 0;
                    swallowTrigger(ctx, h);
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
    else if (hover == HOVER_COGBUTTON) {
        ctx->cogButtonHot = 1;
        if (ctx->triggerEdge[hand]) {
            ctx->cogOpen = !ctx->cogOpen;
            if (ctx->cogOpen) {
                // Always opens on the first tab, so the button does the same
                // thing every time
                ctx->cogTab = COG_TAB_SCREEN;
                ctx->cogPose = cogPanelPose(ctx, &ctx->cogW, &ctx->cogH);
            }
        }
    }
    else if (hover == HOVER_KBBUTTON) {
        ctx->kbButtonHot = 1;
        if (ctx->triggerEdge[hand]) {
            ctx->kbOpen = !ctx->kbOpen;
            if (ctx->kbOpen) {
                // Always comes up in lowercase, so the first key is where the
                // eye expects it however it was left last time
                ctx->kbState = KB_STATE_LOWER;
                ctx->kbPose = kbPanelPose(ctx, &ctx->kbW, &ctx->kbH);
            }
            LOGI("keyboard %s", ctx->kbOpen ? "open" : "closed");
        }
    }
    else if (hover == HOVER_KBPANEL) {
        int key = kbKeyAt(ctx, hitU[hand], hitV[hand]);
        ctx->kbHoverKey = key;
        ctx->kbKeyDown = key >= 0 && ctx->triggerDown[hand];
        if (key >= 0 && ctx->triggerEdge[hand]) {
            int code = ctx->kbCodes[ctx->kbState][key];
            if (code == KB_CODE_SHIFT) {
                // Shift off the symbols page goes to the capitals rather than
                // back where it came from
                ctx->kbState = ctx->kbState == KB_STATE_UPPER
                        ? KB_STATE_LOWER : KB_STATE_UPPER;
            }
            else if (code == KB_CODE_SYMBOLS) {
                ctx->kbState = ctx->kbState == KB_STATE_SYMBOLS
                        ? KB_STATE_LOWER : KB_STATE_SYMBOLS;
            }
            else if (code == KB_CODE_HIDE) {
                ctx->kbOpen = 0;
                LOGI("keyboard closed");
            }
            else if (code > 0) {
                // One key a frame, which is as fast as anyone presses them
                out[IN_KEY] = (float)code;
                // Shift is one shot over the letters, the way a phone keyboard
                // behaves, and sticky over the punctuation row above them
                if (ctx->kbState == KB_STATE_UPPER && code >= 'A' && code <= 'Z') {
                    ctx->kbState = KB_STATE_LOWER;
                }
            }
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

    // A press that lands on nothing at all puts the keyboard away, the same way
    // one off the grid or the settings panel closes those. Only empty ground
    // counts: a press on the picture is a mouse click and stays one, and the
    // furniture and the keys themselves keep their own meanings. Every source
    // is checked rather than the one doing the pointing, so a second hand can
    // dismiss it while the first is still on the screen.
    if (ctx->kbOpen && !ctx->pickerOpen && !ctx->cogOpen) {
        for (int h = 0; h < SRC_COUNT; h++) {
            if (!aimValid[h] || !ctx->pointerAwake || !ctx->triggerEdge[h]) {
                continue;
            }
            // The halo is the invisible fringe around the picture, so it reads
            // as empty space too
            if (hovers[h] == HOVER_NONE || hovers[h] == HOVER_HALO) {
                ctx->kbOpen = 0;
                swallowTrigger(ctx, h);
                LOGI("keyboard closed");
                break;
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
            || hover == HOVER_LOCK || hover == HOVER_HALO || hover == HOVER_COGBUTTON
            || hover == HOVER_COGPANEL || hover == HOVER_KBBUTTON
            || hover == HOVER_KBPANEL) && headValid && hand >= 0) {
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
    for (int h = 0; h < HAND_COUNT; h++) {
        // Per hand rather than either hand, so a trigger spent on the grid or a
        // panel is out of the count while the other one still clicks
        if (ctx->triggerDown[h] && !ctx->triggerSwallowed[h]) {
            mask |= VR_BUTTON_LEFT;
        }
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

// Small integer mode property, left alone if unset or unparseable
static void propInt(const char* name, int* target, long maxRaw) {
    char value[PROP_VALUE_MAX];
    value[0] = '\0';
    if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
        return;
    }
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end != value && v >= 0 && v <= maxRaw) {
        *target = (int)v;
    }
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
    propScaled(PROP_AIM_CUTOFF, &ctx->aimMinCutoff, 0.1f, 200);
    propScaled(PROP_AIM_BETA, &ctx->aimBeta, 0.5f, 100);
    // Millimetres
    propScaled(PROP_BEAM_WIDTH, &ctx->beamWidth, 0.001f, 100);
    // Tenths of a second
    propScaled(PROP_POINTER_WAKE, &ctx->pointerWake, 0.1f, 100);
    propScaled(PROP_POINTER_SLEEP, &ctx->pointerSleep, 0.1f, 600);
    // Metres. Zero is the infinite sphere the layer starts out as.
    propScaled(PROP_ENV_RADIUS, &ctx->envRadius, 1.0f, 200);
    // 0 off, 1 normal, 2 quality
    propInt(PROP_SHARPEN, &ctx->sharpenMode, 2);
    // 0 forces the glow off, 1 to 100 forces it on at that intensity, and
    // unset leaves the panel in charge. Same trap as the rest of these: one
    // left set from an earlier session quietly overrides the panel.
    propInt(PROP_AMBILIGHT, &ctx->ambiOverride, 100);
    propPercent(PROP_AMBI_SMOOTH, &ctx->ambiSmooth);
    // 0 forces the room off, 1 forces the minimal room, 2 the psx cinema, and
    // unset leaves the picker in charge
    propInt(PROP_ROOM, &ctx->roomOverride, 2);
    // Percent, both of them, and 0 hands the value back to the room. The scale
    // reaches the cinema only, and moving it rebuilds the geometry, so it is
    // not a knob to sit on a slider.
    propScaled(PROP_ROOM_SCALE, &ctx->roomScaleOverride, 0.01f, 400);
    propScaled(PROP_ROOM_DIM, &ctx->roomDimOverride, 0.01f, 200);

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

// What the glow is doing this frame. The panel owns it, with the debug
// property over the top of it the way the separation override works.
static void ambiEffective(XrCtx* ctx, int* on, float* level) {
    int enabled = ctx->ambilightOn;
    float value = ctx->ambiIntensity;
    if (ctx->ambiOverride >= 0) {
        enabled = ctx->ambiOverride > 0;
        value = ctx->ambiOverride / 100.0f;
    }
    *on = enabled;
    *level = value;
}

// Boils the frame down to the tiny colour texture. Kept self contained and
// free of anything glow shaped, since it is the frame's colours rather than
// the glow that anything else would want.
static void runFrameColorSample(XrCtx* ctx, const float* texMatrix) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->ambiFbo);
    glViewport(0, 0, AMBI_SAMPLE_TEX, AMBI_SAMPLE_TEX);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glUseProgram(ctx->ambiProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ctx->oesTexture);
    glUniformMatrix4fv(ctx->ambiTexMatrixUniform, 1, GL_FALSE, texMatrix);

    // Mixed into what is already there rather than replacing it. A cut to a
    // different scene would otherwise strobe the whole glow in one frame,
    // where this takes about ten to get there. The first frame has nothing to
    // mix with, so it lands whole.
    if (ctx->ambiSeeded) {
        glEnable(GL_BLEND);
        glBlendColor(ctx->ambiSmooth, ctx->ambiSmooth, ctx->ambiSmooth, ctx->ambiSmooth);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    }

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
    ctx->ambiSeeded = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Spreads those colours over the glow quad, into an image the compositor then
// places behind the screen
static void runGlowRender(XrCtx* ctx) {
    if (ctx->glowSwapchain == XR_NULL_HANDLE) {
        return;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->glowSwapchain, &acquire, &index),
                 "acquire glow image")) {
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->glowSwapchain, &wait);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->glowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->glowImages[index].image, 0);
    glViewport(0, 0, GLOW_TEX, GLOW_TEX);
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    int on;
    float level;
    ambiEffective(ctx, &on, &level);

    glUseProgram(ctx->glowProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    glUniform1f(ctx->glowIntensityUniform, level);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, VERTEX_DATA + 2);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->glowSwapchain, &release);
    ctx->glowRendered = 1;
}

// The room as it stands, which the generator and the shipped look both come
// out of. Metres, and the origin is where the viewer starts. Bare walls with
// nothing in them: the picture is the only thing here worth looking at.
static RoomParams minimalRoomParams(void) {
    RoomParams p;
    memset(&p, 0, sizeof(p));
    p.halfWidth = 4.5f;
    p.floorY = -1.4f;
    // One level throughout, so the picture stands on the same floor as the
    // viewer
    p.screenFloorY = p.floorY;
    p.ceilingY = 3.0f;
    p.screenZ = -5.5f;
    p.backZ = 4.0f;
    // Eight quads a side keeps the gradients smooth across a nine metre wall
    // for a few hundred vertices in total
    p.subdiv = 8;
    p.wallLevel = 0.055f;
    p.floorLevel = 0.065f;
    p.ceilingLevel = 0.038f;
    // Where the picture is hung, and the point the spill is baked from. The
    // bake point sits proud of the wall the way the screen does, so the wall
    // behind the picture catches some of its light too.
    p.screenMountY = 0.6f;
    p.screenProud = 0.10f;
    // Half again the 3 m the sliders start on. A wall nine metres across can
    // carry it, and at this distance it is what the room is for.
    p.screenWidth = 4.5f;
    Vec3 screenAt = { 0.0f, p.screenMountY, p.screenZ + p.screenProud };
    p.screenAt = screenAt;
    p.spillRadius = 2.2f;
    p.spillGain = 0.30f;
    p.seed = 0x9e3779b9u;
    return p;
}

// How high the viewer anchor sits in the model's own space at a given scale.
// The tier under them lands at eye height whatever the room is scaled to, so
// the floor stays where it is while the walls come in and out around it.
static float roomModelAnchorY(float scale) {
    return ROOM_MODEL_TIER_Y + ROOM_EYE_HEIGHT_M / scale;
}

// The baked cinema, measured off the model and put through the same
// (model - anchor) * scale the geometry is, so the screen hangs in the
// proscenium at every scale. The screen sits in the recess behind the curtains,
// so it needs nothing standing it off the wall, and the whole of it is
// textured, so the surface levels and the subdiv the generator works from are
// unused here.
static RoomParams psxCinemaParams(float scale) {
    float anchorY = roomModelAnchorY(scale);
    RoomParams p;
    memset(&p, 0, sizeof(p));
    p.halfWidth = 15.47f * scale;
    // The seating tier the viewer stands on, which the anchor holds at eye
    // height, and the ceiling over the stalls
    p.floorY = -ROOM_EYE_HEIGHT_M;
    // The stage floor at the far wall, model y -4.25, which is a good way below
    // the tier and is what the picture has to clear
    p.screenFloorY = (-4.25f - anchorY) * scale;
    p.ceilingY = (8.57f - anchorY) * scale;
    // The screen wall is at model z -27.53, and the picture hangs 0.18 proud of
    // it, so model -27.35 through the anchor at -12
    p.screenZ = -15.35f * scale;
    p.backZ = 14.33f * scale;
    // The centre of the proscenium opening is model y 2.85, and the picture
    // hangs 15 percent of its own height under that: the opening is 18 model
    // units across, so 10.125 high at 16:9, and 2.85 - 0.15 * 10.125 is 1.33
    p.screenMountY = (1.33f - anchorY) * scale;
    p.screenProud = 0.0f;
    // The opening is 20 m across at full size, so this fills it with a margin
    // either side
    p.screenWidth = 18.0f * scale;
    Vec3 screenAt = { 0.0f, p.screenMountY, p.screenZ };
    p.screenAt = screenAt;
    // A room this size takes the light much further than the small one. It
    // takes less of it per surface than a painted wall would, since the atlas
    // is already carrying the colour, but not as little as it first shipped
    // with: over a textured surface a fifth of the frame's colour never read
    // as light at all.
    p.spillRadius = 7.0f * scale;
    p.spillGain = 0.55f;
    p.texMix = 1.0f;
    // The atlas is already painted as an interior with the lights down, so it
    // goes on as it was baked
    p.dim = 1.0f;
    p.seed = 0x85ebca6bu;
    return p;
}

// Which room a style asks for, at the scale that style is drawn. Anything
// unknown falls back to the generated one rather than leaving the buffers empty.
static RoomParams roomParams(int style, float scale) {
    if (style == ROOM_STYLE_PSX) {
        return psxCinemaParams(scale);
    }
    return minimalRoomParams();
}

// How large a style is drawn. Only the baked room is scaled: the generated one
// is built at the size its own params give. A property set inside the range
// wins over the shipped default.
static float roomScale(XrCtx* ctx, int style) {
    if (style != ROOM_STYLE_PSX) {
        return 1.0f;
    }
    float scale = ctx->roomScaleOverride > 0.0f ? ctx->roomScaleOverride : ROOM_PSX_SCALE;
    if (scale < ROOM_SCALE_MIN) {
        scale = ROOM_SCALE_MIN;
    }
    if (scale > ROOM_SCALE_MAX) {
        scale = ROOM_SCALE_MAX;
    }
    return scale;
}

// How far down the atlas is turned as the room draws. Nothing is baked into the
// geometry from this, so the property moves it frame to frame with no rebuild
// behind it, and it wins over whatever the built style left in place.
static float roomDim(XrCtx* ctx) {
    if (ctx->roomDimOverride <= 0.0f) {
        return ctx->roomDim;
    }
    float dim = ctx->roomDimOverride;
    if (dim < ROOM_DIM_MIN) {
        dim = ROOM_DIM_MIN;
    }
    if (dim > ROOM_DIM_MAX) {
        dim = ROOM_DIM_MAX;
    }
    return dim;
}

// In a 3d room the picture hangs on the far wall, so the placement the sliders
// and the grab produce is put aside on the way in and handed back on the way
// out. Nothing is written to preferences either way: what the user set up in a
// normal environment is still there when they come back to one.
static void applyRoomPlacement(XrCtx* ctx, int style, float aspect, int reseeded) {
    int roomOn = style > 0;
    if (roomOn && !ctx->roomHoldingScreen) {
        ctx->savedScreenPose = ctx->screenPose;
        ctx->savedScreenWidth = ctx->screenWidth;
        ctx->savedScreenRadius = ctx->screenRadius;
        ctx->roomHoldingScreen = 1;
        // Anything held would spend the rest of the drag fighting the wall
        ctx->grabMode = GRAB_NONE;
    }
    else if (roomOn && reseeded) {
        // The panel's reset landed while the room had the screen. What it
        // seeded is the placement that should be waiting when the room ends.
        ctx->savedScreenPose = ctx->screenPose;
        ctx->savedScreenWidth = ctx->screenWidth;
        ctx->savedScreenRadius = ctx->screenRadius;
    }
    else if (!roomOn && ctx->roomHoldingScreen) {
        ctx->screenPose = ctx->savedScreenPose;
        ctx->screenWidth = ctx->savedScreenWidth;
        ctx->screenRadius = ctx->savedScreenRadius;
        ctx->roomHoldingScreen = 0;
    }
    if (!roomOn) {
        return;
    }

    // The same scale the geometry was built at, so the picture and the walls
    // around it never disagree
    RoomParams p = roomParams(style, roomScale(ctx, style));
    // The room says how big its picture is, not the size slider: the wall is a
    // known size and the picture is hung to suit it. The clamps below only
    // catch a room whose width does not fit its own wall.
    float width = p.screenWidth;
    float maxWidth = 2.0f * p.halfWidth - 0.4f;
    float maxHeight = (p.ceilingY - p.floorY) - 0.3f;
    if (width > maxWidth) {
        width = maxWidth;
    }
    if (width * aspect > maxHeight) {
        width = maxHeight / aspect;
    }
    float height = width * aspect;
    // And hung where the whole of it is on the wall rather than through the
    // floor or the ceiling. The floor here is the one under the picture, not
    // the tier the viewer is on, which in a raked room is metres higher and
    // would push the picture back up the wall.
    float mount = p.screenMountY;
    float lowest = p.screenFloorY + height * 0.5f + 0.1f;
    float highest = p.ceilingY - height * 0.5f - 0.1f;
    if (mount < lowest) {
        mount = lowest;
    }
    if (mount > highest) {
        mount = highest;
    }

    // Square to the wall and facing the viewer, the same identity orientation
    // the placement starts out with
    memset(&ctx->screenPose, 0, sizeof(ctx->screenPose));
    ctx->screenPose.orientation.w = 1.0f;
    ctx->screenPose.position.y = mount;
    ctx->screenPose.position.z = p.screenZ + p.screenProud;
    ctx->screenWidth = width;
}

// Whether the assets a baked room is made of have both arrived
static int roomAssetsReady(XrCtx* ctx) {
    return ctx->roomModelReady && ctx->roomTextureReady;
}

// Turns the loaded model into the layout the room's buffer is in. Nothing is
// generated here beyond the light: the shape and the texture coordinates come
// off the model, and the colour is mixed out by the atlas. The model arrives in
// its own space, so this is where the anchor and the scale go on. The normals
// are left alone, since a uniform scale does not turn them.
static int buildModelRoomGeometry(XrCtx* ctx, const RoomParams* p, float scale, float* verts,
                                  unsigned short* indices, int* vertexCount, int* indexCount) {
    if (!ctx->roomModelReady) {
        return 0;
    }
    float anchorY = roomModelAnchorY(scale);
    static const float white[3] = { 1.0f, 1.0f, 1.0f };
    for (int i = 0; i < ctx->roomModelVertexCount; i++) {
        const float* src = ctx->roomModelVerts + (size_t)i * ROOM_MODEL_FLOATS;
        Vec3 pos = { (src[0] - ROOM_MODEL_ANCHOR_X) * scale,
                     (src[1] - anchorY) * scale,
                     (src[2] - ROOM_MODEL_ANCHOR_Z) * scale };
        Vec3 normal = { src[3], src[4], src[5] };
        roomWriteVertex(p, verts, i, pos, white, roomSpillWeight(p, pos, normal),
                        src[6], src[7]);
    }
    memcpy(indices, ctx->roomModelIndices,
           (size_t)ctx->roomModelIndexCount * sizeof(unsigned short));
    *vertexCount = ctx->roomModelVertexCount;
    *indexCount = ctx->roomModelIndexCount;
    return 1;
}

// Builds a style's room and hands it to the buffers. Called once for the first
// room and again whenever the picker moves to another: a one off pass over a
// few thousand vertices, which is cheaper than keeping every room resident for
// a switch that may never come.
static int uploadRoomGeometry(XrCtx* ctx, int style) {
    float scale = roomScale(ctx, style);
    RoomParams params = roomParams(style, scale);
    int baked = style == ROOM_STYLE_PSX;
    if (baked && !roomAssetsReady(ctx)) {
        return 0;
    }
    int maxVerts = 0;
    int maxIndices = 0;
    if (baked) {
        maxVerts = ctx->roomModelVertexCount;
        maxIndices = ctx->roomModelIndexCount;
    }
    else {
        roomMaxCounts(&params, &maxVerts, &maxIndices);
    }
    float* verts = malloc((size_t)maxVerts * ROOM_VERTEX_FLOATS * sizeof(float));
    unsigned short* indices = malloc((size_t)maxIndices * sizeof(unsigned short));
    if (verts == NULL || indices == NULL) {
        free(verts);
        free(indices);
        LOGE("room geometry allocation failed");
        return 0;
    }

    int vertexCount = 0;
    int indexCount = 0;
    int ok = baked
            ? buildModelRoomGeometry(ctx, &params, scale, verts, indices, &vertexCount, &indexCount)
            : buildRoomGeometry(&params, style, verts, maxVerts, indices, maxIndices,
                                &vertexCount, &indexCount);
    if (ok) {
        if (ctx->roomVertexBuffer == 0) {
            glGenBuffers(1, &ctx->roomVertexBuffer);
        }
        glBindBuffer(GL_ARRAY_BUFFER, ctx->roomVertexBuffer);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)vertexCount * ROOM_VERTEX_FLOATS * sizeof(float),
                     verts, GL_STATIC_DRAW);
        if (ctx->roomIndexBuffer == 0) {
            glGenBuffers(1, &ctx->roomIndexBuffer);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->roomIndexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)indexCount * sizeof(unsigned short),
                     indices, GL_STATIC_DRAW);
        // Everything else in here draws from client arrays with no buffer
        // bound, so leaving one bound would turn their pointers into offsets
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        ctx->roomVertexCount = vertexCount;
        ctx->roomIndexCount = indexCount;
        ctx->roomSpillGain = params.spillGain;
        ctx->roomTexMix = params.texMix;
        ctx->roomDim = params.dim;
        if (baked) {
            // A textured room has no wall shade to take this from, and its shell
            // is closed, so all this covers is the frame before the first draw
            ctx->roomClear[0] = 0.010f;
            ctx->roomClear[1] = 0.010f;
            ctx->roomClear[2] = 0.012f;
        }
        else {
            // Darker than any surface in the room, so anything the geometry
            // misses reads as the far end of the same room rather than a hole
            ctx->roomClear[0] = params.wallLevel * 0.5f;
            ctx->roomClear[1] = params.wallLevel * 0.5f;
            ctx->roomClear[2] = params.wallLevel * 0.5f;
        }
        LOGEV("room ready, style %d, scale %.2f, %d vertices, %d indices",
              style, scale, vertexCount, indexCount);
    }
    free(verts);
    free(indices);
    if (!ok) {
        LOGE("room geometry build failed for style %d", style);
    }
    return ok;
}

// Which style can actually be built at this moment. A baked room cannot come up
// until its model and atlas have been read off the assets, so until they land
// the generated room stands in for it and the picker never shows a black world.
static int buildableRoomStyle(XrCtx* ctx, int style) {
    if (style < ROOM_STYLE_MINIMAL) {
        style = ROOM_STYLE_MINIMAL;
    }
    if (style == ROOM_STYLE_PSX && !roomAssetsReady(ctx)) {
        return ROOM_STYLE_MINIMAL;
    }
    return style;
}

// Brings up everything the room draws with, the first frame that asks for it.
// Mid session swapchain creation is already how the background photo arrives.
static int initRoom(XrCtx* ctx) {
    if (ctx->roomReady) {
        return 1;
    }
    if (ctx->roomFailed || ctx->session == XR_NULL_HANDLE) {
        return 0;
    }
    ctx->roomFailed = 1;

    // The whole of what the runtime recommends per eye, so the room's edges are
    // as sharp as the video layer sitting in front of them. That is a couple of
    // hundred megabytes between the side by side colour swapchain and the depth
    // buffer, which is the reason none of it exists until a room is picked. Gen
    // 1 headsets keep the half size the room started on, and a runtime that
    // will not say what it wants gets a modest guess.
    int half = ctx->gen1Headset;
    int maxEye = half ? ROOM_MAX_EYE : ROOM_MAX_EYE_FULL;
    int eyeW = ctx->recommendedEyeWidth > 0 ? ctx->recommendedEyeWidth : 1024;
    int eyeH = ctx->recommendedEyeHeight > 0 ? ctx->recommendedEyeHeight : 1024;
    if (half) {
        eyeW /= 2;
        eyeH /= 2;
    }
    if (eyeW > maxEye) {
        eyeW = maxEye;
    }
    if (eyeH > maxEye) {
        eyeH = maxEye;
    }

    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = ctx->swapchainFormat;
    info.sampleCount = 1;
    // Side by side, the same arrangement the video swapchain uses in stereo
    info.width = eyeW * ROOM_EYES;
    info.height = eyeH;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!checkXr(xrCreateSwapchain(ctx->session, &info, &ctx->roomSwapchain),
                 "create room swapchain")) {
        ctx->roomSwapchain = XR_NULL_HANDLE;
        return 0;
    }
    xrEnumerateSwapchainImages(ctx->roomSwapchain, 0, &ctx->roomImageCount, NULL);
    ctx->roomImages = calloc(ctx->roomImageCount, sizeof(XrSwapchainImageOpenGLESKHR));
    if (ctx->roomImages == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < ctx->roomImageCount; i++) {
        ctx->roomImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    xrEnumerateSwapchainImages(ctx->roomSwapchain, ctx->roomImageCount, &ctx->roomImageCount,
                               (XrSwapchainImageBaseHeader*)ctx->roomImages);

    // The one pass in here that needs a depth buffer, since it is the only one
    // drawing geometry that can be in front of other geometry
    glGenRenderbuffers(1, &ctx->roomDepthBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, ctx->roomDepthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)info.width, eyeH);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // Colour comes from whichever swapchain image the frame acquires, so only
    // the depth attachment can be made once
    glGenFramebuffers(1, &ctx->roomFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->roomFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              ctx->roomDepthBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLuint vs = compileShader(GL_VERTEX_SHADER, ROOM_VERTEX_SRC);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, ROOM_FRAGMENT_SRC);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    ctx->roomProgram = glCreateProgram();
    glAttachShader(ctx->roomProgram, vs);
    glAttachShader(ctx->roomProgram, fs);
    glBindAttribLocation(ctx->roomProgram, 0, "a_position");
    glBindAttribLocation(ctx->roomProgram, 1, "a_color");
    glBindAttribLocation(ctx->roomProgram, 2, "a_spill");
    glBindAttribLocation(ctx->roomProgram, 3, "a_uv");
    glLinkProgram(ctx->roomProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(ctx->roomProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(ctx->roomProgram, sizeof(log), NULL, log);
        LOGE("room program link failed: %s", log);
        return 0;
    }
    ctx->roomViewProjUniform = glGetUniformLocation(ctx->roomProgram, "u_viewproj");
    ctx->roomSpillGainUniform = glGetUniformLocation(ctx->roomProgram, "u_spillGain");
    ctx->roomTexMixUniform = glGetUniformLocation(ctx->roomProgram, "u_texMix");
    ctx->roomDimUniform = glGetUniformLocation(ctx->roomProgram, "u_dim");
    glUseProgram(ctx->roomProgram);
    glUniform1i(glGetUniformLocation(ctx->roomProgram, "u_ambi"), 0);
    glUniform1i(glGetUniformLocation(ctx->roomProgram, "u_room"), 1);

    // The atlas sampler is read whatever the mix is set to, so there is always
    // a complete texture on that unit even before an atlas has been loaded
    glGenTextures(1, &ctx->roomWhiteTexture);
    glBindTexture(GL_TEXTURE_2D, ctx->roomWhiteTexture);
    const unsigned char white[4] = { 255, 255, 255, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Whichever room is being asked for, so the first frame is already the one
    // the picker is on rather than a rebuild later
    int wanted = roomEffective(ctx);
    int style = buildableRoomStyle(ctx, wanted);
    if (!uploadRoomGeometry(ctx, style)) {
        return 0;
    }
    ctx->roomBuiltStyle = style;
    ctx->roomWantedStyle = wanted;
    ctx->roomAssetsSeen = roomAssetsReady(ctx);
    ctx->roomWantedScale = roomScale(ctx, style);

    ctx->roomEyeWidth = eyeW;
    ctx->roomEyeHeight = eyeH;
    ctx->roomReady = 1;
    ctx->roomFailed = 0;
    LOGEV("room ready at %dx%d per eye", eyeW, eyeH);
    return 1;
}

// Where both eyes are this frame. Only the room needs this, so it is only
// asked for while a room is on.
static int locateRoomViews(XrCtx* ctx) {
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = ctx->predictedDisplayTime;
    locateInfo.space = ctx->localSpace;

    XrViewState state = { XR_TYPE_VIEW_STATE };
    XrView views[ROOM_EYES];
    for (int eye = 0; eye < ROOM_EYES; eye++) {
        views[eye].type = XR_TYPE_VIEW;
        views[eye].next = NULL;
    }
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(ctx->session, &locateInfo, &state, ROOM_EYES, &count, views))
            || count < ROOM_EYES) {
        return 0;
    }
    // Both bits, since a room drawn from an orientation with no position in it
    // would sit still while the head moves through the walls
    XrViewStateFlags needed = XR_VIEW_STATE_ORIENTATION_VALID_BIT
            | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        return 0;
    }

    for (int eye = 0; eye < ROOM_EYES; eye++) {
        ctx->roomViews[eye] = views[eye];
    }
    ctx->roomViewsValid = 1;
    return 1;
}

// Everything the room has to have built or rebuilt before it can be drawn.
// Kept out of the frame's timer query on purpose: a swapchain or a buffer
// created inside that window leaves this driver reporting garbage for every
// sample after it, so all of it happens before the query opens.
static void prepareRoom(XrCtx* ctx) {
    if (!initRoom(ctx)) {
        return;
    }
    // The picker can move between rooms with the session running, a baked one
    // can be picked before its assets have arrived, and the scale property can
    // move under either. Nothing about any of them changes frame to frame, so
    // the work only happens when the style asked for, the readiness of those
    // assets or the scale has moved: a build that fails leaves whichever room
    // is already in the buffers and is not tried again.
    int wanted = roomEffective(ctx);
    int assets = roomAssetsReady(ctx);
    int style = buildableRoomStyle(ctx, wanted);
    float scale = roomScale(ctx, style);
    if (wanted == ctx->roomWantedStyle && assets == ctx->roomAssetsSeen
            && scale == ctx->roomWantedScale) {
        return;
    }
    // Decided before the ask is recorded, since the scale is part of both
    int rebuild = style != ctx->roomBuiltStyle || scale != ctx->roomWantedScale;
    ctx->roomWantedStyle = wanted;
    ctx->roomAssetsSeen = assets;
    ctx->roomWantedScale = scale;

    if (rebuild && uploadRoomGeometry(ctx, style)) {
        ctx->roomBuiltStyle = style;
    }
}

// Draws the room into its own image, one half per eye. The layer that shows it
// is submitted in endFrame, with the very poses drawn from here. Nothing is
// created in here: prepareRoom has already been round.
static void renderRoom(XrCtx* ctx) {
    if (!ctx->roomReady) {
        return;
    }
    // A frame the eyes could not be located for keeps the image it already
    // has. The layer still goes up, with the poses that image was drawn from.
    if (!locateRoomViews(ctx)) {
        return;
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->roomSwapchain, &acquire, &index),
                 "acquire room image")) {
        return;
    }
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(ctx->roomSwapchain, &wait);

    // Opened only now, with the image in hand: this driver hands back wrapped
    // nonsense for a query that spans the compositor wait above
    int roomTiming = ctx->timerSupported && !ctx->captureRequested
            && !ctx->roomTimerPending[ctx->roomTimerSlot];
    if (roomTiming) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->roomTimerQueries[ctx->roomTimerSlot]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->roomFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ctx->roomImages[index].image, 0);
    // Once, on the first frame drawn. The colour attachment is a swapchain
    // image, so this is the first point the pair of them can be checked, and a
    // room that never appears is otherwise silent.
    if (!ctx->roomRendered) {
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("room framebuffer incomplete: 0x%x", status);
        }
    }
    // The colours below are authored the way the video arrives, already gamma
    // encoded, so the write must not encode them a second time
    if (ctx->srgbWriteControl) {
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    glClearColor(ctx->roomClear[0], ctx->roomClear[1], ctx->roomClear[2], 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    glUseProgram(ctx->roomProgram);
    // The atlas a baked room is painted with, or the white stand in, which the
    // mix below leaves out of the picture anyway
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx->roomTextureReady ? ctx->roomTexture
                                                       : ctx->roomWhiteTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->ambiTexture);
    // Nothing has been sampled off the video yet on the first frames, so the
    // room is just its baked self until there is, and the same for the option
    // turned off: the baked colours and the atlas stay, only the light the
    // picture throws goes. Deliberately not tied to the ambilight: the wash
    // inside a room and the glow around a floating screen are different
    // effects, and the colour sample they share is taken for either one.
    int lit = ctx->ambiSeeded && ctx->roomLightOn;
    glUniform1f(ctx->roomSpillGainUniform, lit ? ctx->roomSpillGain : 0.0f);
    glUniform1f(ctx->roomTexMixUniform, ctx->roomTexMix);
    glUniform1f(ctx->roomDimUniform, roomDim(ctx));

    glBindBuffer(GL_ARRAY_BUFFER, ctx->roomVertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->roomIndexBuffer);
    GLsizei stride = ROOM_VERTEX_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(7 * sizeof(float)));
    glEnableVertexAttribArray(3);

    for (int eye = 0; eye < ROOM_EYES; eye++) {
        glViewport(eye * ctx->roomEyeWidth, 0, ctx->roomEyeWidth, ctx->roomEyeHeight);

        float proj[16];
        float view[16];
        float viewProj[16];
        // Near enough to walk into a wall without it clipping, far enough to
        // hold a room a few metres across
        projectionFromFov(proj, ctx->roomViews[eye].fov, 0.05f, 60.0f);
        viewFromPose(view, ctx->roomViews[eye].pose);
        matMul(viewProj, proj, view);
        glUniformMatrix4fv(ctx->roomViewProjUniform, 1, GL_FALSE, viewProj);

        glDrawElements(GL_TRIANGLES, ctx->roomIndexCount, GL_UNSIGNED_SHORT, (const void*)0);
    }

    if (roomTiming) {
        pfnEndQuery(GL_TIME_ELAPSED_EXT);
        ctx->roomTimerPending[ctx->roomTimerSlot] = 1;
        ctx->roomTimerPendingFrames[ctx->roomTimerSlot] = 0;
        ctx->roomTimerSlot = 1 - ctx->roomTimerSlot;
    }

    glDisable(GL_DEPTH_TEST);
    // Handed back exactly as the other passes expect to find it: no buffers
    // bound, since they all draw from client arrays, and only the two attribute
    // arrays they use left on
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(ctx->roomSwapchain, &release);
    ctx->roomRendered = 1;
}

static void renderVideoFrame(XrCtx* ctx, const float* texMatrix, float separation) {
    int upsampling = ctx->stereoMode == DEPTH_MODE_MODEL && ctx->upsampleEnabled;
    int occluding = upsampling && ctx->occlusionEnabled && separation > 0.0f;

    // Capture frames do readbacks and file writes inside what would be the
    // query window, which both ruins the number and, on this driver, leaves a
    // query that never becomes available. Skip timing them.
    int timing = ctx->timerSupported && !ctx->captureRequested;

    // Before the query opens, since creating the room's swapchain and buffers
    // inside the window wedges every sample after it. Only the CPU and buffer
    // work: the room's own draw is at the end of this function.
    int roomOn = roomEffective(ctx) > 0;
    if (roomOn) {
        prepareRoom(ctx);
    }

    if (timing && !ctx->timerPending[ctx->timerSlot]) {
        pfnBeginQuery(GL_TIME_ELAPSED_EXT, ctx->timerQueries[ctx->timerSlot]);
    }

    if (upsampling) {
        runUpsample(ctx, texMatrix);
    }
    if (occluding) {
        runOffsetSearch(ctx, separation);
    }

    // Inside the query window with the rest of them, so what the glow costs
    // shows up in the same GPU number
    int glowOn;
    float glowLevel;
    ambiEffective(ctx, &glowOn, &glowLevel);
    // The room is lit from the same colours the glow is made of, so the sample
    // is taken for either of them. It happens here rather than with the room's
    // draw, which now follows the video, so the room is lit from this frame's
    // colour rather than the last one's.
    if (glowOn || roomOn) {
        runFrameColorSample(ctx, texMatrix);
    }
    if (glowOn) {
        runGlowRender(ctx);
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!checkXr(xrAcquireSwapchainImage(ctx->swapchain, &acquireInfo, &imageIndex), "acquire image")) {
        // The query opened above must not be left standing on this early out,
        // or the next frame's begin would nest inside it
        if (timing && !ctx->timerPending[ctx->timerSlot]) {
            pfnEndQuery(GL_TIME_ELAPSED_EXT);
            ctx->timerPending[ctx->timerSlot] = 1;
            ctx->timerPendingFrames[ctx->timerSlot] = 0;
            ctx->timerSlot = 1 - ctx->timerSlot;
        }
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
                // Only a plausible sample is kept: zero and anything past 50 ms
                // is the driver rather than a frame, and the wrapped negatives
                // land far past the cap. The disjoint flag is deliberately not
                // consulted: this driver raises it on every GPU clock change,
                // which the room render provokes constantly, and gating on it
                // starved the stats to nothing while the values stayed sane.
                if (elapsed > 0 && elapsed < 50000000ull) {
                    ctx->gpuTotalNs += (long)elapsed;
                    ctx->gpuSamples++;
                    ctx->overlayGpuTotalNs += (long)elapsed;
                    ctx->overlayGpuSamples++;
                    if ((long)elapsed > ctx->gpuMaxNs) {
                        ctx->gpuMaxNs = (long)elapsed;
                    }
                }
                else {
                    // Counted and kept so the stats line can say what the
                    // driver was actually handing back on a starved window
                    ctx->gpuDropped++;
                    ctx->gpuLastDroppedNs = elapsed;
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

    // Last of all, after the video has been queued and the main query closed.
    // GL runs one queue in order, and under head motion the compositor preempts
    // this full resolution pass often enough to stretch it past 20 ms: anything
    // queued behind it waits, which is what the video was doing while this ran
    // first. Behind the warp a preempted room only holds up its own layer, and
    // the compositor covers that by reprojecting the image it already has.
    // The two timer pairs stay disjoint: the main one is ended just above, and
    // renderRoom opens its own once it has its swapchain image.
    if (roomOn) {
        renderRoom(ctx);
    }

    // The room's pair is opened just above, but collected down here, on every
    // frame rather than only the ones it drew: a query from the last frame it
    // rendered still has to be picked up after it goes away.
    if (timing) {
        int roomOther = ctx->roomTimerSlot;
        if (ctx->roomTimerPending[roomOther]) {
            GLuint ready = 0;
            pfnGetQueryObjectuiv(ctx->roomTimerQueries[roomOther], GL_QUERY_RESULT_AVAILABLE_EXT,
                                 &ready);
            if (ready) {
                GLuint64 elapsed = 0;
                pfnGetQueryObjectui64v(ctx->roomTimerQueries[roomOther], GL_QUERY_RESULT_EXT,
                                       &elapsed);
                ctx->roomTimerPending[roomOther] = 0;
                ctx->roomTimerPendingFrames[roomOther] = 0;
                // Same plausibility filter as the warp's, for the same reason
                if (elapsed > 0 && elapsed < 50000000ull) {
                    ctx->roomGpuTotalNs += (long)elapsed;
                    ctx->roomGpuSamples++;
                }
                else {
                    ctx->roomGpuDropped++;
                }
            }
            else if (++ctx->roomTimerPendingFrames[roomOther] > 90) {
                ctx->roomTimerPending[roomOther] = 0;
                ctx->roomTimerPendingFrames[roomOther] = 0;
                LOGW("room: gave up on a GPU timer query that never landed");
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
        if (tabs[tab] == NULL) {
            continue;
        }
        const unsigned char* px = (*env)->GetDirectBufferAddress(env, tabs[tab]);
        if (px != NULL) {
            ctx->cogPanelReady[tab] = uploadFlipped(ctx, ctx->cogPanelSwapchains[tab],
                                                    ctx->cogPanelImages[tab], px,
                                                    COG_TEX_W, COG_TEX_H);
        }
    }
    if (button != NULL) {
        const unsigned char* px = (*env)->GetDirectBufferAddress(env, button);
        if (px != NULL) {
            ctx->cogButtonReady = uploadFlipped(ctx, ctx->cogButtonSwapchain,
                                                ctx->cogButtonImages, px,
                                                OUTLINE_TEX, OUTLINE_TEX);
        }
    }
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
        if (sheets[state] == NULL) {
            continue;
        }
        const unsigned char* px = (*env)->GetDirectBufferAddress(env, sheets[state]);
        if (px != NULL) {
            ctx->kbPanelReady[state] = uploadFlipped(ctx, ctx->kbPanelSwapchains[state],
                                                     ctx->kbPanelImages[state], px,
                                                     KB_TEX_W, KB_TEX_H);
        }
    }
    if (buttonIcon != NULL) {
        const unsigned char* px = (*env)->GetDirectBufferAddress(env, buttonIcon);
        if (px != NULL) {
            ctx->kbButtonReady = uploadFlipped(ctx, ctx->kbButtonSwapchain,
                                               ctx->kbButtonImages, px,
                                               OUTLINE_TEX, OUTLINE_TEX);
        }
    }

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

// Whether curved screens are available at all, which is what says if the panel
// should draw its curve row live or greyed out
JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeGetCylinderSupported(JNIEnv* env, jobject thiz,
                                                                       jlong handle) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    return (ctx != NULL && ctx->cylinderSupported) ? JNI_TRUE : JNI_FALSE;
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

// The baked room model. Read off the assets in Java and parsed here, since the
// renderer has no glTF loader: the bake script has already flattened it to
// positions, normals and texture coordinates. Handed over from the frame loop,
// which is the thread that builds the geometry out of it.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomModel(JNIEnv* env, jobject thiz,
                                                                   jlong handle, jobject buffer,
                                                                   jint length) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || length < 12) {
        return;
    }
    const unsigned char* data = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (data == NULL || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)length) {
        return;
    }
    if (memcmp(data, "MXR1", 4) != 0) {
        LOGW("room model is not an MXR1 file, ignoring it");
        return;
    }

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    memcpy(&vertexCount, data + 4, sizeof(vertexCount));
    memcpy(&indexCount, data + 8, sizeof(indexCount));
    // Both are held to what the file could possibly hold before any of the byte
    // counts are worked out, so none of the arithmetic below can wrap
    size_t payload = (size_t)length - 12;
    if (vertexCount == 0 || vertexCount > ROOM_MAX_VERTS
            || indexCount == 0 || indexCount % 3 != 0
            || indexCount > payload / sizeof(unsigned short)) {
        LOGW("room model counts make no sense: %u vertices, %u indices",
             vertexCount, indexCount);
        return;
    }
    size_t vertexBytes = (size_t)vertexCount * ROOM_MODEL_FLOATS * sizeof(float);
    size_t indexBytes = (size_t)indexCount * sizeof(unsigned short);
    if (12 + vertexBytes + indexBytes != (size_t)length) {
        LOGW("room model is %d bytes, its header asks for %zu",
             length, 12 + vertexBytes + indexBytes);
        return;
    }

    float* verts = malloc(vertexBytes);
    unsigned short* indices = malloc(indexBytes);
    if (verts == NULL || indices == NULL) {
        free(verts);
        free(indices);
        LOGE("room model allocation failed");
        return;
    }
    memcpy(verts, data + 12, vertexBytes);
    memcpy(indices, data + 12 + vertexBytes, indexBytes);

    for (uint32_t i = 0; i < indexCount; i++) {
        if (indices[i] >= vertexCount) {
            free(verts);
            free(indices);
            LOGW("room model index %u is past its %u vertices",
                 (unsigned)indices[i], vertexCount);
            return;
        }
    }
    // Kept in the model's own space. The anchor and the scale go on as the
    // geometry is built, so the scale can move without this being read again.
    free(ctx->roomModelVerts);
    free(ctx->roomModelIndices);
    ctx->roomModelVerts = verts;
    ctx->roomModelIndices = indices;
    ctx->roomModelVertexCount = (int)vertexCount;
    ctx->roomModelIndexCount = (int)indexCount;
    ctx->roomModelReady = 1;
    LOGEV("room model ready, %u vertices, %u indices", vertexCount, indexCount);
}

// The atlas that model is painted with. A plain texture rather than a swapchain,
// since nothing composites it: the room samples it as it draws. Also from the
// frame loop, which is where the GL context is current.
JNIEXPORT void JNICALL
Java_com_limelight_binding_video_XrRenderer_nativeUploadRoomTexture(JNIEnv* env, jobject thiz,
                                                                     jlong handle, jobject buffer,
                                                                     jint width, jint height) {
    XrCtx* ctx = (XrCtx*)(intptr_t)handle;
    if (ctx == NULL || buffer == NULL || width <= 0 || height <= 0) {
        return;
    }
    const unsigned char* px = (const unsigned char*)(*env)->GetDirectBufferAddress(env, buffer);
    if (px == NULL
            || (*env)->GetDirectBufferCapacity(env, buffer) < (jlong)width * height * 4) {
        return;
    }

    if (ctx->roomTexture == 0) {
        glGenTextures(1, &ctx->roomTexture);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->roomTexture);
    // The rows arrive top down out of the decoder and the model's texture
    // coordinates start at the top too, so this one is not flipped on the way in
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);
    // A wall seen at a glancing angle across a room this size is minified hard,
    // so the atlas is worth the mip chain
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    ctx->roomTextureReady = 1;
    LOGEV("room texture %dx%d ready", width, height);
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
    float p[10];
    if ((*env)->GetArrayLength(env, poseArr) < 10) {
        return;
    }
    (*env)->GetFloatArrayRegion(env, poseArr, 0, 10, p);

    if (p[7] < SCREEN_MIN_WIDTH || p[7] > SCREEN_MAX_WIDTH || p[8] <= 0.0f) {
        LOGW("stored screen placement out of range, ignoring it");
        return;
    }
    // Anything below zero means the panel never set a curve, and anything else
    // out of range is not worth trusting either
    ctx->panelCurve = (p[9] >= 0.0f && p[9] <= 1.0f) ? p[9] : -1.0f;

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

    float aspect = (float)ctx->videoHeight / (float)ctx->videoWidth;
    int stereo = ctx->stereoMode != DEPTH_MODE_OFF;
    int roomStyle = roomEffective(ctx);
    int roomOn = roomStyle > 0;
    // A room hangs the picture on a wall, and a wall does not follow the head
    // about however the preference is set
    XrSpace space = (headLocked && !roomOn) ? ctx->viewSpace : ctx->localSpace;

    if (!ctx->pointerArtReady && ctx->pointerSwapchain != XR_NULL_HANDLE && ctx->shouldRender) {
        uploadPointerArt(ctx);
    }

    // The panel's curve, if it has one, so a reseed keeps the curve in force
    // rather than snapping back to the preference
    float curve = effectiveCurvature(ctx);
    int reseeded = updatePlacement(ctx, distance, quadWidth, curve);
    // Then the wall has the last word on where the picture is, and a picture
    // flat on a wall is flat
    applyRoomPlacement(ctx, roomStyle, aspect, reseeded);
    if (roomOn) {
        curve = 0.0f;
    }
    XrPosef screenPose = ctx->screenPose;
    float screenWidth = ctx->screenWidth;
    float screenHeight = screenWidth * aspect;

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

    XrCompositionLayerEquirect2KHR backgroundLayer;
    XrCompositionLayerProjection roomLayer;
    XrCompositionLayerProjectionView roomProjViews[ROOM_EYES];
    XrCompositionLayerQuad glowLayer;
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
    XrCompositionLayerQuad cogButtonLayer;
    XrCompositionLayerQuad cogPanelLayer;
    XrCompositionLayerQuad kbButtonLayer;
    XrCompositionLayerQuad kbPanelLayer;
    XrCompositionLayerQuad kbMarkLayer;
    XrCompositionLayerQuad cogThumbLayers[COG_SLIDER_COUNT];
    // One per option row for what is chosen, plus one for the hover
    XrCompositionLayerQuad cogMarkLayers[COG_OPTION_COUNT + 1];
    XrCompositionLayerSettingsFB sharpenSettings;
    // Worst case reachable is the screen tab open: background, the glow, both
    // eyes, stats, the cog button, the panel, six thumbs, ray and cursor, which
    // is 15. The 3d room replaces the environment layer one for one, and sheds
    // the move pill and the screen tab's thumbs, so it only ever comes to
    // less. The panel is modal, and since the frame a modal opens now sheds
    // the bar furniture too, the two can no longer land in one frame together.
    // The keyboard sheds the same furniture and adds only its panel and one
    // ring, so it comes to 9. Sized well past that anyway: an overflow here is
    // a smashed stack, and the margin costs five pointers.
    const XrCompositionLayerBaseHeader* layers[20];
    uint32_t layerCount = 0;

    // Compositor sharpening during its sampling pass, so it costs us nothing.
    // One struct serves every layer that wants it. NULL when off or unsupported
    // leaves the chain untouched and today's exact behaviour.
    const void* sharpenChain = NULL;
    if (ctx->layerSettingsSupported && ctx->sharpenMode != 0) {
        memset(&sharpenSettings, 0, sizeof(sharpenSettings));
        sharpenSettings.type = XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB;
        sharpenSettings.layerFlags = ctx->sharpenMode == 2
                ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB
                : XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
        sharpenChain = &sharpenSettings;
    }

    // The environment, whichever of the two it is. The 3d room takes the
    // photo's place rather than sitting in front of it, and passthrough wants
    // the real room instead, so no two of the three ever go up together.
    if (roomOn && ctx->roomRendered && ctx->roomViewsValid && !ctx->passthrough) {
        memset(&roomLayer, 0, sizeof(roomLayer));
        memset(roomProjViews, 0, sizeof(roomProjViews));
        roomLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        roomLayer.layerFlags = 0;
        // World locked like the photo it stands in for, even when the screen
        // is head locked
        roomLayer.space = ctx->localSpace;
        roomLayer.viewCount = ROOM_EYES;
        roomLayer.views = roomProjViews;
        for (int eye = 0; eye < ROOM_EYES; eye++) {
            roomProjViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            // The poses the image was actually drawn from, so the compositor
            // reprojects it rather than being told a pose it does not match
            roomProjViews[eye].pose = ctx->roomViews[eye].pose;
            roomProjViews[eye].fov = ctx->roomViews[eye].fov;
            roomProjViews[eye].subImage.swapchain = ctx->roomSwapchain;
            roomProjViews[eye].subImage.imageRect.offset.x = eye * ctx->roomEyeWidth;
            roomProjViews[eye].subImage.imageRect.offset.y = 0;
            roomProjViews[eye].subImage.imageRect.extent.width = ctx->roomEyeWidth;
            roomProjViews[eye].subImage.imageRect.extent.height = ctx->roomEyeHeight;
            roomProjViews[eye].subImage.imageArrayIndex = 0;
        }
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&roomLayer;
    }

    // The photo, in that same slot: submitted before everything else so all of
    // it sits in front, and skipped when the room or passthrough has the slot.
    if (ctx->backgroundReady && ctx->backgroundEnabled && !ctx->passthrough && !roomOn) {
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

    // The glow, over the environment and under the picture. Deliberately not
    // sharpened: being soft is the whole of the effect.
    int glowOn;
    float glowLevel;
    ambiEffective(ctx, &glowOn, &glowLevel);
    if (glowOn && ctx->glowRendered && ctx->everRendered && ctx->shouldRender) {
        memset(&glowLayer, 0, sizeof(glowLayer));
        glowLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        glowLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        glowLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        glowLayer.subImage.swapchain = ctx->glowSwapchain;
        glowLayer.subImage.imageRect.offset.x = 0;
        glowLayer.subImage.imageRect.offset.y = 0;
        glowLayer.subImage.imageRect.extent.width = GLOW_TEX;
        glowLayer.subImage.imageRect.extent.height = GLOW_TEX;
        glowLayer.subImage.imageArrayIndex = 0;
        glowLayer.space = space;
        glowLayer.pose.orientation = screenPose.orientation;
        // Local +z is behind the picture, the same direction the cylinder puts
        // its axis. Far enough back that the two never z fight, near enough
        // that the glow reads as coming off the screen.
        Vec3 behindLocal = { 0.0f, 0.0f, GLOW_BEHIND_M };
        Vec3 behind = quatRotate(screenPose.orientation, behindLocal);
        glowLayer.pose.position.x = screenPose.position.x + behind.x;
        glowLayer.pose.position.y = screenPose.position.y + behind.y;
        glowLayer.pose.position.z = screenPose.position.z + behind.z;
        glowLayer.size.width = screenWidth * GLOW_SCALE;
        glowLayer.size.height = screenHeight * GLOW_SCALE;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&glowLayer;
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

            if (curve > 0.01f && ctx->cylinderSupported) {
                XrCompositionLayerCylinderKHR* cyl = &cylLayers[eye];
                memset(cyl, 0, sizeof(*cyl));
                cyl->type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
                cyl->next = sharpenChain;
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
                quad->next = sharpenChain;
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
        // distance, both eyes, so they read at screen depth with no disparity.
        // Visibility is the gate rather than the content: switching them off
        // leaves the last text sitting in the swapchain, since nothing comes
        // along to overwrite it.
        if (ctx->overlayHasContent && ctx->overlayVisible
                && ctx->overlaySwapchain != XR_NULL_HANDLE) {
            float overlayW = screenWidth * 0.30f;
            float overlayH = overlayW * (float)OVERLAY_HEIGHT / (float)OVERLAY_WIDTH;
            float margin = screenWidth * 0.02f;

            memset(&overlayLayer, 0, sizeof(overlayLayer));
            overlayLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            overlayLayer.next = sharpenChain;
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

        // The bar and the two buttons beside it share a hover area, so reaching
        // for one keeps the others on screen rather than swapping them. A modal
        // takes it away on the very frame it opens: the hover is still on the
        // button that was pressed, so the furniture and the panel would both go
        // up for one frame, and that stack overflowed the runtime's layer limit
        // and cost the whole frame with a -24 on device.
        int barArea = !ctx->pickerOpen && !ctx->cogOpen && !ctx->kbOpen
                && (ctx->hoverKind == HOVER_BAR || ctx->hoverKind == HOVER_ENVBUTTON
                    || ctx->hoverKind == HOVER_COGBUTTON
                    || ctx->hoverKind == HOVER_KBBUTTON);

        // Move bar and resize corner, shown only while the ray is over them.
        // Both live in the screen's own frame, so they travel with it. Neither
        // goes up in a room, where the wall holds the picture and there is
        // nothing for either to move. The buttons beside the bar still come up
        // on the same hover.
        if (ctx->handleArtReady && !roomOn && (barArea || ctx->hoverKind == HOVER_CORNER)) {
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

        // The cog that opens the settings panel, right of the move bar. Same
        // rules as the environment button on the other side.
        if (ctx->cogButtonReady && (barArea || ctx->cogOpen)) {
            Vec3 local;
            float side;
            cogButtonPlacement(ctx, screenHeight, &local, &side);
            Vec3 offset = quatRotate(screenPose.orientation, local);

            memset(&cogButtonLayer, 0, sizeof(cogButtonLayer));
            cogButtonLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            cogButtonLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            cogButtonLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cogButtonLayer.subImage.swapchain = ctx->cogButtonSwapchain;
            cogButtonLayer.subImage.imageRect.offset.x = 0;
            cogButtonLayer.subImage.imageRect.offset.y = 0;
            cogButtonLayer.subImage.imageRect.extent.width = OUTLINE_TEX;
            cogButtonLayer.subImage.imageRect.extent.height = OUTLINE_TEX;
            cogButtonLayer.subImage.imageArrayIndex = 0;
            cogButtonLayer.space = space;
            cogButtonLayer.pose.orientation = screenPose.orientation;
            cogButtonLayer.pose.position.x = screenPose.position.x + offset.x;
            cogButtonLayer.pose.position.y = screenPose.position.y + offset.y;
            cogButtonLayer.pose.position.z = screenPose.position.z + offset.z;
            float cogScale = (ctx->cogButtonHot || ctx->cogOpen) ? 1.18f : 1.0f;
            cogButtonLayer.size.width = side * cogScale;
            cogButtonLayer.size.height = side * cogScale;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&cogButtonLayer;
        }

        // The keyboard button, one place further out. Unlike the cog it goes
        // away while its panel is up, since the panel covers the bar anyway and
        // the hide key is what puts it away.
        if (ctx->kbButtonReady && barArea) {
            Vec3 local;
            float side;
            kbButtonPlacement(ctx, screenHeight, &local, &side);
            Vec3 offset = quatRotate(screenPose.orientation, local);

            memset(&kbButtonLayer, 0, sizeof(kbButtonLayer));
            kbButtonLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            kbButtonLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            kbButtonLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            kbButtonLayer.subImage.swapchain = ctx->kbButtonSwapchain;
            kbButtonLayer.subImage.imageRect.offset.x = 0;
            kbButtonLayer.subImage.imageRect.offset.y = 0;
            kbButtonLayer.subImage.imageRect.extent.width = OUTLINE_TEX;
            kbButtonLayer.subImage.imageRect.extent.height = OUTLINE_TEX;
            kbButtonLayer.subImage.imageArrayIndex = 0;
            kbButtonLayer.space = space;
            kbButtonLayer.pose.orientation = screenPose.orientation;
            kbButtonLayer.pose.position.x = screenPose.position.x + offset.x;
            kbButtonLayer.pose.position.y = screenPose.position.y + offset.y;
            kbButtonLayer.pose.position.z = screenPose.position.z + offset.z;
            float kbScale = ctx->kbButtonHot ? 1.18f : 1.0f;
            kbButtonLayer.size.width = side * kbScale;
            kbButtonLayer.size.height = side * kbScale;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&kbButtonLayer;
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
            pickerLayer.next = sharpenChain;
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
                float cellH = pickH * (float)PICKER_CELL_PX / (float)PICKER_TEX_H;
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
                    // Down the texture past this band's header to the middle
                    // of its row of cells
                    float centreV = (row * PICKER_BAND_PX + PICKER_HEADER_PX
                                     + PICKER_CELL_PX * 0.5f) / (float)PICKER_TEX_H;
                    Vec3 local;
                    local.x = ((col + 0.5f) / PICKER_COLS - 0.5f) * pickW;
                    local.y = (0.5f - centreV) * pickH;
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

        // The settings panel, at the pose it was opened with. The tab is a
        // choice of swapchain, all were filled at startup, and in a room the
        // screen tab picks its own sheet. Sharpened: it carries text.
        int cogArt = cogScreenLocked(ctx) ? COG_ART_ROOM_SCREEN : ctx->cogTab;
        if (ctx->cogOpen && ctx->cogPanelReady[cogArt]) {
            memset(&cogPanelLayer, 0, sizeof(cogPanelLayer));
            cogPanelLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            cogPanelLayer.next = sharpenChain;
            cogPanelLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            cogPanelLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            cogPanelLayer.subImage.swapchain = ctx->cogPanelSwapchains[cogArt];
            cogPanelLayer.subImage.imageRect.offset.x = 0;
            cogPanelLayer.subImage.imageRect.offset.y = 0;
            cogPanelLayer.subImage.imageRect.extent.width = COG_TEX_W;
            cogPanelLayer.subImage.imageRect.extent.height = COG_TEX_H;
            cogPanelLayer.subImage.imageArrayIndex = 0;
            cogPanelLayer.space = space;
            cogPanelLayer.pose = ctx->cogPose;
            cogPanelLayer.size.width = ctx->cogW;
            cogPanelLayer.size.height = ctx->cogH;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&cogPanelLayer;

            // Display tab. The cells are drawn into the texture, so what is
            // chosen and what is under the ray are rings over them, the same
            // trick the picker uses to mark cells without an upload. One per
            // row for the choice, then a wider one for the hover.
            if (ctx->cogTab == COG_TAB_DISPLAY && ctx->outlineReady) {
                for (int m = 0; m <= COG_OPTION_COUNT; m++) {
                    int hoverMark = m == COG_OPTION_COUNT;
                    int option = hoverMark ? ctx->cogHoverSlider : m;
                    int cell = hoverMark ? ctx->cogHoverCell : cogOptionValue(ctx, m);
                    if (option < 0 || option >= COG_OPTION_COUNT || cell < 0) {
                        continue;
                    }
                    float scale = hoverMark ? 1.12f : 1.0f;

                    int count = cogOptionCells(option);
                    float span = (COG_TRACK_R - COG_TRACK_L) / count;
                    Vec3 local;
                    local.x = (COG_TRACK_L + (cell + 0.5f) * span - 0.5f) * ctx->cogW;
                    local.y = (0.5f - (COG_ROW_V0 + option * COG_ROW_STEP)) * ctx->cogH;
                    local.z = 0.004f;
                    Vec3 offset = quatRotate(ctx->cogPose.orientation, local);

                    XrCompositionLayerQuad* mark = &cogMarkLayers[m];
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
                    mark->pose.orientation = ctx->cogPose.orientation;
                    mark->pose.position.x = ctx->cogPose.position.x + offset.x;
                    mark->pose.position.y = ctx->cogPose.position.y + offset.y;
                    mark->pose.position.z = ctx->cogPose.position.z + offset.z;
                    mark->size.width = span * ctx->cogW * scale;
                    mark->size.height = 2.0f * COG_CELL_HALF * ctx->cogH * scale;
                    layers[layerCount++] = (const XrCompositionLayerBaseHeader*)mark;
                }
            }

            // Nothing to drag on the sheet the room shows, so no thumbs go
            // over it either
            if (ctx->cogThumbReady && !cogScreenLocked(ctx)) {
                float thumbSize = ctx->cogH * 0.085f;
                int rowCount = cogTabRowCount(ctx->cogTab);
                for (int s = 0; s < rowCount; s++) {
                    // No thumb on a row that cannot be dragged
                    if (ctx->cogTab == COG_TAB_SCREEN && s == COG_SLIDER_CURVE
                            && !ctx->cylinderSupported) {
                        continue;
                    }
                    if (ctx->cogTab == COG_TAB_3D && ctx->stereoMode == DEPTH_MODE_OFF) {
                        continue;
                    }
                    // Which on the display tab is every row but the level one,
                    // since the rest are cells with rings over them
                    if (ctx->cogTab == COG_TAB_DISPLAY && s != COG_DISPLAY_SLIDER_ROW) {
                        continue;
                    }
                    float t = cogSliderValue(ctx, ctx->cogTab, s);
                    Vec3 local;
                    local.x = (COG_TRACK_L + t * (COG_TRACK_R - COG_TRACK_L) - 0.5f) * ctx->cogW;
                    local.y = (0.5f - (COG_ROW_V0 + s * COG_ROW_STEP)) * ctx->cogH;
                    local.z = 0.004f;
                    Vec3 offset = quatRotate(ctx->cogPose.orientation, local);

                    XrCompositionLayerQuad* thumb = &cogThumbLayers[s];
                    memset(thumb, 0, sizeof(*thumb));
                    thumb->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                    thumb->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    thumb->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    thumb->subImage.swapchain = ctx->cogThumbSwapchain;
                    thumb->subImage.imageRect.offset.x = 0;
                    thumb->subImage.imageRect.offset.y = 0;
                    thumb->subImage.imageRect.extent.width = COG_THUMB_TEX;
                    thumb->subImage.imageRect.extent.height = COG_THUMB_TEX;
                    thumb->subImage.imageArrayIndex = 0;
                    thumb->space = space;
                    thumb->pose.orientation = ctx->cogPose.orientation;
                    thumb->pose.position.x = ctx->cogPose.position.x + offset.x;
                    thumb->pose.position.y = ctx->cogPose.position.y + offset.y;
                    thumb->pose.position.z = ctx->cogPose.position.z + offset.z;
                    // Grows under the ray, the same feedback the buttons give
                    float grow = (ctx->cogHoverSlider == s || ctx->cogDragSlider == s)
                            ? 1.25f : 1.0f;
                    thumb->size.width = thumbSize * grow;
                    thumb->size.height = thumbSize * grow;
                    layers[layerCount++] = (const XrCompositionLayerBaseHeader*)thumb;
                }
            }
        }

        // The keyboard, at the pose it was opened with, with the key under the
        // ray ringed. Sharpened, since it is all text. It stands down while a
        // modal is up rather than stacking under one: the two together would
        // crowd the runtime's layer ceiling, and the modal has the ray anyway.
        if (ctx->kbOpen && !ctx->pickerOpen && !ctx->cogOpen
                && ctx->kbPanelReady[ctx->kbState]) {
            memset(&kbPanelLayer, 0, sizeof(kbPanelLayer));
            kbPanelLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            kbPanelLayer.next = sharpenChain;
            kbPanelLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            kbPanelLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            kbPanelLayer.subImage.swapchain = ctx->kbPanelSwapchains[ctx->kbState];
            kbPanelLayer.subImage.imageRect.offset.x = 0;
            kbPanelLayer.subImage.imageRect.offset.y = 0;
            kbPanelLayer.subImage.imageRect.extent.width = KB_TEX_W;
            kbPanelLayer.subImage.imageRect.extent.height = KB_TEX_H;
            kbPanelLayer.subImage.imageArrayIndex = 0;
            kbPanelLayer.space = space;
            kbPanelLayer.pose = ctx->kbPose;
            kbPanelLayer.size.width = ctx->kbW;
            kbPanelLayer.size.height = ctx->kbH;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&kbPanelLayer;

            if (ctx->outlineReady && ctx->kbHoverKey >= 0
                    && ctx->kbHoverKey < ctx->kbKeyCount) {
                const float* r = &ctx->kbKeyRects[ctx->kbHoverKey * 4];
                Vec3 local;
                local.x = ((r[0] + r[2]) * 0.5f - 0.5f) * ctx->kbW;
                local.y = (0.5f - (r[1] + r[3]) * 0.5f) * ctx->kbH;
                local.z = 0.004f;
                Vec3 offset = quatRotate(ctx->kbPose.orientation, local);

                memset(&kbMarkLayer, 0, sizeof(kbMarkLayer));
                kbMarkLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                kbMarkLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                kbMarkLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                kbMarkLayer.subImage.swapchain = ctx->outlineSwapchain;
                kbMarkLayer.subImage.imageRect.offset.x = 0;
                kbMarkLayer.subImage.imageRect.offset.y = 0;
                kbMarkLayer.subImage.imageRect.extent.width = OUTLINE_TEX;
                kbMarkLayer.subImage.imageRect.extent.height = OUTLINE_TEX;
                kbMarkLayer.subImage.imageArrayIndex = 0;
                kbMarkLayer.space = space;
                kbMarkLayer.pose.orientation = ctx->kbPose.orientation;
                kbMarkLayer.pose.position.x = ctx->kbPose.position.x + offset.x;
                kbMarkLayer.pose.position.y = ctx->kbPose.position.y + offset.y;
                kbMarkLayer.pose.position.z = ctx->kbPose.position.z + offset.z;
                // Swells while the trigger is held, which is the only press
                // feedback a quad layer can give
                float grow = ctx->kbKeyDown ? 1.12f : 1.0f;
                kbMarkLayer.size.width = (r[2] - r[0]) * ctx->kbW * grow;
                kbMarkLayer.size.height = (r[3] - r[1]) * ctx->kbH * grow;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&kbMarkLayer;
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

    // Said once and only once, since a frame that crowds the limit is usually
    // every frame after it. Nothing is dropped here: a missing layer is a
    // silent bug, where the count in the log points straight at the culprit.
    if (layerCount >= (uint32_t)ctx->maxLayerCount && !ctx->layerLimitWarned) {
        ctx->layerLimitWarned = 1;
        LOGW("submitted %u composition layers against a limit of %d",
             layerCount, ctx->maxLayerCount);
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
