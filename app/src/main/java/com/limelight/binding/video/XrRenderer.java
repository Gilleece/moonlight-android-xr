package com.limelight.binding.video;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Process;
import android.preference.PreferenceManager;
import android.text.TextUtils;
import android.view.Surface;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.binding.input.EyeTrackingPermission;
import com.limelight.binding.input.XrPad;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.preferences.XrDisplayRates;
import com.limelight.utils.BugReport;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import static com.limelight.binding.video.XrShared.*;

/**
 * Presents the decoded stream in an OpenXR session. Same input contract as
 * GlPassthroughRenderer: the decoder renders into our SurfaceTexture, and we
 * consume it from the frame loop thread. All OpenXR work happens in native
 * code, this class owns the thread and the SurfaceTexture plumbing.
 */
public class XrRenderer implements SurfaceTexture.OnFrameAvailableListener {

    static {
        System.loadLibrary("xr-renderer");
    }

    // Averaged over this many inferences before hitting logcat
    private static final int DEPTH_STATS_INTERVAL = 30;
    // Far longer than any gap between two maps of a running stream, so a gap
    // this long is a stall or the headset off the head, and stays out of the
    // period
    private static final long DEPTH_PERIOD_GAP_NS = 1000000000L;
    private static final int DEPTH_AGE_INTERVAL = 300;

    private static final float OVERLAY_TEXT_SIZE = 22.0f;
    private static final float OVERLAY_LINE_HEIGHT = 28.0f;

    // Written by the frame loop thread and read by whichever thread reports the
    // stats, so the write has to be visible across them
    private volatile long nativeCtx;
    // Held around every native call made off the frame loop, and by the frame
    // loop while it frees the context, so no thread can reach a context that
    // is halfway through being destroyed
    private final Object nativeLock = new Object();
    private Thread renderThread;
    private Thread depthThread;
    private Thread depthStageThread;
    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;
    // The frame loop reads the SurfaceTexture every frame, so it cannot be
    // released out from under it. If cleanup arrives while the loop is still
    // running it leaves a note instead, and the loop releases both on its way
    // out.
    private final Object teardownLock = new Object();
    private boolean renderThreadDone;
    private boolean releaseOnExit;

    private final AtomicInteger pendingFrames = new AtomicInteger(0);
    private final float[] texMatrix = new float[16];
    private volatile boolean stopping;
    // Set once the session has been focused, which is when a launch is through
    private volatile boolean focusedOnce;
    // The session gone to stopping or idle after that, as the listener was
    // last told. Frame loop only.
    private boolean sessionAway;
    private long videoFrameIndex;

    // The depth pipeline. Each capture travels in one of DEPTH_PAIRS pairs of
    // native staging: the frame loop reads it back, the stage thread turns it
    // into model input, the depth thread runs the model, and the stage thread
    // uploads the map (DepthPairs). With two pairs the next frame is read
    // back while the model runs and the last map goes up while the next one
    // runs, so the model is the only stage a map waits on. The frame loop
    // never waits: with no pair free, or a capture already waiting, it skips
    // the frame, so depth runs at whatever rate the model manages. All of it
    // under depthLock, which only the two depth threads ever wait on.
    private final Object depthLock = new Object();
    private final DepthPairs pairs = new DepthPairs(DEPTH_PAIRS);
    // What each stage of a pair's trip cost, when the stage thread started on
    // it, and the frame it came from, each written by the thread doing that
    // stage before it hands the pair on under the lock
    private final long[] pairCaptureNs = new long[DEPTH_PAIRS];
    private final long[] pairFinishNs = new long[DEPTH_PAIRS];
    private final long[] pairStartNs = new long[DEPTH_PAIRS];
    private final long[] pairInferenceNs = new long[DEPTH_PAIRS];
    private final long[] pairFrameIndex = new long[DEPTH_PAIRS];
    private final long[] pairFrameNs = new long[DEPTH_PAIRS];
    private boolean depthExit;
    private int skippedFrames;
    private volatile boolean depthReady;

    // How far behind the picture the depth map is. The map warping a frame was
    // computed from an earlier one, and then reused until the next inference
    // lands, so during camera motion it is spatially offset from the colour it
    // is warping. Measured rather than assumed: these are the frame index and
    // clock reading of the frame the live depth map came from.
    private volatile long publishedFrameIndex;
    private volatile long publishedFrameNs;

    // Stats overlay. Text is drawn to a bitmap on whichever thread reports the
    // stats, then handed to the frame loop, which owns the GL context. Two
    // buffers so the drawing side never writes one the renderer is reading.
    private final AtomicReference<ByteBuffer> pendingOverlay = new AtomicReference<>();
    private ByteBuffer[] overlayBuffers;
    private int overlayBufferIndex;
    private Bitmap overlayBitmap;
    private Canvas overlayCanvas;
    private Paint overlayPaint;
    private volatile float lastInferenceMs;
    // Which model, at what size, on which runtime, for the overlay
    private volatile String depthLabel = "";
    private volatile float lastDepthAgeMs;
    private volatile int lastDepthSkips;
    // Maps a second as the last Depth stage line measured them
    private volatile float lastMapsPerSecond;
    // Whether the 3D is on, as the frame before said. The bar and the 3D tab
    // can switch it off for the rest of the session, and while it is off the
    // model is not fed, so it sits idle until it comes back on.
    private volatile boolean stereoLive = true;
    // The eye tracking permission is not refused, or not the platform's to
    // grant. Look to point only works with it.
    private volatile boolean gazeAllowed = true;
    // Where the start stopped, if it did
    private volatile XrStartFailure startFailure;

    // Controller pointer. The native side does the ray maths and hands back a
    // hit point and a button mask, this side turns that into host events. The
    // slots in that array and the ids the panel reports are the IN_ and
    // SETTING_ values in XrShared, so both sides read them off the same file.
    private final float[] inputState = new float[IN_SLOTS];
    private int heldButtons;
    // Gamepad mode's pad as the listener was last told it: plugged in or not,
    // then its buttons, triggers and sticks in the IN_PAD_ order. Frame loop
    // only.
    private boolean padPlugged;
    private final int[] padSent = new int[IN_PAD_RY - IN_PAD_BUTTONS + 1];
    // The host's rumble on that pad as the last word said it, written from
    // the connection's thread, and the word last handed down, frame loop only
    private final Object rumbleLock = new Object();
    private long rumbleCount;
    private volatile long rumbleWord;
    private long rumbleHanded;
    // The head's yaw against the screen, for the virtual surround. Written by
    // the frame loop and read by the audio thread once a block.
    private volatile float headYaw;
    private InputListener inputListener;
    private volatile SessionListener sessionListener;
    private Context prefsContext;
    private PreferenceConfiguration prefConfig;

    // The baked rooms that ship with the app, a mesh and its atlases each,
    // named by the picker cell that shows them in roomMeshFile and
    // roomAtlasFiles below
    private static final String ROOM_DIR = "rooms";
    // The controller drawn at each hand when the setting is on, baked the way
    // the rooms are and painted from its vertex colours
    private static final String CONTROLLER_MODEL = "models/controller.room";

    // Panel art on its way to the GPU. XrPanels draws it on the loader thread
    // and it waits here for the frame loop, which owns the GL context. The
    // splash is the exception, drawn before the frame loop starts so it is
    // up from the first frame.
    private final AtomicReference<ByteBuffer> pendingSplash = new AtomicReference<>();
    private final AtomicReference<ByteBuffer[]> pendingKbSheets = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbButton = new AtomicReference<>();
    // Built next to the art and read on the frame loop when it uploads
    private volatile float[] kbKeyRects;
    private volatile int[][] kbCodes;
    // The modifiers each keyboard sheet was last drawn with lit, and those the
    // host has been told are held. Frame loop only, and the sheets are only
    // drawn again once the first set is up.
    private final int[] kbSheetMods = new int[KB_STATE_COUNT];
    private boolean kbArtUp;
    private int heldKbMods;
    // One sheet at a time is drawn again off the frame loop, and waits here
    private final AtomicReference<KbSheet> pendingKbSheet = new AtomicReference<>();
    private boolean kbSheetDrawing;

    // The report sheet: what its fields hold, which opening of it is up, and
    // what was last drawn and handed down, all frame loop only. It is drawn
    // again on a thread of its own whenever what it shows changes, one
    // drawing at a time, and goes up from the frame loop once done if it is
    // still for the opening that is up.
    private final XrReportForm reportForm = new XrReportForm();
    private volatile XrPanels.ReportSheet reportArt;
    private int reportOpening;
    private String reportDrawn;
    private boolean reportDrawing;
    private boolean reportSendHanded;
    private final AtomicReference<ReportDrawing> pendingReport = new AtomicReference<>();

    private final AtomicReference<ByteBuffer> pendingExitButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitPlain = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitHot = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCancelHot = new AtomicReference<>();

    private final AtomicReference<ByteBuffer> pendingPickerArt = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingEnvButton = new AtomicReference<>();
    // Every sheet of the settings panel, in COG_ART_ order
    private final AtomicReference<ByteBuffer[]> pendingCogSheets = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogButton = new AtomicReference<>();
    // The values beside the Room or Picture tab's tracks, drawn on the frame
    // loop when the frame says one has moved, and the values last drawn
    private XrPanels.Readout cogReadout;
    private final int[] readoutDrawn = new int[READOUT_VALUES];
    private final int[] readoutWanted = new int[READOUT_VALUES];
    // The toast's sheet, drawn on the frame loop when a notice goes up, and
    // the words of the notices this side raises, each a line and the one
    // under it: queued from any thread, then kept in slots the native side
    // names them by
    private XrPanels.Toast toast;
    // The marks on the display tab's cells, drawn on the frame loop when one
    // moves, and the cells they were last drawn for
    private XrPanels.Marks cogMarks;
    // The time and the battery, for the first line of the stats and the strip
    // over the settings panel, and what the strip last said
    private volatile XrClock clock;
    private XrPanels.ClockStrip cogClock;
    private String cogClockDrawn;
    private final int[] marksDrawn = new int[MARK_VALUES];
    private final int[] marksWanted = new int[MARK_VALUES];
    // The tick a press on the panels makes, built once off the frame loop and
    // fed from it, whether one is on its way, and whether the session is over,
    // which a late one is let go for. Handed over under the lock.
    private final Object clickLock = new Object();
    private volatile XrClickSound clickSound;
    private boolean clickSoundStarting;
    private boolean clickSessionOver;
    private final ConcurrentLinkedQueue<NoticeWords> pendingNotices = new ConcurrentLinkedQueue<>();
    private final NoticeWords[] noticeTexts = new NoticeWords[TOAST_TEXT_SLOTS];
    private int noticeSlot;
    // The hand lock hint's sheet, only drawn in a session that may show it
    private final AtomicReference<ByteBuffer> pendingHandHint = new AtomicReference<>();
    // The Ko-fi sheet the About tab opens, in every session
    private final AtomicReference<ByteBuffer> pendingKofi = new AtomicReference<>();
    // The 3D switch's two faces, only drawn in a session with stereo to switch
    private final AtomicReference<ByteBuffer> pendingStereoOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingStereoOn = new AtomicReference<>();
    // The ray's switch's two faces, drawn in every session
    private final AtomicReference<ByteBuffer> pendingRayOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingRayOn = new AtomicReference<>();
    // Head aim's switch's two faces, the same
    private final AtomicReference<ByteBuffer> pendingAimOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingAimOn = new AtomicReference<>();
    // And gamepad mode's, pointer and gamepad
    private final AtomicReference<ByteBuffer> pendingPadOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingPadOn = new AtomicReference<>();
    // A baked room on its way to the GPU, read off the frame loop like the art
    // above. The native side shows the void in its place until it has landed.
    // The mesh, the atlases and the cell they belong to travel as one, so a
    // room picked while another is being read can never leave the native side
    // with half of each.
    private static final class RoomAssets {
        final int cell;
        final ByteBuffer mesh;
        final int meshBytes;
        // Whole .atlas files, which go up as they were read, in the slot order
        // the mesh's parts name them by
        final ByteBuffer[] atlases;

        RoomAssets(int cell, ByteBuffer mesh, ByteBuffer[] atlases) {
            this.cell = cell;
            this.mesh = mesh;
            this.meshBytes = mesh.remaining();
            this.atlases = atlases;
        }
    }

    private final AtomicReference<RoomAssets> pendingRoom = new AtomicReference<>();
    // The controller model, read once a session off the frame loop, small
    // enough to read whether or not it is shown so the Display tab's row can
    // bring it up at once
    private final AtomicReference<ByteBuffer> pendingControllerModel = new AtomicReference<>();
    // Only the room on screen is resident. Every pick takes a new ticket, so a
    // room still being read for an earlier pick is never parked over the one
    // chosen since, and the cell last parked is not read again while it stands.
    // Both only under roomLock, which is what makes the check and the park one
    // step against a pick.
    private final Object roomLock = new Object();
    private int roomTicket;
    private int parkedRoomCell = -1;
    private XrPanels panels;
    private volatile int environmentChoice = ENV_CELL_VOID;
    private volatile boolean passthroughOn;

    /**
     * Pointer events out of the VR session. Called on the frame loop thread.
     * Buttons are 0 left, 1 right, 2 middle.
     */
    public interface InputListener {
        void onVrPointerMove(float u, float v);
        // Relative mouse motion in host pixels, right and down positive: the
        // head's turn while head aim is on, with the pointer's nudges added,
        // since in that mode nothing moves the mouse to a position
        void onVrMouseMove(int dx, int dy);
        void onVrButton(int button, boolean down);
        void onVrScroll(int clicks);
        // Gamepad mode's pad, the two controllers as one Xbox pad: plugged in
        // or out on the host, then each time it changes what it reads, PAD_
        // button bits, triggers 0 to 255 and sticks -32766 to 32766, up
        // positive. Always plugged in before its first state and let go of
        // before it comes out.
        void onVrGamepadPlugged(boolean plugged);
        void onVrGamepadState(int buttons, int leftTrigger, int rightTrigger,
                              int leftX, int leftY, int rightX, int rightY);
        // A key from the in world keyboard. Unicode with the shift already
        // applied, backspace, tab, enter and space as their control codes, or
        // a virtual key code over KB_CODE_VK. The modifiers, KB_MOD_ bits, are
        // the ones held down on the host while it goes.
        void onVrKey(int code, int modifiers);
        // Ctrl, Alt and Win as lit on the keyboard now, KB_MOD_ bits, which
        // are to be held down on the host until they go out
        void onVrModifiers(int modifiers);
        // The exit prompt was confirmed, so the session is to end
        void onVrExit();
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    // Told when the runtime ends the session under the stream
    public void setSessionListener(SessionListener listener) {
        this.sessionListener = listener;
    }

    /**
     * Whether the eyes may point, which they may not while the eye tracking
     * permission is refused. Read fresh each frame, so an answer that arrives
     * mid session takes effect on the next. Any thread.
     */
    public void setGazeAllowed(boolean allowed) {
        gazeAllowed = allowed;
    }

    /**
     * How far the head has turned from the screen, in radians, positive to
     * the left, as of the last frame. 0 with the screen locked to the head.
     * Any thread.
     */
    public float getHeadYaw() {
        return headYaw;
    }

    /**
     * The host's rumble for gamepad mode's pad, the two motors as they came:
     * the low one on the left controller, the high one on the right, until
     * the host says otherwise or the pad comes out. Any thread; the frame
     * loop picks it up.
     */
    public void setRumble(short lowFreqMotor, short highFreqMotor) {
        synchronized (rumbleLock) {
            rumbleCount++;
            rumbleWord = XrPad.rumbleWord(rumbleCount, lowFreqMotor, highFreqMotor);
        }
    }

    /**
     * Whether the session has been focused yet. Until it has, the runtime may
     * still be holding the launch, behind a boundary prompt for one, and the
     * activity can be stopped meanwhile without the user having left. Any
     * thread.
     */
    public boolean hasBeenFocused() {
        return focusedOnce;
    }

    /**
     * Says something on the toast inside the session, where a 2d toast or
     * dialog is never seen: a line, and a quieter one under it or null. Any
     * thread; it goes up once the frame loop next comes round.
     */
    public void showNotice(String text, String more) {
        showNotice(text, more, false);
    }

    // The same, where the second line is a path: cut from the middle when it
    // will not fit, so the folder it starts in and the file both stay in view
    private void showNotice(String text, String more, boolean path) {
        if (text != null) {
            pendingNotices.add(new NoticeWords(text, more, path));
        }
    }

    // A notice of this side's own, as it waits for its turn
    private static final class NoticeWords {
        final String text;
        final String more;
        final boolean path;

        NoticeWords(String text, String more, boolean path) {
            this.text = text;
            this.more = more;
            this.path = path;
        }
    }

    /**
     * Told when a VR session could not be started at all, so the activity can
     * do something visible about it rather than stream into a window the
     * headset's shell never shows. Called off the main thread.
     */
    public interface SessionListener {
        // failure says where the start stopped, never null
        void onVrUnavailable(XrStartFailure failure);

        // The runtime ended a running session (quit from the system menu, the
        // session lost, a frame call failing), so nothing will show the
        // stream again. Once at most, from the render thread, and never for
        // a stop the activity asked for. reason is for the log.
        void onVrSessionEnded(String reason);

        // After the first focus the session went to stopping or idle, which
        // is what a removed headset does, and later came back to focused.
        // Once each way, in turn, from the render thread.
        default void onVrSessionAway() {
        }

        default void onVrSessionBack() {
        }
    }

    // How long a start is waited for before it counts as failed
    private static final int INIT_WAIT_SECONDS = 5;

    // The page the Ko-fi sheet's code points at, which the sheet writes out
    // beside it as well
    static final String SUPPORT_URL = "https://ko-fi.com/moonlightxr";

    private static native void nativeSetFileLog(String path, int level);
    // Where the last start that failed stopped, as XrStartFailure reads it, or
    // null. Taken once.
    private static native String[] nativeTakeStartFailure();
    // envResTier is the EnvResTier the room renders at: 0 low, 1 standard,
    // 2 high, 3 ultra. fps is the stream's, which the display rate is matched to.
    private native long nativeInit(Activity activity, int width, int height, int fps,
                                   int stereoMode, int depthWidth, int depthHeight,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean handTracking, int sharpenMode, int supersampleMode,
                                   boolean perfOverlay,
                                   boolean ambilight, int ambiLevel, boolean roomLight,
                                   int envResTier);
    private native void nativeSetCaptureDir(long ctx, String dir);
    private native int nativeGetTexId(long ctx);
    private native ByteBuffer nativeGetModelInput(long ctx, int pair);
    private native ByteBuffer nativeGetModelOutput(long ctx, int pair);
    private native long nativeCaptureDepthInput(long ctx, float[] texMatrix, int pair);
    private native long nativeFinishDepthCapture(long ctx, int pair);
    private native long nativeUploadDepth(long ctx, int pair);
    private native void nativeDropDepth(long ctx, int pair);
    private native boolean nativeBindDepthContext(long ctx);
    private native void nativeUnbindDepthContext(long ctx);
    private native boolean nativeBindDepthStageContext(long ctx);
    private native void nativeUnbindDepthStageContext(long ctx);
    private native int nativeWaitBeginFrame(long ctx);
    private native void nativeEndFrame(long ctx, boolean newFrame, float[] texMatrix,
                                       float distance, float quadWidth, float curvature,
                                       boolean headLocked, float separation, boolean eyeSwap,
                                       boolean passthrough);
    private native void nativeUpdateInput(long ctx, float distance, float quadWidth,
                                          float curvature, boolean headLocked,
                                          boolean pointerEnabled, boolean gazeEnabled,
                                          boolean pointerSleep, float[] out);
    private native void nativeSetScreenPose(long ctx, float[] pose);
    // The room's assets name the picker cell they belong to, which the native
    // side turns into its own room style
    private native void nativeUploadRoomModel(long ctx, ByteBuffer mesh, int length, int cell);
    private native void nativeUploadRoomAtlas(long ctx, ByteBuffer atlas, int cell, int slot);
    // The controller model's .room file, whole
    private native void nativeUploadControllerModel(long ctx, ByteBuffer mesh, int length);
    // Whether the controller model is drawn, which the Display tab's row
    // then writes back
    private native void nativeSetControllerModel(long ctx, boolean on);
    private native void nativeUploadPicker(long ctx, ByteBuffer grid, ByteBuffer button, int cells);
    private native void nativeUploadCog(long ctx, ByteBuffer[] sheets, ByteBuffer button);
    private native void nativeUploadCogReadout(long ctx, ByteBuffer strip, int[] values);
    // One room's own Room tab values, by the picker cell that shows it
    private native void nativeSetRoomLevels(long ctx, int cell, int brightness, boolean glow,
                                            int light, int screen);
    // The running model's own separation and convergence, in the preferences'
    // units, which the 3D tab's reset goes back to, and the separations its
    // presets write, in cell order
    private native void nativeSetDepthDefaults(long ctx, int separation, int convergence,
                                               int[] presets);
    // Brightness, contrast, gamma and saturation over the picture, in the
    // PICTURE_ order and units
    private native void nativeSetPicture(long ctx, int[] picture);
    // The sheets and the code tables in KB_STATE_ order
    private native void nativeUploadKeyboard(long ctx, ByteBuffer[] sheets, ByteBuffer buttonIcon,
                                             float[] keyRects, int[][] codes);
    private native void nativeUploadKeyboardSheet(long ctx, int state, ByteBuffer sheet);
    private native void nativeUploadReport(long ctx, ByteBuffer sheet);
    private native void nativeSetReportSend(long ctx, boolean ready);
    private native void nativeUploadExit(long ctx, ByteBuffer button, ByteBuffer promptPlain,
                                         ByteBuffer promptExitHot, ByteBuffer promptCancelHot);
    private native boolean nativeGetCylinderSupported(long ctx);
    private native boolean nativeHasBeenFocused(long ctx);
    // A PRESENCE_ value, for a removed headset's hold
    private native int nativeGetPresence(long ctx);
    // Why the runtime ended the frame loop, or null
    private native String nativeGetExitReason(long ctx);
    private native String nativeGetRuntime(long ctx);
    private native void nativeUploadHandHint(long ctx, ByteBuffer sheet);
    private native void nativeUploadKofi(long ctx, ByteBuffer sheet);
    // Whether this session may show the hand lock hint, before the first frame
    private native void nativeSetHandHint(long ctx, boolean wanted);
    private native void nativeUploadStereoButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadRayButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadAimButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadPadButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeUploadSplash(long ctx, ByteBuffer sheet);
    // The toast's words for a notice just gone up, and a notice of this
    // side's own to be queued, a TOAST_TEXT under its slot
    private native void nativeUploadToast(long ctx, ByteBuffer sheet, int kind, int arg);
    private native void nativePushNotice(long ctx, int kind, int arg);
    private native void nativeUploadCogMarks(long ctx, ByteBuffer strip);
    private native void nativeUploadCogClock(long ctx, ByteBuffer strip);
    // Whether a press ticks, which the display tab's row reads back
    private native void nativeSetClickSound(long ctx, boolean on);
    // Whether a session starts with the ray drawn, which the bar's ray button
    // and the Display tab's row then switch for the session
    private native void nativeSetShowRay(long ctx, boolean on);
    // Whether a session starts with head aim on, its pixels a degree and its
    // dead zone in degrees a second
    private native void nativeSetHeadAim(long ctx, boolean on, int sensitivity, int deadZone);
    // Whether a session starts in gamepad mode, and the sticks' dead zone in
    // the whole percent the settings keep for a real pad
    private native void nativeSetGamepad(long ctx, int shortcut, int deadzonePercent);
    // The host's rumble on the pad, each motor 0 to 65535
    private native void nativeSetRumble(long ctx, int lowMotor, int highMotor);
    // The depth model will make no map this session, so the splash stops
    // waiting for one. Any thread.
    private native void nativeDepthGaveUp(long ctx);
    private native void nativeSetEnvironment(long ctx, int choice);
    private native void nativeUploadOverlay(long ctx, ByteBuffer pixels, int width, int height);
    private native float nativeGetWarpGpuMs(long ctx);
    // The rate the display is on, and the one the session last asked for, 0
    // when it has not asked
    private native float nativeGetDisplayRate(long ctx);
    private native float nativeGetAskedRate(long ctx);
    // The most depth maps a second the model runs at, which the governor
    // starts at and cuts from when the frame budget is missed
    private native void nativeSetDepthRate(long ctx, int perSecond);
    // Frame loop, with a new frame in hand: whether it is the one to capture
    // for the depth model. force takes it whatever the timing says.
    private native boolean nativeDepthDue(long ctx, boolean force);
    // What the governor has the model running at now, maps a second
    private native int nativeGetDepthTarget(long ctx);
    // Every rate the display offers, empty where the runtime does not say
    private native float[] nativeGetOfferedRates(long ctx);
    private native void nativeDestroy(long ctx);

    public boolean start(final Activity activity, final int videoWidth, final int videoHeight,
                         final PreferenceConfiguration prefs) {
        final CountDownLatch initLatch = new CountDownLatch(1);
        final boolean[] initOk = new boolean[1];

        renderThread = new Thread() {
            @Override
            public void run() {
                try {
                    runSession();
                } finally {
                    finishRenderThread();
                }
            }

            private void runSession() {
                // Submission has to land inside the compositor's frame window,
                // so this thread cannot sit behind the decoder or the depth
                // worker the way an unprioritised thread would. Thread's own
                // setPriority only changes the JVM's bookkeeping, not the
                // Linux scheduler, so the real call goes through Process.
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);

                // Before init, so everything the session setup finds ends up
                // in the log too
                nativeSetFileLog(FileLog.getLogPath(), FileLog.getLevel());

                // The depth staging is allocated at the input size of the
                // export this headset loads, so the route is settled here,
                // before any of it is built. The test patterns run at the
                // default model's size.
                MidasDepthSource.Spec depthSpec = MidasDepthSource.specFor(prefs.vrDepthModel);
                MidasDepthSource.Route depthRoute =
                        depthSpec.route(PreferenceConfiguration.isXr2Gen1Headset());
                DepthSize mapSize = depthRoute.size;
                nativeCtx = nativeInit(activity, videoWidth, videoHeight, prefs.fps,
                        prefs.vrDepthMode, mapSize.width, mapSize.height,
                        prefs.vrDepthDebug, prefs.vrConvergence, prefs.vrDepthScale,
                        prefs.vrHandTracking, prefs.vrSharpening, prefs.vrSupersampling,
                        prefs.enablePerfOverlay,
                        prefs.vrAmbilight, prefs.vrAmbilightLevel, prefs.vrRoomLight,
                        prefs.vrEnvResTier);
                if (nativeCtx == 0) {
                    initLatch.countDown();
                    return;
                }

                prefsContext = activity.getApplicationContext();
                gazeAllowed = EyeTrackingPermission.gazeAllowed(prefsContext);
                // Kept for a report, which can be made long after this session,
                // and a failed start before it no longer says how VR stands
                SharedPreferences.Editor kept =
                        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                                .remove(BugReport.START_FAILURE_PREF);
                String runtime = nativeGetRuntime(nativeCtx);
                if (runtime != null) {
                    kept.putString(BugReport.RUNTIME_PREF, runtime);
                }
                kept.apply();
                // For the frame rate list, which can only ask the Android
                // display otherwise
                XrDisplayRates.remember(prefsContext, nativeGetOfferedRates(nativeCtx));
                // Held on to rather than only read here: the stats toggle on
                // the panel writes back to this same instance, which is the one
                // the decoder's stats path checks
                prefConfig = prefs;
                nativeSetDepthDefaults(nativeCtx, depthSpec.defaultSeparation,
                        depthSpec.defaultConvergence,
                        DepthPresets.values(depthSpec.defaultSeparation));
                nativeSetPicture(nativeCtx, prefs.vrPicture);
                nativeSetDepthRate(nativeCtx, prefs.vrDepthRate);
                restoreScreenPose();
                startEnvironment(prefs);
                // A few milliseconds here, and the first frame has it
                pendingSplash.set(panels.buildSplash());

                File captureDir = activity.getExternalFilesDir(null);
                if (captureDir != null) {
                    nativeSetCaptureDir(nativeCtx, captureDir.getAbsolutePath());
                }

                // The EGL context is current on this thread now, so the
                // SurfaceTexture attaches to it here
                surfaceTexture = new SurfaceTexture(nativeGetTexId(nativeCtx));
                surfaceTexture.setDefaultBufferSize(videoWidth, videoHeight);
                surfaceTexture.setOnFrameAvailableListener(XrRenderer.this);
                inputSurface = new Surface(surfaceTexture);

                if (prefs.vrDepthMode == DEPTH_MODE_MODEL) {
                    FileLog.event("depth model "+depthSpec.name+", "+depthRoute.label()
                            +", map "+mapSize);
                    startDepthThread(activity, depthSpec, depthRoute);
                }

                initOk[0] = true;
                initLatch.countDown();

                // Read before the context goes, and only where the runtime
                // ended the loop rather than a stop asked for here
                String endedFor = null;
                if (runFrameLoop(prefs)) {
                    String reason = nativeGetExitReason(nativeCtx);
                    endedFor = reason != null ? reason : "unknown";
                }
                // The session is over, so the pad comes out with it
                unplugPad();

                stopDepthThread();
                XrClickSound click;
                synchronized (clickLock) {
                    clickSessionOver = true;
                    click = clickSound;
                    clickSound = null;
                }
                if (click != null) {
                    click.release();
                }

                // Tear down on the same thread that owns the GL context, and
                // under the lock so a stats report cannot land on a context
                // that is halfway through being freed. The SurfaceTexture
                // and Surface stay alive for the codec until cleanup().
                synchronized (nativeLock) {
                    long ctx = nativeCtx;
                    nativeCtx = 0;
                    nativeDestroy(ctx);
                }

                // With the session gone, so the activity's stop finds this
                // thread done rather than waiting on it
                if (endedFor != null) {
                    LimeLog.warning("VR session ended by the runtime: " + endedFor);
                    SessionListener listener = sessionListener;
                    if (listener != null) {
                        listener.onVrSessionEnded(endedFor);
                    }
                }
            }
        };
        renderThread.setName("Video - XR Renderer");
        renderThread.start();

        boolean initFinished;
        try {
            // Session setup can take a moment on a cold runtime
            initFinished = initLatch.await(INIT_WAIT_SECONDS, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            initFinished = false;
        }

        if (!initFinished || !initOk[0]) {
            LimeLog.severe("XR renderer init failed");
            XrStartFailure failure = initFinished
                    ? XrStartFailure.fromNative(nativeTakeStartFailure())
                    : XrStartFailure.timedOut(INIT_WAIT_SECONDS);
            keepStartFailure(activity, failure);
            startFailure = failure;
            prepareForStop();
            cleanup();
            return false;
        }

        LimeLog.info("XR renderer initialized at "+videoWidth+"x"+videoHeight);
        return true;
    }

    /** Where the start stopped, once start has returned false; null before that. */
    public XrStartFailure getStartFailure() {
        return startFailure;
    }

    // The whole of a failed start in one block of the log, and kept for a
    // report, which on a headset with no session can only be made from the
    // 2d settings later
    private static void keepStartFailure(Activity activity, XrStartFailure failure) {
        LimeLog.severe(failure.block(null));
        String when = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        PreferenceManager.getDefaultSharedPreferences(activity.getApplicationContext()).edit()
                .putString(BugReport.START_FAILURE_PREF, failure.block(when)).apply();
    }

    /**
     * Inference is longer than a display frame, so it lives on its own
     * thread with its own context in the render context's share group. The
     * frame loop hands over a captured frame and carries on submitting. The
     * stage thread, started here once the model has loaded, turns captures
     * into model input and model output into maps around the model's runs.
     */
    private void startDepthThread(final Activity activity, final MidasDepthSource.Spec spec,
                                  final MidasDepthSource.Route route) {
        depthThread = new Thread() {
            @Override
            public void run() {
                // A little above the default so a busy system does not starve
                // inference behind everything else, but deliberately not
                // BACKGROUND: that cpuset is little cores only on this SoC and
                // would make a model run slower in wall clock, not faster
                Process.setThreadPriority(Process.THREAD_PRIORITY_MORE_FAVORABLE);

                if (!nativeBindDepthContext(nativeCtx)) {
                    nativeDepthGaveUp(nativeCtx);
                    return;
                }

                DepthSource source = null;
                try {
                    ByteBuffer[] inputs = new ByteBuffer[DEPTH_PAIRS];
                    ByteBuffer[] outputs = new ByteBuffer[DEPTH_PAIRS];
                    for (int i = 0; i < DEPTH_PAIRS; i++) {
                        inputs[i] = nativeGetModelInput(nativeCtx, i);
                        outputs[i] = nativeGetModelOutput(nativeCtx, i);
                        if (inputs[i] == null || outputs[i] == null) {
                            LimeLog.severe("Depth staging buffers missing");
                            nativeDepthGaveUp(nativeCtx);
                            return;
                        }
                    }

                    MidasDepthSource model = new MidasDepthSource(route);
                    source = model;
                    if (!source.initialize(activity, inputs, outputs)) {
                        // The depth texture keeps the flat map it was
                        // initialized with, so zero disparity, and the
                        // stream stays watchable
                        LimeLog.severe("Depth source init failed, stereo will be flat");
                        nativeDepthGaveUp(nativeCtx);
                        return;
                    }
                    depthLabel = spec.name+" "+route.size+" "+model.runtimeLabel();

                    startDepthStage(source);
                    depthReady = true;
                    runDepthModel(source);
                } finally {
                    depthReady = false;
                    // The stage thread uses the native context too, so it is
                    // over before this thread is, which is what
                    // stopDepthThread waits on
                    stopDepthStage();
                    if (source != null) {
                        source.release();
                    }
                    nativeUnbindDepthContext(nativeCtx);
                }
            }
        };
        depthThread.setName("Video - XR Depth");
        depthThread.start();
    }

    /**
     * The stage thread, on the third context in the share group: it reads
     * each capture back into its pair's model input and uploads each map,
     * both while the model runs on the other pair.
     */
    private void startDepthStage(final DepthSource source) {
        depthStageThread = new Thread() {
            @Override
            public void run() {
                // As the depth thread, for the same reason: a map that waits
                // on this waits a turn of the model after it
                Process.setThreadPriority(Process.THREAD_PRIORITY_MORE_FAVORABLE);
                if (!nativeBindDepthStageContext(nativeCtx)) {
                    LimeLog.severe("Depth stage context would not bind, stereo will stay as it is");
                    nativeDepthGaveUp(nativeCtx);
                    return;
                }
                try {
                    runDepthStage(source);
                } finally {
                    nativeUnbindDepthStageContext(nativeCtx);
                }
            }
        };
        depthStageThread.setName("Video - XR Depth stage");
        depthStageThread.start();
    }

    /** Ends the stage thread and waits for it, whatever it is part way through. */
    private void stopDepthStage() {
        Thread stage = depthStageThread;
        if (stage == null) {
            return;
        }
        synchronized (depthLock) {
            depthExit = true;
            depthLock.notifyAll();
        }
        boolean interrupted = false;
        while (stage.isAlive()) {
            try {
                stage.join();
            } catch (InterruptedException e) {
                interrupted = true;
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
        depthStageThread = null;
    }

    /** The depth thread's loop: the model, on whichever pair is staged. */
    private void runDepthModel(DepthSource source) {
        while (true) {
            int pair;
            synchronized (depthLock) {
                while (true) {
                    if (depthExit) {
                        return;
                    }
                    pair = pairs.takeToRun();
                    if (pair >= 0) {
                        break;
                    }
                    try {
                        depthLock.wait();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }

            boolean ok = source.estimate(pair);
            long inferenceNs = (long)(source.getLastInferenceMs() * 1000000.0f);
            if (ok) {
                lastInferenceMs = source.getLastInferenceMs();
            }

            synchronized (depthLock) {
                pairInferenceNs[pair] = inferenceNs;
                pairs.ran(pair, ok);
                depthLock.notifyAll();
            }
        }
    }

    /**
     * The stage thread's loop. A capture waiting to be read back goes first,
     * since the model may be waiting on it; otherwise the oldest pair in
     * flight, once the model is done with it, is uploaded or dropped, so the
     * maps go up in the order their frames came.
     */
    private void runDepthStage(DepthSource source) {
        long runs = 0, skipped = 0;
        long inferenceNs = 0, uploadNs = 0, captureNs = 0, worstNs = 0;
        long periodNs = 0, periods = 0, lastMapNs = 0;

        while (true) {
            int pair;
            boolean finish;
            boolean ok = false;
            synchronized (depthLock) {
                while (true) {
                    if (depthExit) {
                        return;
                    }
                    pair = pairs.takeToFinish();
                    if (pair >= 0) {
                        finish = true;
                        break;
                    }
                    pair = pairs.takeToUpload();
                    if (pair >= 0) {
                        ok = pairs.made(pair);
                        finish = false;
                        break;
                    }
                    try {
                        depthLock.wait();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }

            if (finish) {
                // The frame loop only queued the readback. This is where it
                // is waited on and turned into the model input, on a thread
                // with no frame to miss, while the model runs the other pair.
                long start = System.nanoTime();
                long finishNs = nativeFinishDepthCapture(nativeCtx, pair);
                synchronized (depthLock) {
                    pairStartNs[pair] = start;
                    pairFinishNs[pair] = finishNs;
                    pairInferenceNs[pair] = 0;
                    // With nothing to run the model on, it waits its turn
                    // among the maps to be dropped
                    pairs.finished(pair, finishNs >= 0);
                    depthLock.notifyAll();
                }
                continue;
            }

            long upload = 0;
            if (ok) {
                upload = nativeUploadDepth(nativeCtx, pair);
                publishedFrameIndex = pairFrameIndex[pair];
                publishedFrameNs = pairFrameNs[pair];
            }
            else {
                nativeDropDepth(nativeCtx, pair);
            }
            long end = System.nanoTime();
            long capture = pairCaptureNs[pair] + Math.max(0, pairFinishNs[pair]);
            long inference = pairInferenceNs[pair];
            // From the readback being finished to the map going up, the waits
            // for the model and for this thread included
            long total = end - pairStartNs[pair];

            synchronized (depthLock) {
                pairs.freed(pair);
                skipped += skippedFrames;
                skippedFrames = 0;
                depthLock.notifyAll();
            }

            if (!ok) {
                continue;
            }

            long gap = lastMapNs == 0 ? 0 : end - lastMapNs;
            lastMapNs = end;
            captureNs += capture;
            inferenceNs += inference;
            uploadNs += upload;
            if (total > worstNs) {
                worstNs = total;
            }
            if (gap > 0 && gap < DEPTH_PERIOD_GAP_NS) {
                periodNs += gap;
                periods++;
            }
            if (++runs == DEPTH_STATS_INTERVAL) {
                LimeLog.info("Depth stage ("+(source.isGpuAccelerated() ? "GPU" : "CPU")
                        +"): capture "+msPer(captureNs, runs)
                        +" ms, inference "+msPer(inferenceNs, runs)
                        +" ms, upload "+msPer(uploadNs, runs)
                        +" ms, worst "+msPer(worstNs, 1)
                        +" ms, period "+(periods == 0 ? "0" : msPer(periodNs, periods))
                        +" ms, "+mapsPerSecond(periodNs, periods)
                        +" maps/s (target "+nativeGetDepthTarget(nativeCtx)
                        +"), frames skipped while busy "+skipped);
                lastDepthSkips = (int)skipped;
                lastMapsPerSecond = periodNs <= 0 ? 0.0f : (float)(periods * 1e9 / periodNs);
                runs = 0;
                skipped = 0;
                periods = 0;
                captureNs = inferenceNs = uploadNs = worstNs = periodNs = 0;
            }
        }
    }

    /** Maps a second over that many gaps between maps, one decimal, 0 for none. */
    private static String mapsPerSecond(long periodNs, long periods) {
        return periodNs <= 0 ? "0" : String.format("%.1f", periods * 1e9 / periodNs);
    }

    private void stopDepthThread() {
        if (depthThread == null) {
            return;
        }
        synchronized (depthLock) {
            depthExit = true;
            depthLock.notifyAll();
        }
        // The context is freed the moment this returns, and the thread uses
        // it, so a slow inference is waited out however long it takes rather
        // than left running on memory that is about to go. The stage thread
        // uses it too, and the depth thread waits that out before it ends.
        boolean interrupted = false;
        try {
            depthThread.join(2000);
        } catch (InterruptedException e) {
            interrupted = true;
        }
        if (depthThread.isAlive()) {
            LimeLog.warning("XR depth thread did not stop in time, waiting for it");
            while (depthThread.isAlive()) {
                try {
                    depthThread.join();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
        depthThread = null;
    }

    // True where the runtime ended the loop, false where a stop asked for here did
    private boolean runFrameLoop(PreferenceConfiguration prefs) {
        float distance = prefs.vrDistance / 10.0f;
        float quadWidth = prefs.vrScreenSize / 10.0f;
        float curvature = prefs.vrCurvature / 100.0f;
        // Stored as tenths of a percent of frame width
        float separation = prefs.vrStereoSeparation / 1000.0f;
        boolean eyeSwap = prefs.vrEyeSwap;
        boolean pointer = prefs.vrPointer;
        boolean gaze = prefs.vrGaze;

        long ageFrames = 0, ageNs = 0, ageSamples = 0, worstAgeNs = 0;
        // When the 3D last came back on. Until a map captured since then is
        // up, the live one is from before it went off, and its age says how
        // long the 3D was off rather than how far behind the depth is.
        long stereoBackNs = 0;

        while (!stopping) {
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                return !stopping;
            }
            trackPresence();
            if (r == FRAME_IDLE) {
                // Native side slept already while the session is not running.
                // The click's track is kept fed regardless, so it is playing
                // when the session comes back.
                XrClickSound click = clickSound;
                if (click != null) {
                    click.feed();
                }
                continue;
            }

            if (!focusedOnce && nativeHasBeenFocused(nativeCtx)) {
                focusedOnce = true;
            }

            // Read fresh each frame rather than once on the way in: the panel's
            // row writes it back to this same object, and the space is picked
            // from it on both sides of the frame, so a press takes effect on
            // the next one with no native state to keep in step.
            boolean headLocked = prefs.vrHeadLocked;

            // Each word from the host arms the controllers on this pass
            long rumble = rumbleWord;
            if (rumble != rumbleHanded) {
                rumbleHanded = rumble;
                nativeSetRumble(nativeCtx, XrPad.rumbleLow(rumble), XrPad.rumbleHigh(rumble));
            }

            // The pointer sleep row on the panel writes back to this same
            // object, so it is read fresh each frame like head lock
            nativeUpdateInput(nativeCtx, distance, quadWidth, curvature, headLocked,
                    pointer, gaze && gazeAllowed, prefs.vrPointerSleep, inputState);
            headYaw = inputState[IN_HEAD_YAW];
            dispatchInput();
            updateReport();
            updateCogReadout();
            updateCogMarks();
            updateCogClock();
            updateKeyboardSheet();
            updateToast();
            updateClickSound(prefs);

            // Switched off, the warp draws flat and the model is left idle.
            // Back on, it wants a map of what is showing now, so the frame in
            // hand is captured at once whatever the depth rate says.
            boolean stereoOn = inputState[IN_STEREO] != 0.0f;
            boolean stereoBack = stereoOn && !stereoLive;
            stereoLive = stereoOn;
            if (stereoBack) {
                stereoBackNs = System.nanoTime();
            }

            boolean newFrame = pendingFrames.getAndSet(0) > 0;
            if (newFrame) {
                surfaceTexture.updateTexImage();
                surfaceTexture.getTransformMatrix(texMatrix);

                if (depthReady && stereoOn) {
                    // By time since the last capture rather than by frame
                    // count, so the model's rate does not follow the stream's
                    if (nativeDepthDue(nativeCtx, stereoBack)) {
                        startDepthCapture();
                    }
                    if (publishedFrameNs != 0 && publishedFrameNs >= stereoBackNs) {
                        long age = System.nanoTime() - publishedFrameNs;
                        // Smoothed for the overlay, the raw value swings a lot
                        // between one inference landing and the next
                        float ageMs = age / 1000000.0f;
                        lastDepthAgeMs = lastDepthAgeMs == 0.0f ? ageMs
                                : lastDepthAgeMs * 0.95f + ageMs * 0.05f;
                        ageFrames += videoFrameIndex - publishedFrameIndex;
                        ageNs += age;
                        ageSamples++;
                        if (age > worstAgeNs) {
                            worstAgeNs = age;
                        }
                        if (ageSamples == DEPTH_AGE_INTERVAL) {
                            LimeLog.info("Depth age: "+String.format("%.1f", ageFrames
                                    / (double)ageSamples)+" video frames, "
                                    +msPer(ageNs, ageSamples)+" ms avg, "
                                    +msPer(worstAgeNs, 1)+" ms worst");
                            ageFrames = ageNs = ageSamples = worstAgeNs = 0;
                        }
                    }
                }
                videoFrameIndex++;
            }
            else if (stereoBack && depthReady && videoFrameIndex > 0) {
                // Nothing new from the decoder, so the frame still latched
                nativeDepthDue(nativeCtx, true);
                startDepthCapture();
            }
            ByteBuffer splash = pendingSplash.getAndSet(null);
            if (splash != null) {
                nativeUploadSplash(nativeCtx, splash);
            }

            // Upload here rather than from the reporting thread, since this is
            // the thread that owns the GL context
            ByteBuffer overlay = pendingOverlay.getAndSet(null);
            if (overlay != null) {
                nativeUploadOverlay(nativeCtx, overlay, OVERLAY_WIDTH, OVERLAY_HEIGHT);
            }

            ByteBuffer grid = pendingPickerArt.getAndSet(null);
            ByteBuffer button = pendingEnvButton.getAndSet(null);
            if (grid != null || button != null) {
                nativeUploadPicker(nativeCtx, grid, button, ENV_CELL_COUNT);
            }

            ByteBuffer[] cogSheets = pendingCogSheets.getAndSet(null);
            ByteBuffer cog = pendingCogButton.getAndSet(null);
            if (cogSheets != null || cog != null) {
                nativeUploadCog(nativeCtx, cogSheets, cog);
            }

            ByteBuffer[] kbSheets = pendingKbSheets.getAndSet(null);
            ByteBuffer kbButton = pendingKbButton.getAndSet(null);
            if (kbSheets != null || kbButton != null) {
                nativeUploadKeyboard(nativeCtx, kbSheets, kbButton, kbKeyRects, kbCodes);
                kbArtUp |= kbSheets != null;
            }

            ByteBuffer exitButton = pendingExitButton.getAndSet(null);
            ByteBuffer exitPlain = pendingExitPlain.getAndSet(null);
            ByteBuffer exitHot = pendingExitHot.getAndSet(null);
            ByteBuffer cancelHot = pendingCancelHot.getAndSet(null);
            if (exitButton != null || exitPlain != null || exitHot != null || cancelHot != null) {
                nativeUploadExit(nativeCtx, exitButton, exitPlain, exitHot, cancelHot);
            }

            ByteBuffer hint = pendingHandHint.getAndSet(null);
            if (hint != null) {
                nativeUploadHandHint(nativeCtx, hint);
            }

            ByteBuffer kofi = pendingKofi.getAndSet(null);
            if (kofi != null) {
                nativeUploadKofi(nativeCtx, kofi);
            }

            ByteBuffer stereoOff = pendingStereoOff.getAndSet(null);
            ByteBuffer stereoOnArt = pendingStereoOn.getAndSet(null);
            if (stereoOff != null && stereoOnArt != null) {
                nativeUploadStereoButton(nativeCtx, stereoOff, stereoOnArt);
            }

            ByteBuffer rayOff = pendingRayOff.getAndSet(null);
            ByteBuffer rayOn = pendingRayOn.getAndSet(null);
            if (rayOff != null && rayOn != null) {
                nativeUploadRayButton(nativeCtx, rayOff, rayOn);
            }

            ByteBuffer aimOff = pendingAimOff.getAndSet(null);
            ByteBuffer aimOn = pendingAimOn.getAndSet(null);
            if (aimOff != null && aimOn != null) {
                nativeUploadAimButton(nativeCtx, aimOff, aimOn);
            }

            ByteBuffer padOff = pendingPadOff.getAndSet(null);
            ByteBuffer padOn = pendingPadOn.getAndSet(null);
            if (padOff != null && padOn != null) {
                nativeUploadPadButton(nativeCtx, padOff, padOn);
            }

            ByteBuffer controller = pendingControllerModel.getAndSet(null);
            if (controller != null) {
                nativeUploadControllerModel(nativeCtx, controller, controller.remaining());
            }

            RoomAssets room = pendingRoom.getAndSet(null);
            if (room != null) {
                nativeUploadRoomModel(nativeCtx, room.mesh, room.meshBytes, room.cell);
                for (int slot = 0; slot < room.atlases.length; slot++) {
                    nativeUploadRoomAtlas(nativeCtx, room.atlases[slot], room.cell, slot);
                }
            }

            nativeEndFrame(nativeCtx, newFrame, texMatrix, distance, quadWidth, curvature,
                    headLocked, separation, eyeSwap, passthroughOn);
        }
        return false;
    }

    /**
     * Settles on a starting environment, then hands the slow half to another
     * thread: reading a room and drawing the panels take long enough that doing
     * it here would hold up the first frame and hang the shell on its loading
     * screen.
     */
    private void startEnvironment(PreferenceConfiguration prefs) {
        panels = new XrPanels(prefsContext, prefs.vrGamepadToggle);

        SharedPreferences saved = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        int id = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, -1);
        if (id < 0) {
            // An install from before the ids has a cell instead, which only
            // means anything read against the layout it was written under. The
            // old key is left where it is, since nothing costs less than a
            // stale int and an older build can still start on it.
            int legacy = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING, -1);
            if (EnvironmentIds.idForLegacyCell(legacy) >= 0) {
                id = EnvironmentIds.idForLegacyCell(legacy);
                saved.edit()
                        .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, id)
                        .apply();
            }
        }

        int cell = EnvironmentIds.startCell(id, prefs.vrPassthrough);
        if (id >= 0 && EnvironmentIds.cellForId(id) < 0) {
            // A photo or a room this build no longer has. Saying so once and
            // writing the void back keeps it from being said every launch.
            FileLog.event("environment id " + id + " is no longer offered, starting in the void");
            saved.edit()
                    .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING,
                            EnvironmentIds.idForCell(cell))
                    .apply();
        }
        environmentChoice = cell;
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;
        nativeSetEnvironment(nativeCtx, cell);

        // Every room's own Room tab values at once, so the picker can move
        // between rooms without asking again
        for (int roomCell = 0; roomCell < ENV_CELL_COUNT; roomCell++) {
            if (!EnvironmentIds.isRoomCell(roomCell)) {
                continue;
            }
            PreferenceConfiguration.RoomLevels levels = PreferenceConfiguration.readRoomLevels(
                    saved, EnvironmentIds.idForCell(roomCell));
            nativeSetRoomLevels(nativeCtx, roomCell, levels.brightness, levels.glow,
                    levels.light, levels.screen);
        }
        cogReadout = new XrPanels.Readout();
        // Nothing drawn yet, so the first look at either tab draws it
        Arrays.fill(readoutDrawn, -2);
        toast = new XrPanels.Toast();
        cogMarks = new XrPanels.Marks();
        cogClock = new XrPanels.ClockStrip();
        clock = new XrClock(prefsContext);
        // Nothing drawn yet, so the first look at the tab draws them
        Arrays.fill(marksDrawn, -2);
        nativeSetClickSound(nativeCtx, prefs.vrClickSound);
        nativeSetShowRay(nativeCtx, prefs.vrShowRay);
        nativeSetHeadAim(nativeCtx, prefs.vrHeadAim, prefs.vrHeadAimSensitivity,
                prefs.vrHeadAimDeadZone);
        // Every session starts as the pointer, and the shortcut chosen is the
        // one way the controllers switch themselves
        nativeSetGamepad(nativeCtx, prefs.vrGamepadToggle, prefs.deadzonePercentage);
        nativeSetControllerModel(nativeCtx, prefs.vrControllerModel);
        final boolean handHint = prefs.vrHandTracking && !prefs.vrHandLockHintSeen;
        nativeSetHandHint(nativeCtx, handHint);

        final int startRoom = cell;
        final int roomTicketAtStart;
        synchronized (roomLock) {
            roomTicketAtStart = ++roomTicket;
        }
        final boolean click = prefs.vrClickSound;
        if (click) {
            synchronized (clickLock) {
                clickSoundStarting = true;
            }
        }
        Thread loader = new Thread() {
            @Override
            public void run() {
                if (click) {
                    startClickSound();
                }
                buildPanelArt(handHint);
                pendingControllerModel.set(readAsset(CONTROLLER_MODEL));
                loadRoomAssets(startRoom, roomTicketAtStart);
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    // Every panel, drawn once and parked for the frame loop. The hand lock
    // hint only where the session may show it.
    private void buildPanelArt(boolean handHint) {
        pendingPickerArt.set(panels.buildPickerGrid());
        pendingEnvButton.set(panels.buildEnvButton());
        if (handHint) {
            pendingHandHint.set(panels.buildHandHint());
        }
        pendingKofi.set(panels.buildKofiSheet(SUPPORT_URL));

        // Curvature needs a layer type the runtime may not offer, and a slider
        // that cannot do anything is better shown greyed than hidden
        boolean curveOk;
        synchronized (nativeLock) {
            curveOk = nativeCtx != 0 && nativeGetCylinderSupported(nativeCtx);
        }
        // Same for the 3D rows with stereo turned off in settings. Their ticks
        // mark the running model's own pair.
        boolean stereoOk = prefConfig != null && prefConfig.vrDepthMode != DEPTH_MODE_OFF;
        MidasDepthSource.Spec spec = MidasDepthSource.specFor(
                prefConfig != null ? prefConfig.vrDepthModel : null);
        pendingCogSheets.set(panels.buildCogTabs(curveOk, stereoOk, spec.defaultSeparation,
                spec.defaultConvergence));
        pendingCogButton.set(panels.buildCogButton());
        // The 3D switch on the bar is left out altogether without stereo:
        // there is nothing for it to switch, and the 3D tab already says why
        if (stereoOk) {
            ByteBuffer[] faces = panels.buildStereoButtons();
            pendingStereoOff.set(faces[0]);
            pendingStereoOn.set(faces[1]);
        }
        ByteBuffer[] rayFaces = panels.buildRayButtons();
        pendingRayOff.set(rayFaces[0]);
        pendingRayOn.set(rayFaces[1]);
        ByteBuffer[] aimFaces = panels.buildAimButtons();
        pendingAimOff.set(aimFaces[0]);
        pendingAimOn.set(aimFaces[1]);
        ByteBuffer[] padFaces = panels.buildPadButtons();
        pendingPadOff.set(padFaces[0]);
        pendingPadOn.set(padFaces[1]);

        XrPanels.Keyboard keyboard = panels.buildKeyboard();
        kbKeyRects = keyboard.keyRects;
        kbCodes = keyboard.codes;
        pendingKbSheets.set(keyboard.sheets);
        pendingKbButton.set(keyboard.button);

        ByteBuffer[] exit = panels.buildExitArt();
        pendingExitButton.set(exit[0]);
        pendingExitPlain.set(exit[1]);
        pendingExitHot.set(exit[2]);
        pendingCancelHot.set(exit[3]);
    }

    // A cell is worth switching to if it is one of the grid's real ones. Past
    // those the grid is blank tiles, which the native side is told about so it
    // can leave them alone.
    private static boolean cellExists(int cell) {
        return cell >= 0 && cell < ENV_CELL_COUNT;
    }

    /**
     * A cell was picked in the grid. A room is read now and shows once it has
     * landed, with the void in its place until then.
     */
    private void chooseEnvironment(int cell) {
        if (!cellExists(cell)) {
            return;
        }
        environmentChoice = cell;
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;

        requestRoom(cell);
        nativeSetEnvironment(nativeCtx, cell);

        // The grid is a second way to reach the passthrough switch, so the
        // setting follows it rather than disagreeing with what is on screen.
        // The 2D settings' list keeps its choice the same way.
        EnvironmentIds.store(PreferenceManager.getDefaultSharedPreferences(prefsContext),
                EnvironmentIds.idForCell(cell));
    }

    // The mesh a baked room is built from, by the cell that shows it, or null
    // for a cell with no model behind it
    private static String roomMeshFile(int cell) {
        switch (cell) {
            case ENV_CELL_HOME_THEATER: return "home_theater.room";
            case ENV_CELL_GRAND_CINEMA: return "grand_cinema.room";
            case ENV_CELL_SYNTHWAVE: return "synthwave.room";
            default: return null;
        }
    }

    // And the atlases it is painted with, in the slot order its parts name
    // them by, already ASTC with their mip chains, so they go up as read. Each
    // room ships two sets from the same source: 4096, and 2048 for the XR2
    // Gen 1 headsets, whose rooms draw at half size and which have the least
    // memory to spare.
    private static String[] roomAtlasFiles(int cell) {
        String set = PreferenceConfiguration.isXr2Gen1Headset() ? "_lo" : "";
        switch (cell) {
            case ENV_CELL_HOME_THEATER: return new String[] { "home_theater" + set + "_0.atlas" };
            case ENV_CELL_GRAND_CINEMA: return new String[] { "grand_cinema" + set + "_0.atlas" };
            case ENV_CELL_SYNTHWAVE: return new String[] { "synthwave" + set + "_0.atlas" };
            default: return null;
        }
    }

    /**
     * Every pick lands here, room or not, so a room still being read for an
     * earlier pick is dropped rather than put up over the one chosen now. The
     * room already resident is not read again.
     */
    private void requestRoom(final int cell) {
        final int ticket;
        synchronized (roomLock) {
            ticket = ++roomTicket;
            if (roomMeshFile(cell) == null || cell == parkedRoomCell) {
                return;
            }
        }
        Thread loader = new Thread() {
            @Override
            public void run() {
                loadRoomAssets(cell, ticket);
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    /**
     * One baked room, its mesh and its atlases, read when it is picked and
     * parked for the frame loop to hand over, since that thread owns the GL
     * context and is the one that builds the geometry. Any of them failing
     * parks nothing, and the cell shows the void instead of anything broken.
     */
    private void loadRoomAssets(int cell, int ticket) {
        String meshFile = roomMeshFile(cell);
        String[] atlasFiles = roomAtlasFiles(cell);
        if (meshFile == null || atlasFiles == null) {
            return;
        }
        long started = System.nanoTime();
        ByteBuffer mesh = readAsset(ROOM_DIR + "/" + meshFile);
        if (mesh == null) {
            return;
        }

        ByteBuffer[] atlases = new ByteBuffer[atlasFiles.length];
        for (int slot = 0; slot < atlasFiles.length; slot++) {
            atlases[slot] = readAsset(ROOM_DIR + "/" + atlasFiles[slot]);
            if (atlases[slot] == null) {
                return;
            }
        }
        RoomAssets room = new RoomAssets(cell, mesh, atlases);

        synchronized (roomLock) {
            // Something else was picked while this was read
            if (ticket != roomTicket) {
                return;
            }
            pendingRoom.set(room);
            parkedRoomCell = cell;
        }
        FileLog.event("room " + meshFile + " and " + TextUtils.join(", ", atlasFiles) + " read in "
                + (System.nanoTime() - started) / 1000000 + " ms");
    }

    // A whole asset in a direct buffer, which is the only kind the native side
    // can read without a copy
    private ByteBuffer readAsset(String path) {
        InputStream in = null;
        try {
            in = prefsContext.getAssets().open(path);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[16384];
            int read;
            while ((read = in.read(chunk)) > 0) {
                out.write(chunk, 0, read);
            }
            byte[] all = out.toByteArray();
            ByteBuffer buffer = ByteBuffer.allocateDirect(all.length);
            buffer.put(all);
            buffer.rewind();
            return buffer;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Asset " + path + " failed: " + e);
            return null;
        } finally {
            XrPanels.closeQuietly(in);
        }
    }

    // Moves the pointer before any press, so a click lands where the user is
    // pointing rather than where they pointed last frame
    private void dispatchInput() {
        // The screen placement and the environment grid are ours either way,
        // only the host events need somewhere to go
        if (inputListener != null) {
            if (inputState[IN_HIT] != 0.0f) {
                inputListener.onVrPointerMove(inputState[IN_U], inputState[IN_V]);
            }
            // Head aim's turn and the pointer's nudges while it is on, as one
            // relative move. The native side never sets a hit then.
            int dx = (int)inputState[IN_MOUSE_DX];
            int dy = (int)inputState[IN_MOUSE_DY];
            if (dx != 0 || dy != 0) {
                inputListener.onVrMouseMove(dx, dy);
            }

            int buttons = (int)inputState[IN_BUTTONS];
            int changed = buttons ^ heldButtons;
            if (changed != 0) {
                for (int i = 0; i < 3; i++) {
                    int mask = 1 << i;
                    if ((changed & mask) != 0) {
                        inputListener.onVrButton(i, (buttons & mask) != 0);
                    }
                }
                heldButtons = buttons;
            }

            int clicks = (int)inputState[IN_SCROLL];
            if (clicks != 0) {
                inputListener.onVrScroll(clicks);
            }

            dispatchPad();

            // Every real code is 8 or more, so anything at zero or above is a
            // key rather than the sentinel. It goes before the modifiers are
            // let go of, since it was typed with them held. While the report
            // sheet is up the keys are its own, below.
            int key = (int)inputState[IN_KEY];
            if (key >= 0 && inputState[IN_REPORT_ZONE] < 0.0f) {
                inputListener.onVrKey(key, (int)inputState[IN_KEY_MODS]);
            }
            int mods = (int)inputState[IN_KB_MODS];
            if (mods != heldKbMods) {
                heldKbMods = mods;
                inputListener.onVrModifiers(mods);
            }

            // Cleared here as well as being written once natively, so a frame
            // that lands while the activity is on its way out cannot ask twice
            if (inputState[IN_EXIT] != 0.0f) {
                inputState[IN_EXIT] = 0.0f;
                inputListener.onVrExit();
            }
        }

        // The report sheet's fields take the keys while it is up, and nothing
        // typed there reaches the host. What was typed stays out of the log.
        int reportKey = (int)inputState[IN_KEY];
        if (reportKey >= 0 && inputState[IN_REPORT_ZONE] >= 0.0f && reportForm.type(reportKey)) {
            LimeLog.info("Report sheet: " + reportForm.note().length() + " characters in the note, "
                    + reportForm.address().length() + " in the address, the keys on the "
                    + (reportForm.focus() == REPORT_ZONE_NOTE ? "note" : "address")
                    + (reportForm.canSend() ? ", ready to send" : ""));
        }

        // A 3d room forces the picture onto its wall, so what comes back while
        // one is on is the wall's placement rather than the user's. Writing it
        // would lose where they had the screen in every other environment.
        if (inputState[IN_POSE_DIRTY] != 0.0f && !EnvironmentIds.isRoomCell(environmentChoice)) {
            saveScreenPose();
        }

        int pick = (int)inputState[IN_PICKER_PICK];
        if (pick >= 0) {
            chooseEnvironment(pick);
        }

        // "Don't show this again" on the hand lock hint, kept for every
        // session after this one
        if (inputState[IN_HINT] != 0.0f && prefsContext != null) {
            PreferenceConfiguration.markHandLockHintSeen(
                    PreferenceManager.getDefaultSharedPreferences(prefsContext));
            FileLog.event("hand lock hint put away for good");
        }

        int setting = (int)inputState[IN_SETTING];
        if (setting >= 0) {
            applySetting(setting, (int)inputState[IN_SETTING_VALUE],
                    (int)inputState[IN_SETTING_ROOM]);
        }
    }

    // The session leaving after its first focus and coming back, told to the
    // activity once each way. Frame loop only.
    private void trackPresence() {
        int presence = nativeGetPresence(nativeCtx);
        SessionListener listener = sessionListener;
        if (presence == PRESENCE_AWAY && !sessionAway) {
            sessionAway = true;
            LimeLog.info("VR session stopped after its first focus");
            if (listener != null) {
                listener.onVrSessionAway();
            }
        }
        else if (presence == PRESENCE_FOCUSED && sessionAway) {
            sessionAway = false;
            LimeLog.info("VR session focused again");
            if (listener != null) {
                listener.onVrSessionBack();
            }
        }
    }

    // The pad plugged in, its state each time it changes, and the pad taken
    // out, in that order, and only on a change, since the native side fills
    // the slots every frame
    private void dispatchPad() {
        boolean plugged = inputState[IN_PAD] != 0.0f;
        if (plugged && !padPlugged) {
            padPlugged = true;
            inputListener.onVrGamepadPlugged(true);
            // Nothing sent yet, so the first state always goes
            Arrays.fill(padSent, Integer.MIN_VALUE);
        }
        if (!plugged) {
            unplugPad();
            return;
        }
        boolean changed = false;
        for (int i = 0; i < padSent.length; i++) {
            int value = (int)inputState[IN_PAD_BUTTONS + i];
            if (value != padSent[i]) {
                padSent[i] = value;
                changed = true;
            }
        }
        if (changed) {
            inputListener.onVrGamepadState(padSent[0], padSent[1], padSent[2], padSent[3],
                    padSent[4], padSent[5], padSent[6]);
        }
    }

    // The pad out, if it was in. Frame loop only.
    private void unplugPad() {
        if (padPlugged) {
            padPlugged = false;
            if (inputListener != null) {
                inputListener.onVrGamepadPlugged(false);
            }
        }
    }

    // The report sheet coming up, a field taking the keys and Send, then the
    // sheet drawn again when what it shows has changed, and Send's readiness
    // handed down when it has. Cancel needs nothing: the next opening starts
    // the note afresh.
    private void updateReport() {
        int event = (int)inputState[IN_REPORT];
        int zone = (int)inputState[IN_REPORT_ZONE];
        if (event == REPORT_OPENED) {
            reportOpening++;
            reportForm.open(prefsContext != null ? PreferenceManager
                    .getDefaultSharedPreferences(prefsContext).getString(BugReport.EMAIL_PREF, "")
                    : "");
            reportDrawn = null;
            // Each opening starts with Send held on the native side
            reportSendHanded = false;
        }
        else if (event == REPORT_ZONE_NOTE || event == REPORT_ZONE_EMAIL) {
            reportForm.focus(event);
        }
        else if (event == REPORT_ZONE_SEND) {
            sendReport();
        }

        ReportDrawing drawn = pendingReport.getAndSet(null);
        if (drawn != null) {
            reportDrawing = false;
            if (drawn.opening == reportOpening && zone >= 0 && drawn.pixels != null) {
                nativeUploadReport(nativeCtx, drawn.pixels);
                reportDrawn = drawn.key;
                LimeLog.info("Report sheet drawn in " + drawn.ms + " ms, off the frame loop");
            }
        }
        if (zone < 0) {
            return;
        }

        boolean ready = reportForm.canSend();
        if (ready != reportSendHanded) {
            nativeSetReportSend(nativeCtx, ready);
            reportSendHanded = ready;
        }
        final int opening = reportOpening;
        final String note = reportForm.note();
        final String address = reportForm.address();
        final int focus = reportForm.focus();
        final boolean wrong = reportForm.addressWrong();
        final String key = opening + "|" + focus + "|" + zone + "|" + ready + "|" + wrong + "|"
                + note + "\u0000" + address;
        if (reportDrawing || key.equals(reportDrawn) || prefsContext == null) {
            return;
        }
        reportDrawing = true;
        final int hover = zone;
        final boolean sendable = ready;
        final Context context = prefsContext;
        Thread draw = new Thread() {
            @Override
            public void run() {
                long start = System.nanoTime();
                ByteBuffer pixels = null;
                try {
                    XrPanels.ReportSheet art = reportArt;
                    if (art == null) {
                        art = new XrPanels.ReportSheet(context, BugReport.collectorConfigured());
                        reportArt = art;
                    }
                    pixels = art.draw(note, address, focus, hover, sendable, wrong);
                } catch (RuntimeException | OutOfMemoryError e) {
                    LimeLog.warning("Report sheet failed to draw: " + e);
                }
                pendingReport.set(new ReportDrawing(opening, key, pixels,
                        msPer(System.nanoTime() - start, 1)));
            }
        };
        draw.setName("Video - XR Report");
        draw.start();
    }

    // A drawing of the report sheet, waiting to go up
    private static final class ReportDrawing {
        final int opening;
        final String key;
        final ByteBuffer pixels;
        final String ms;

        ReportDrawing(int opening, String key, ByteBuffer pixels, String ms) {
            this.opening = opening;
            this.key = key;
            this.pixels = pixels;
            this.ms = ms;
        }
    }

    // Send was pressed with a note that will do: the address is remembered
    // for next time, and the report is put together, saved and posted on a
    // thread of its own while the stream carries on. The toast says how it
    // went.
    private void sendReport() {
        final Context context = prefsContext;
        final String note = reportForm.note();
        final String address = reportForm.address().trim();
        if (context == null || !BugReport.canSend(note, address)) {
            return;
        }
        PreferenceManager.getDefaultSharedPreferences(context).edit()
                .putString(BugReport.EMAIL_PREF, address).apply();
        final String session = "environment " + environmentName(environmentChoice)
                + ", 3d " + (stereoLive ? "on" : "off")
                + ", display " + Math.round(nativeGetDisplayRate(nativeCtx)) + " Hz";
        FileLog.event("report from the session: " + note.length() + " characters, "
                + (address.isEmpty() ? "no address" : "an address") + ", "
                + (BugReport.collectorConfigured() ? "sending" : "saving"));
        if (BugReport.collectorConfigured()) {
            showNotice(context.getString(R.string.bug_report_sending), null);
        }
        Thread send = new Thread() {
            @Override
            public void run() {
                BugReport.Outcome outcome = BugReport.file(context, note, address, session);
                FileLog.event("report " + outcome.result.name().toLowerCase(Locale.ROOT)
                        + (outcome.path != null ? " at " + outcome.path : "")
                        + (outcome.detail != null ? ": " + outcome.detail : ""));
                String where = context.getString(R.string.vr_report_saved_at,
                        BugReport.shortPath(outcome.path));
                switch (outcome.result) {
                    case SENT:
                        showNotice(context.getString(R.string.bug_report_sent), null);
                        break;
                    case NOT_SENT:
                        showNotice(context.getString(R.string.vr_report_not_sent), where, true);
                        break;
                    case BUSY:
                        showNotice(context.getString(R.string.vr_report_busy), where, true);
                        break;
                    case SAVED:
                        showNotice(context.getString(R.string.vr_report_saved), where, true);
                        break;
                    default:
                        showNotice(context.getString(R.string.vr_report_not_written),
                                outcome.detail);
                        break;
                }
            }
        };
        send.setName("Video - XR Report send");
        send.start();
    }

    // The environment showing, as the report's session line names it
    private static String environmentName(int cell) {
        switch (cell) {
            case ENV_CELL_PASSTHROUGH: return "passthrough";
            case ENV_CELL_VOID: return "void";
            case ENV_CELL_HOME_THEATER: return "home theater";
            case ENV_CELL_GRAND_CINEMA: return "grand cinema";
            case ENV_CELL_SYNTHWAVE: return "synthwave";
            default: return "cell " + cell;
        }
    }

    // Redraws the values beside the Room or Picture tab's tracks when the
    // frame says one has moved, and hands the strip straight up, since this is
    // the thread with the GL context. The native side only shows a strip drawn
    // from the values in force, so a stale one never reaches the panel.
    private void updateCogReadout() {
        if (inputState[IN_READOUT] < 0.0f || cogReadout == null) {
            return;
        }
        boolean changed = false;
        for (int i = 0; i < READOUT_VALUES; i++) {
            readoutWanted[i] = (int)inputState[IN_READOUT + i];
            changed |= readoutWanted[i] != readoutDrawn[i];
        }
        if (!changed) {
            return;
        }
        nativeUploadCogReadout(nativeCtx, cogReadout.draw(readoutWanted), readoutWanted);
        System.arraycopy(readoutWanted, 0, readoutDrawn, 0, READOUT_VALUES);
    }

    // Redraws the display tab's marks when the frame says one has moved, and
    // hands the strip straight up, since this is the thread with the GL context
    private void updateCogMarks() {
        if (inputState[IN_MARKS] < 0.0f || cogMarks == null) {
            return;
        }
        boolean changed = false;
        for (int i = 0; i < MARK_VALUES; i++) {
            marksWanted[i] = (int)inputState[IN_MARKS + i];
            changed |= marksWanted[i] != marksDrawn[i];
        }
        if (changed) {
            nativeUploadCogMarks(nativeCtx, cogMarks.draw(marksWanted));
            System.arraycopy(marksWanted, 0, marksDrawn, 0, MARK_VALUES);
        }
    }

    // Draws the keyboard sheet showing again when the modifiers lit on the
    // keyboard are not the ones it was drawn with. A sheet took up to 44 ms to
    // draw on a Quest 2 while the code was cold, a few frames, so it is drawn
    // on a thread of its own and goes up from here, the thread with the GL
    // context, once it is done. A press changes them, so this is once a press
    // at most.
    private void updateKeyboardSheet() {
        KbSheet drawn = pendingKbSheet.getAndSet(null);
        if (drawn != null) {
            nativeUploadKeyboardSheet(nativeCtx, drawn.state, drawn.pixels);
            kbSheetMods[drawn.state] = drawn.mods;
            kbSheetDrawing = false;
            LimeLog.info("Keyboard sheet " + drawn.state + " drawn with modifiers " + drawn.mods
                    + " in " + drawn.ms + " ms, off the frame loop");
        }
        final int sheet = (int)inputState[IN_KB_SHEET];
        final XrPanels art = panels;
        if (!kbArtUp || kbSheetDrawing || art == null || sheet < 0 || sheet >= KB_STATE_COUNT) {
            return;
        }
        final int mods = (int)inputState[IN_KB_MODS];
        if (mods == kbSheetMods[sheet]) {
            return;
        }
        kbSheetDrawing = true;
        Thread draw = new Thread() {
            @Override
            public void run() {
                long start = System.nanoTime();
                ByteBuffer pixels = null;
                try {
                    pixels = art.buildKeyboardSheet(sheet, mods);
                } catch (RuntimeException | OutOfMemoryError e) {
                    // The sheet stays as it was rather than the keyboard
                    // waiting on a drawing that will never come
                    LimeLog.warning("Keyboard sheet " + sheet + " failed to draw: " + e);
                }
                pendingKbSheet.set(new KbSheet(sheet, mods, pixels,
                        msPer(System.nanoTime() - start, 1)));
            }
        };
        draw.setName("Video - XR Keyboard");
        draw.start();
    }

    // A keyboard sheet drawn off the frame loop, waiting to go up
    private static final class KbSheet {
        final int state;
        final int mods;
        final ByteBuffer pixels;
        final String ms;

        KbSheet(int state, int mods, ByteBuffer pixels, String ms) {
            this.state = state;
            this.mods = mods;
            this.pixels = pixels;
            this.ms = ms;
        }
    }

    // Keeps the clock line over the settings panel up to the minute while the
    // panel is up, and draws it as it comes up if the minute moved while it
    // was down
    private void updateCogClock() {
        XrClock now = clock;
        if (inputState[IN_COG_OPEN] == 0.0f || now == null || cogClock == null) {
            return;
        }
        String line = now.line(System.currentTimeMillis());
        if (!line.equals(cogClockDrawn)) {
            nativeUploadCogClock(nativeCtx, cogClock.draw(line));
            cogClockDrawn = line;
        }
    }

    // Keeps the click's track fed every frame and ticks for a press, while the
    // setting is on. Switched on part way, the track is built off the frame
    // loop and used from the frame it is ready.
    private void updateClickSound(PreferenceConfiguration prefs) {
        XrClickSound click = clickSound;
        if (click == null) {
            if (!prefs.vrClickSound) {
                return;
            }
            synchronized (clickLock) {
                if (clickSoundStarting) {
                    return;
                }
                clickSoundStarting = true;
            }
            Thread starter = new Thread() {
                @Override
                public void run() {
                    startClickSound();
                }
            };
            starter.setName("Video - XR Click");
            starter.start();
            return;
        }
        click.feed();
        if (inputState[IN_CLICK] != 0.0f && prefs.vrClickSound) {
            click.click();
        }
    }

    // Off the frame loop, once: the track talks to the audio system as it is
    // built. A session already over by the time it is ready lets it go.
    private void startClickSound() {
        // The track's non blocking write arrived in M
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return;
        }
        XrClickSound click = XrClickSound.start();
        if (click == null) {
            return;
        }
        synchronized (clickLock) {
            if (!clickSessionOver) {
                clickSound = click;
                return;
            }
        }
        click.release();
    }

    // Queues the notices raised on this side since the last frame, then draws
    // whichever notice the frame says has just gone up and hands it straight
    // up, since this is the thread with the GL context
    private void updateToast() {
        NoticeWords notice;
        while ((notice = pendingNotices.poll()) != null) {
            int slot = noticeSlot;
            noticeSlot = (noticeSlot + 1) % TOAST_TEXT_SLOTS;
            noticeTexts[slot] = notice;
            nativePushNotice(nativeCtx, TOAST_TEXT, slot);
        }
        int kind = (int)inputState[IN_TOAST];
        if (kind < 0 || toast == null || prefsContext == null) {
            return;
        }
        int arg = (int)inputState[IN_TOAST_ARG];
        String text;
        String more = null;
        boolean path = false;
        switch (kind) {
            case TOAST_RATE:
                text = prefsContext.getString(R.string.vr_toast_rate, arg);
                break;
            case TOAST_HANDS_LOCKED:
                text = prefsContext.getString(R.string.vr_toast_hands_locked);
                break;
            case TOAST_HANDS_UNLOCKED:
                text = prefsContext.getString(R.string.vr_toast_hands_unlocked);
                break;
            case TOAST_3D_OFF:
                text = prefsContext.getString(R.string.vr_toast_3d_off);
                break;
            case TOAST_3D_ON:
                text = prefsContext.getString(R.string.vr_toast_3d_on);
                break;
            case TOAST_HEAD_AIM_OFF:
                text = prefsContext.getString(R.string.vr_toast_head_aim_off);
                break;
            case TOAST_HEAD_AIM_ON:
                text = prefsContext.getString(R.string.vr_toast_head_aim_on);
                more = prefsContext.getString(R.string.vr_toast_head_aim_on_more);
                break;
            case TOAST_GAMEPAD_MODE:
                // With the way back, which is the shortcut chosen
                text = prefsContext.getString(R.string.vr_toast_gamepad_mode);
                more = prefsContext.getString(XrPanels.gamepadBackText(
                        prefConfig != null ? prefConfig.vrGamepadToggle : PAD_SHORTCUT_MENU_GRIP));
                break;
            case TOAST_POINTER_MODE:
                text = prefsContext.getString(R.string.vr_toast_pointer_mode);
                break;
            case TOAST_TEXT:
                NoticeWords words = arg >= 0 && arg < TOAST_TEXT_SLOTS ? noticeTexts[arg] : null;
                if (words == null) {
                    return;
                }
                text = words.text;
                more = words.more;
                path = words.path;
                break;
            default:
                return;
        }
        nativeUploadToast(nativeCtx, toast.draw(text, more, path), kind, arg);
    }

    /**
     * A row on one of the panel's tabs was pressed or let go of. The native
     * side has already applied it to the running session, this end only has
     * to make it stick and tell whatever else in the app cares. A room's own
     * values go under the id of the room they were set in, which roomCell
     * names.
     */
    private void applySetting(int setting, int value, int roomCell) {
        if (prefsContext == null) {
            return;
        }

        if (setting == SETTING_ROOM_BRIGHTNESS || setting == SETTING_ROOM_GLOW
                || setting == SETTING_ROOM_LIGHT_LEVEL || setting == SETTING_ROOM_SCREEN) {
            applyRoomSetting(setting, value, EnvironmentIds.idForCell(roomCell));
        }
        else if (setting == SETTING_SHARPEN) {
            String choice = value == 2 ? "quality" : (value == 1 ? "normal" : "off");
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putString(PreferenceConfiguration.VR_SHARPENING_PREF_STRING, choice)
                    .apply();
        }
        else if (setting == SETTING_SUPERSAMPLE) {
            String choice = value == 2 ? "quality" : (value == 1 ? "normal" : "off");
            if (prefConfig != null) {
                prefConfig.vrSupersampling = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putString(PreferenceConfiguration.VR_SUPERSAMPLING_PREF_STRING, choice)
                    .apply();
        }
        else if (setting == SETTING_STATS) {
            boolean on = value != 0;
            // The decoder reads this off the same configuration object every
            // time it is about to report, so stats stop or resume at the next
            // one second window with nothing to restart
            if (prefConfig != null) {
                prefConfig.enablePerfOverlay = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.ENABLE_PERF_OVERLAY_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_AMBILIGHT) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrAmbilight = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_AMBILIGHT_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_ROOM_LIGHT) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrRoomLight = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_ROOM_LIGHT_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_HEAD_LOCK) {
            boolean on = value != 0;
            // The frame loop reads this off the same configuration object every
            // frame and passes it down, so the screen follows the head, or
            // stops following it, on the next one. A room ignores it either
            // way, which is why the row stays live in one rather than greying.
            if (prefConfig != null) {
                prefConfig.vrHeadLocked = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_HEAD_LOCKED_PREF_STRING, on)
                    .apply();
        }
        else if (setting == SETTING_POINTER_SLEEP) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrPointerSleep = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_POINTER_SLEEP_PREF_STRING, on)
                    .apply();
            FileLog.event("pointer sleep " + (on ? "on" : "off") + " saved");
        }
        else if (setting == SETTING_CLICK_SOUND) {
            boolean on = value != 0;
            // The frame loop reads this off the same configuration object
            if (prefConfig != null) {
                prefConfig.vrClickSound = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_CLICK_SOUND_PREF_STRING, on)
                    .apply();
            FileLog.event("click sound " + (on ? "on" : "off") + " saved");
        }
        else if (setting == SETTING_CONTROLLER_MODEL) {
            boolean on = value != 0;
            if (prefConfig != null) {
                prefConfig.vrControllerModel = on;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putBoolean(PreferenceConfiguration.VR_CONTROLLER_MODEL_PREF_STRING, on)
                    .apply();
            FileLog.event("controller model " + (on ? "on" : "off") + " saved");
        }
        else if (setting == SETTING_HEAD_AIM_SENSITIVITY) {
            // The native side already turns at it; this is for next time
            if (prefConfig != null) {
                prefConfig.vrHeadAimSensitivity = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, value)
                    .apply();
            FileLog.event("head aim sensitivity " + value + " saved");
        }
        else if (setting == SETTING_HEAD_AIM_DEADZONE) {
            if (prefConfig != null) {
                prefConfig.vrHeadAimDeadZone = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, value)
                    .apply();
            FileLog.event("head aim dead zone " + value + " saved");
        }
        else if (setting == SETTING_AMBI_LEVEL) {
            if (prefConfig != null) {
                prefConfig.vrAmbilightLevel = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_AMBILIGHT_LEVEL_PREF_STRING, value)
                    .apply();
        }
        else if (setting == SETTING_SEPARATION) {
            // The frame loop read its copy once and keeps passing that stale
            // one down, but the native panel value overrides it for the rest of
            // the session, so this write is only for next time
            if (prefConfig != null) {
                prefConfig.vrStereoSeparation = value;
            }
            SharedPreferences saved = PreferenceManager.getDefaultSharedPreferences(prefsContext);
            saved.edit().putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, value).apply();
            FileLog.event("separation " + value + " saved, the preference holds "
                    + saved.getInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, -1)
                    + ", preset " + PreferenceConfiguration.presetLabel(value,
                            prefConfig != null ? prefConfig.vrDepthModel : null));
        }
        else if (setting == SETTING_CONVERGENCE) {
            if (prefConfig != null) {
                prefConfig.vrConvergence = value;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING, value)
                    .apply();
        }
        else if (setting == SETTING_RESET_3D) {
            // Both at once, since the reset button moved both, and back to
            // nothing stored rather than to numbers: nothing stored is the
            // running model's own pair, so a later change of model moves it
            // along with the model the way it would have had it never been
            // touched
            String model = prefConfig != null ? prefConfig.vrDepthModel : null;
            if (prefConfig != null) {
                prefConfig.vrStereoSeparation = PreferenceConfiguration.defaultSeparation(model);
                prefConfig.vrConvergence = PreferenceConfiguration.defaultConvergence(model);
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .remove(PreferenceConfiguration.VR_SEPARATION_PREF_STRING)
                    .remove(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING)
                    .apply();
            FileLog.event("3d pair back to the model's own "
                    + PreferenceConfiguration.defaultPairLabel(model));
        }
        else if (pictureRow(setting) >= 0) {
            // The grade is already live on the native side, and this is the
            // same configuration object the session started from, so only the
            // preference has to be written
            int row = pictureRow(setting);
            int units = PreferenceConfiguration.clampPicture(row, value);
            if (prefConfig != null) {
                prefConfig.vrPicture[row] = units;
            }
            String key = PreferenceConfiguration.pictureKey(row);
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(key, units)
                    .apply();
            FileLog.event("picture setting " + key + " = " + units + " saved");
        }
        else if (setting == SETTING_RESET_PICTURE) {
            // Back to nothing stored, which reads as the picture as streamed,
            // and which the 2d seekbars show as their defaults
            SharedPreferences.Editor editor =
                    PreferenceManager.getDefaultSharedPreferences(prefsContext).edit();
            for (int row = 0; row < PICTURE_VALUES; row++) {
                if (prefConfig != null) {
                    prefConfig.vrPicture[row] = PreferenceConfiguration.pictureDefault(row);
                }
                editor.remove(PreferenceConfiguration.pictureKey(row));
            }
            editor.apply();
            FileLog.event("picture back to the picture as streamed");
        }
    }

    // Which picture row a setting id writes, or -1 for any other setting
    private static int pictureRow(int setting) {
        switch (setting) {
            case SETTING_PICTURE_BRIGHTNESS: return PICTURE_BRIGHTNESS;
            case SETTING_PICTURE_CONTRAST: return PICTURE_CONTRAST;
            case SETTING_PICTURE_GAMMA: return PICTURE_GAMMA;
            case SETTING_PICTURE_SATURATION: return PICTURE_SATURATION;
            default: return -1;
        }
    }

    // One room's own value, under that room's key, with a line in the log so
    // a report says what each room was left at
    private void applyRoomSetting(int setting, int value, int roomId) {
        if (!PreferenceConfiguration.isRoomEnvironment(roomId)) {
            return;
        }
        SharedPreferences.Editor editor =
                PreferenceManager.getDefaultSharedPreferences(prefsContext).edit();
        String key;
        if (setting == SETTING_ROOM_GLOW) {
            key = PreferenceConfiguration.roomGlowKey(roomId);
            editor.putBoolean(key, value != 0);
        }
        else {
            if (setting == SETTING_ROOM_BRIGHTNESS) {
                key = PreferenceConfiguration.roomBrightnessKey(roomId);
            }
            else if (setting == SETTING_ROOM_LIGHT_LEVEL) {
                key = PreferenceConfiguration.roomLightKey(roomId);
            }
            else {
                key = PreferenceConfiguration.roomScreenKey(roomId);
                value = PreferenceConfiguration.clampRoomScreen(roomId, value);
            }
            editor.putInt(key, value);
        }
        editor.apply();
        FileLog.event("room setting " + key + " = "
                + (setting == SETTING_ROOM_GLOW ? String.valueOf(value != 0) : value) + " saved");
    }

    // Written once when a grab ends, so the screen is where it was left next
    // time. Cleared by the reset in settings.
    private void saveScreenPose() {
        if (prefsContext == null) {
            return;
        }

        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < POSE_VALUES; i++) {
            if (i > 0) {
                sb.append(',');
            }
            sb.append(inputState[IN_POSE + i]);
        }

        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putString(PreferenceConfiguration.VR_SCREEN_POSE_PREF_STRING, sb.toString())
                .apply();
    }

    private void restoreScreenPose() {
        String saved = PreferenceManager.getDefaultSharedPreferences(prefsContext)
                .getString(PreferenceConfiguration.VR_SCREEN_POSE_PREF_STRING, null);
        if (saved == null) {
            return;
        }

        String[] parts = saved.split(",");
        // Anything saved before the settings panel existed is one value short,
        // and a missing curvature means nobody has chosen one
        if (parts.length < POSE_VALUES - 1) {
            return;
        }

        float[] pose = new float[POSE_VALUES];
        try {
            for (int i = 0; i < POSE_VALUES; i++) {
                pose[i] = i < parts.length ? Float.parseFloat(parts[i]) : -1.0f;
            }
        } catch (NumberFormatException e) {
            return;
        }

        nativeSetScreenPose(nativeCtx, pose);
    }

    /**
     * Asks the GPU for a downscaled copy of the frame just latched, into a
     * free pair, and wakes the stage thread. Only this stays on the frame
     * loop, since it has to sample the video texture this context owns, and
     * it only queues work: the stage thread waits for the pixels itself, in
     * nativeFinishDepthCapture.
     */
    private void startDepthCapture() {
        int pair;
        synchronized (depthLock) {
            pair = pairs.forCapture();
            if (pair < 0) {
                skippedFrames++;
                return;
            }
        }

        // Still free as far as the lock knows, but only this thread hands out
        // free pairs, so it is this capture's until it is marked below
        pairCaptureNs[pair] = nativeCaptureDepthInput(nativeCtx, texMatrix, pair);
        pairFrameIndex[pair] = videoFrameIndex;
        pairFrameNs[pair] = System.nanoTime();

        synchronized (depthLock) {
            pairs.captured(pair);
            depthLock.notifyAll();
        }
    }

    /**
     * Draws the stats into the overlay layer. Called about once a second from
     * whichever thread produced them, never from the frame loop, so the
     * bitmap work cannot stall frame submission.
     *
     * The renderer appends its own numbers, since decode and network stats
     * come from the decoder but warp, inference and depth age only exist here.
     */
    public void setOverlayText(String text) {
        if (nativeCtx == 0) {
            return;
        }
        // The previous one has not been picked up yet, so skip this update
        // rather than write a buffer the frame loop may be reading
        if (pendingOverlay.get() != null) {
            return;
        }

        if (overlayBitmap == null) {
            overlayBitmap = Bitmap.createBitmap(OVERLAY_WIDTH, OVERLAY_HEIGHT,
                    Bitmap.Config.ARGB_8888);
            overlayCanvas = new Canvas(overlayBitmap);
            overlayPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
            overlayPaint.setTypeface(Typeface.MONOSPACE);
            overlayPaint.setTextSize(OVERLAY_TEXT_SIZE);
            overlayPaint.setColor(Color.WHITE);
            overlayBuffers = new ByteBuffer[2];
            for (int i = 0; i < overlayBuffers.length; i++) {
                overlayBuffers[i] = ByteBuffer.allocateDirect(OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);
                overlayBuffers[i].order(ByteOrder.nativeOrder());
            }
        }

        // Dark backing so the text stays readable over any content
        overlayCanvas.drawColor(0xB0000000, PorterDuff.Mode.SRC);
        // Texture rows run bottom up, so draw mirrored and let the upload put
        // it back the right way round
        overlayCanvas.save();
        overlayCanvas.translate(0.0f, OVERLAY_HEIGHT);
        overlayCanvas.scale(1.0f, -1.0f);
        float y = OVERLAY_LINE_HEIGHT;
        // The time and the battery first, so a long session has both in view
        XrClock now = clock;
        String first = now != null ? now.line(System.currentTimeMillis()) + '\n' : "";
        for (String line : (first + text + '\n' + rendererStats()).split("\n")) {
            overlayCanvas.drawText(line, 8.0f, y, overlayPaint);
            y += OVERLAY_LINE_HEIGHT;
            if (y > OVERLAY_HEIGHT) {
                break;
            }
        }
        overlayCanvas.restore();

        ByteBuffer buf = overlayBuffers[overlayBufferIndex];
        overlayBufferIndex = (overlayBufferIndex + 1) % overlayBuffers.length;
        buf.rewind();
        overlayBitmap.copyPixelsToBuffer(buf);
        buf.rewind();
        pendingOverlay.set(buf);
    }

    private String rendererStats() {
        float warpMs, displayHz, askedHz;
        int depthTarget;
        synchronized (nativeLock) {
            if (nativeCtx == 0) {
                return "";
            }
            warpMs = nativeGetWarpGpuMs(nativeCtx);
            displayHz = nativeGetDisplayRate(nativeCtx);
            askedHz = nativeGetAskedRate(nativeCtx);
            depthTarget = nativeGetDepthTarget(nativeCtx);
        }
        StringBuilder sb = new StringBuilder();
        if (displayHz > 0.0f) {
            sb.append("Display: ").append(Math.round(displayHz)).append(" Hz");
            // Still on its way, or the runtime would not move
            if (askedHz > 0.0f && Math.abs(askedHz - displayHz) > 0.5f) {
                sb.append(", ").append(Math.round(askedHz)).append(" asked");
            }
            sb.append('\n');
        }
        sb.append(String.format("Warp GPU: %.2f ms", warpMs));
        // Switched off from the bar or the 3D tab, the model's numbers are
        // the last ones it had and mean nothing, so they make way for saying so
        boolean switchedOff = !stereoLive && prefConfig != null
                && prefConfig.vrDepthMode != DEPTH_MODE_OFF;
        if (depthReady) {
            sb.append('\n').append("Depth model: ").append(depthLabel);
        }
        if (switchedOff) {
            sb.append('\n').append(depthReady ? "3D: off, depth model idle" : "3D: off");
        }
        else if (depthReady) {
            sb.append('\n').append(depthRateLine(lastMapsPerSecond, depthTarget));
            sb.append('\n').append(String.format("Depth inference: %.1f ms", lastInferenceMs));
            sb.append('\n').append(String.format("Depth age: %.0f ms", lastDepthAgeMs));
            sb.append('\n').append("Depth frames skipped: ").append(lastDepthSkips);
        }
        return sb.toString();
    }

    /** The overlay's depth rate line: maps a second as measured, and the governor's target. */
    static String depthRateLine(float mapsPerSecond, int target) {
        return String.format(Locale.US, "Depth %.1f/s (target %d)", mapsPerSecond, target);
    }

    private static String msPer(long totalNs, long count) {
        return String.format("%.2f", totalNs / (double)count / 1000000.0);
    }

    public Surface getInputSurface() {
        return inputSurface;
    }

    // May run on any thread, the frame loop picks the counter up on its own
    @Override
    public void onFrameAvailable(SurfaceTexture st) {
        pendingFrames.incrementAndGet();
    }

    /**
     * Stops the frame loop and destroys the OpenXR session. The codec-facing
     * surface stays valid until cleanup(). The join is bounded by one
     * xrWaitFrame period plus teardown.
     */
    public void prepareForStop() {
        stopping = true;

        if (renderThread != null) {
            try {
                renderThread.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (renderThread.isAlive()) {
                LimeLog.warning("XR render thread did not stop in time");
            }
        }
    }

    /**
     * Releases the surface handed to MediaCodec. Only call after the codec
     * has been released. A frame loop that has not stopped yet is still
     * reading the SurfaceTexture, so in that case the release is left for it
     * to do on its way out.
     */
    public void cleanup() {
        boolean releaseNow;
        synchronized (teardownLock) {
            releaseNow = renderThread == null || renderThreadDone;
            if (!releaseNow) {
                releaseOnExit = true;
            }
        }
        if (releaseNow) {
            releaseSurfaces();
        }
    }

    // The last thing the frame loop thread does, whichever way it ended
    private void finishRenderThread() {
        boolean release;
        synchronized (teardownLock) {
            renderThreadDone = true;
            release = releaseOnExit;
        }
        if (release) {
            releaseSurfaces();
        }
    }

    private void releaseSurfaces() {
        if (inputSurface != null) {
            inputSurface.release();
            inputSurface = null;
        }
        if (surfaceTexture != null) {
            surfaceTexture.release();
            surfaceTexture = null;
        }
    }
}
