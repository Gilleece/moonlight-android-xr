package com.limelight.binding.video;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.preference.PreferenceManager;
import android.view.Surface;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

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

    private static final int FRAME_EXIT = -1;
    private static final int FRAME_IDLE = 0;
    private static final int FRAME_RENDER = 1;

    private static final int DEPTH_MODE_OFF = 0;
    private static final int DEPTH_MODE_MODEL = 6;

    // Averaged over this many inferences before hitting logcat
    private static final int DEPTH_STATS_INTERVAL = 30;
    private static final int DEPTH_AGE_INTERVAL = 300;

    // Matches OVERLAY_WIDTH and OVERLAY_HEIGHT in xr_renderer.c
    private static final int OVERLAY_WIDTH = 768;
    private static final int OVERLAY_HEIGHT = 512;
    private static final float OVERLAY_TEXT_SIZE = 22.0f;
    private static final float OVERLAY_LINE_HEIGHT = 28.0f;

    private long nativeCtx;
    private Thread renderThread;
    private Thread depthThread;
    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;

    private final AtomicInteger pendingFrames = new AtomicInteger(0);
    private final float[] texMatrix = new float[16];
    private volatile boolean stopping;
    private long videoFrameIndex;

    // Handoff to the depth thread. The frame loop fills the model input and
    // sets pending, the depth thread runs inference and uploads the result.
    // If it is still busy when the next frame is due, the frame loop skips
    // rather than waits, so depth just runs at whatever rate it manages.
    private final Object depthLock = new Object();
    private boolean depthPending;
    private boolean depthBusy;
    private boolean depthExit;
    private int skippedFrames;
    private volatile boolean depthReady;
    private volatile long lastCaptureNs;

    // How far behind the picture the depth map is. The map warping a frame was
    // computed from an earlier one, and then reused until the next inference
    // lands, so during camera motion it is spatially offset from the colour it
    // is warping. Measured rather than assumed: these are the frame index and
    // clock reading of the frame the live depth map came from.
    private long captureFrameIndex;
    private long captureFrameNs;
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
    private volatile float lastDepthAgeMs;
    private volatile int lastDepthSkips;

    // Controller pointer. The native side does the ray maths and hands back a
    // hit point and a button mask, this side turns that into host events.
    private static final int IN_HIT = 0;
    private static final int IN_U = 1;
    private static final int IN_V = 2;
    private static final int IN_BUTTONS = 3;
    private static final int IN_SCROLL = 4;
    private static final int IN_POSE_DIRTY = 6;
    // Which setting the panel just changed, or -1. Zero is a real id, so this
    // is a sentinel rather than a zeroed slot.
    private static final int IN_SETTING = 7;
    private static final int IN_POSE = 8;
    private static final int IN_PICKER_PICK = 18;
    private static final int IN_SETTING_VALUE = 19;
    // What the in world keyboard just typed, or -1. Backspace is 8, so the
    // sentinel has to sit below every real code.
    private static final int IN_KEY = 20;
    private static final int IN_SLOTS = 21;
    // Ids the panel reports, matching the SETTING_ constants in xr_renderer.c
    private static final int SETTING_SHARPEN = 0;
    private static final int SETTING_STATS = 1;
    private static final int SETTING_SEPARATION = 2;
    private static final int SETTING_CONVERGENCE = 3;
    private static final int SETTING_RESET_3D = 4;
    private static final int SETTING_AMBILIGHT = 5;
    private static final int SETTING_AMBI_LEVEL = 6;
    // Position, orientation, width, cylinder radius, then the curvature the
    // settings panel asked for
    private static final int POSE_VALUES = 10;
    private final float[] inputState = new float[IN_SLOTS];
    private int heldButtons;
    private InputListener inputListener;
    private Context prefsContext;
    private PreferenceConfiguration prefConfig;

    // The 360 photo shown behind the screen. Decoded off the frame loop and
    // picked up whenever it is ready, so a slow decode cannot delay the first
    // frame and hang the shell on its loading screen.
    private final AtomicReference<ByteBuffer> pendingBackground = new AtomicReference<>();
    private volatile int backgroundWidth;
    private volatile int backgroundHeight;

    // Environment picker, a grid of thumbnails reachable from inside the
    // session. The first two cells are passthrough and an empty black room,
    // then the photos in the assets folder in name order, and after them the
    // 3d rooms. The rooms sit at the end so a saved cell from an older install
    // still means what it used to. Must match the PICKER_ constants in
    // xr_renderer.c.
    private static final String ENVIRONMENT_DIR = "environments";
    private static final String IMAGE_DIR = "images";
    private static final int PICKER_COLS = 4;
    private static final int PICKER_ROWS = 2;
    private static final int PICKER_CELLS = PICKER_COLS * PICKER_ROWS;
    private static final int PICKER_TEX_W = 1024;
    private static final int PICKER_TEX_H = 512;
    private static final int ENV_BUTTON_TEX = 128;
    // The padlock that locks the hands out. Must match LOCK_TEX in xr_renderer.c.
    private static final int LOCK_TEX = 384;
    private static final int CELL_PASSTHROUGH = 0;
    private static final int CELL_VOID = 1;
    private static final int CELL_FIRST_PHOTO = 2;
    // Shared with the 2d side, so it can recognise the cell without reaching
    // in here
    private static final int CELL_MINIMAL_ROOM = PreferenceConfiguration.VR_ENV_MINIMAL_ROOM;
    private static final int MAX_PHOTOS = CELL_MINIMAL_ROOM - CELL_FIRST_PHOTO;

    // The settings panel behind the cog button. Drawn here, placed and dragged
    // natively, so the layout has to be agreed between the two: these must
    // match the COG_ constants in xr_renderer.c.
    private static final int COG_TEX_W = 768;
    private static final int COG_TEX_H = 640;
    private static final float COG_TRACK_L = 0.42f;
    private static final float COG_TRACK_R = 0.93f;
    private static final float COG_TAB_BAR_B = 0.16f;
    // Six rows on the screen tab, so they start a little higher and sit closer
    // together than they did at five
    private static final float COG_ROW_V0 = 0.25f;
    private static final float COG_ROW_STEP = 0.11f;
    private static final float COG_CELL_HALF = 0.045f;
    private static final float COG_RESET_L = 0.35f;
    private static final float COG_RESET_R = 0.65f;
    // Clear of the last row, which reaches 0.80 plus the half band
    private static final float COG_RESET_T = 0.87f;
    private static final float COG_RESET_B = 0.97f;
    // Three tabs, a texture each, all uploaded once so switching is free, and
    // a fourth sheet handed over after them: the screen tab as it reads while
    // a 3d room hangs the picture, which the native side picks for itself.
    private static final int COG_TAB_SCREEN = 0;
    private static final int COG_TAB_DISPLAY = 1;
    private static final int COG_TAB_3D = 2;
    private static final String[] COG_TABS = { "Screen", "Display", "3D" };
    private static final String[] COG_SLIDER_ROWS =
            { "Distance", "Height", "Tilt", "Rotate", "Curve", "Size" };
    private static final int COG_ROW_TILT = 2;
    private static final int COG_ROW_ROTATE = 3;
    private static final int COG_ROW_CURVE = 4;
    // What stands in for those rows in a room, where the wall decides both the
    // placement and the size
    private static final String COG_ROOM_NOTICE =
            "Screen size cannot be changed in a 3D environment. "
                    + "Please choose a different environment to customise the screen size.";
    // Display tab: a label and a row of cells, one of which is in force, and
    // the glow level track under them
    private static final String[] COG_OPTION_ROWS = { "Sharpen", "Stats", "Glow" };
    private static final String[][] COG_OPTION_CELLS = {
            { "Off", "Normal", "Quality" },
            { "Off", "On" },
            { "Off", "On" }
    };
    // Must match COG_DISPLAY_SLIDER_ROW in xr_renderer.c
    private static final int COG_DISPLAY_SLIDER_ROW = 3;
    // 3D tab: two sliders, drawn the same way the screen tab's are. Only
    // values that take effect the moment they move belong on the panel, which
    // is why the depth source itself stays in the 2d settings.
    private static final String[] COG_SLIDER3D_ROWS = { "Depth", "Convergence" };
    // Where the measured comfort cap, which is also the shipped default, falls
    // along the separation track. Must match COG_SEP_MAX and COG_SEP_STEPS in
    // xr_renderer.c.
    private static final float COG_SEP_CAP_T = 5.0f / 15.0f;

    // The in world keyboard. Three sheets of the same layout, one per state,
    // handed over in state order, along with the geometry that goes with them:
    // the native side is given key rectangles and codes and knows nothing else
    // about it. KB_TEX_W, KB_TEX_H and the code values must match the KB_
    // constants in xr_renderer.c.
    private static final int KB_TEX_W = 1120;
    private static final int KB_TEX_H = 448;
    private static final int KB_CODE_SHIFT = -2;
    private static final int KB_CODE_SYMBOLS = -3;
    private static final int KB_CODE_HIDE = -4;
    // Key widths per row, in units where a plain key is 1, and where each row
    // starts. One table for all three states, so every state has to lay its
    // keys out the same way.
    private static final float[][] KB_ROW_WIDTHS = {
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1.5f, 1, 1, 1, 1, 1, 1, 1, 1.5f },
            { 1.5f, 1, 4, 1, 1.5f, 1 }
    };
    // Only the home row is inset, the way it is on a real keyboard
    private static final float[] KB_ROW_INDENT = { 0.0f, 0.0f, 0.5f, 0.0f, 0.0f };
    private static final float KB_ROW_UNITS = 10.0f;
    // Margins and the gap between two keys, all as fractions of the panel
    private static final float KB_PAD_U = 0.012f;
    private static final float KB_PAD_V = 0.030f;
    private static final float KB_GAP_U = 0.005f;
    private static final float KB_GAP_V = 0.014f;

    private static final String[][] KB_LABELS_LOWER = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
            { "a", "s", "d", "f", "g", "h", "j", "k", "l" },
            { "Shift", "z", "x", "c", "v", "b", "n", "m", "Del" },
            { "?123", ",", "space", ".", "Enter", "Hide" }
    };
    private static final int[][] KB_CODES_LOWER = {
            { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
            { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p' },
            { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l' },
            { KB_CODE_SHIFT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    private static final String[][] KB_LABELS_UPPER = {
            { "!", "@", "#", "$", "%", "^", "&", "*", "(", ")" },
            { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
            { "A", "S", "D", "F", "G", "H", "J", "K", "L" },
            { "Shift", "Z", "X", "C", "V", "B", "N", "M", "Del" },
            { "?123", ",", "space", ".", "Enter", "Hide" }
    };
    private static final int[][] KB_CODES_UPPER = {
            { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')' },
            { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P' },
            { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L' },
            { KB_CODE_SHIFT, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    // The brackets row has one slot fewer than a letter row, since the
    // geometry is shared, so tab takes the place shift had
    private static final String[][] KB_LABELS_SYMBOLS = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "@", "#", "$", "%", "&", "*", "-", "+", "(", ")" },
            { "!", "\"", "'", ":", ";", "/", "?", "_", "=" },
            { "Tab", "<", ">", "[", "]", "{", "}", "\\", "Del" },
            { "ABC", ",", "space", ".", "Enter", "Hide" }
    };
    private static final int[][] KB_CODES_SYMBOLS = {
            { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
            { '@', '#', '$', '%', '&', '*', '-', '+', '(', ')' },
            { '!', '"', '\'', ':', ';', '/', '?', '_', '=' },
            { 9, '<', '>', '[', ']', '{', '}', '\\', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };

    private final AtomicReference<ByteBuffer> pendingKbLower = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbUpper = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbSymbols = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbButton = new AtomicReference<>();
    // Built next to the art and read on the frame loop when it uploads
    private volatile float[] kbKeyRects;
    private volatile int[] kbCodesLower;
    private volatile int[] kbCodesUpper;
    private volatile int[] kbCodesSymbols;

    private final AtomicReference<ByteBuffer> pendingPickerArt = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingEnvButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogScreenTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogDisplayTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCog3dTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogRoomTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingLockShut = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingLockOpen = new AtomicReference<>();
    private String[] environmentFiles = new String[0];
    private volatile int environmentChoice = CELL_VOID;
    private volatile boolean passthroughOn;
    // Which photo is in the background swapchain, so switching back to one
    // already loaded costs nothing and the old one stays up during a decode
    private volatile int loadedPhoto = -1;
    private volatile int pendingPhoto = -1;
    private volatile boolean backgroundArrived;
    private final AtomicInteger photoRequest = new AtomicInteger();

    /**
     * Pointer events out of the VR session. Called on the frame loop thread.
     * Buttons are 0 left, 1 right, 2 middle.
     */
    public interface InputListener {
        void onVrPointerMove(float u, float v);
        void onVrButton(int button, boolean down);
        void onVrScroll(int clicks);
        // A key from the in world keyboard. Unicode with the shift already
        // applied, or backspace, tab, enter and space as their control codes.
        void onVrKey(int code);
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    private static native void nativeSetFileLog(String path, int level);
    private native long nativeInit(Activity activity, int width, int height, int stereoMode,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean handTracking, int sharpenMode, boolean perfOverlay,
                                   boolean ambilight, int ambiLevel);
    private native void nativeSetCaptureDir(long ctx, String dir);
    private native int nativeGetTexId(long ctx);
    private native ByteBuffer nativeGetModelInput(long ctx);
    private native ByteBuffer nativeGetModelOutput(long ctx);
    private native long nativeCaptureDepthInput(long ctx, float[] texMatrix);
    private native long nativeUploadDepth(long ctx);
    private native boolean nativeBindDepthContext(long ctx);
    private native void nativeUnbindDepthContext(long ctx);
    private native int nativeWaitBeginFrame(long ctx);
    private native void nativeEndFrame(long ctx, boolean newFrame, float[] texMatrix,
                                       float distance, float quadWidth, float curvature,
                                       boolean headLocked, float separation, boolean eyeSwap,
                                       boolean passthrough);
    private native void nativeUpdateInput(long ctx, float distance, float quadWidth,
                                          float curvature, boolean headLocked,
                                          boolean pointerEnabled, boolean gazeEnabled,
                                          float[] out);
    private native void nativeSetScreenPose(long ctx, float[] pose);
    private native void nativeUploadBackground(long ctx, ByteBuffer pixels, int width, int height);
    private native void nativeUploadPicker(long ctx, ByteBuffer grid, ByteBuffer button);
    private native void nativeUploadCog(long ctx, ByteBuffer screenTab, ByteBuffer displayTab,
                                        ByteBuffer tab3d, ByteBuffer roomTab, ByteBuffer button);
    private native void nativeUploadKeyboard(long ctx, ByteBuffer lower, ByteBuffer upper,
                                             ByteBuffer symbols, ByteBuffer buttonIcon,
                                             float[] keyRects, int[] codesLower,
                                             int[] codesUpper, int[] codesSymbols);
    private native boolean nativeGetCylinderSupported(long ctx);
    private native void nativeUploadLock(long ctx, ByteBuffer shut, ByteBuffer open);
    private native void nativeSetEnvironment(long ctx, int choice, boolean backgroundOn);
    private native void nativeUploadOverlay(long ctx, ByteBuffer pixels, int width, int height);
    private native float nativeGetWarpGpuMs(long ctx);
    private native void nativeDestroy(long ctx);

    public boolean start(final Activity activity, final int videoWidth, final int videoHeight,
                         final PreferenceConfiguration prefs) {
        final CountDownLatch initLatch = new CountDownLatch(1);
        final boolean[] initOk = new boolean[1];

        renderThread = new Thread() {
            @Override
            public void run() {
                // Before init, so everything the session setup finds ends up
                // in the log too
                nativeSetFileLog(FileLog.getLogPath(), FileLog.getLevel());

                nativeCtx = nativeInit(activity, videoWidth, videoHeight, prefs.vrDepthMode,
                        prefs.vrDepthDebug, prefs.vrConvergence, prefs.vrDepthScale,
                        prefs.vrHandTracking, prefs.vrSharpening, prefs.enablePerfOverlay,
                        prefs.vrAmbilight, prefs.vrAmbilightLevel);
                if (nativeCtx == 0) {
                    initLatch.countDown();
                    return;
                }

                prefsContext = activity.getApplicationContext();
                // Held on to rather than only read here: the stats toggle on
                // the panel writes back to this same instance, which is the one
                // the decoder's stats path checks
                prefConfig = prefs;
                restoreScreenPose();
                startEnvironment(prefs);

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
                    startDepthThread(activity);
                }

                initOk[0] = true;
                initLatch.countDown();

                runFrameLoop(prefs);

                stopDepthThread();

                // Tear down on the same thread that owns the GL context.
                // The SurfaceTexture and Surface stay alive for the codec
                // until cleanup().
                long ctx = nativeCtx;
                nativeCtx = 0;
                nativeDestroy(ctx);
            }
        };
        renderThread.setName("Video - XR Renderer");
        renderThread.start();

        boolean initFinished;
        try {
            // Session setup can take a moment on a cold runtime
            initFinished = initLatch.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            initFinished = false;
        }

        if (!initFinished || !initOk[0]) {
            LimeLog.severe("XR renderer init failed");
            prepareForStop();
            cleanup();
            return false;
        }

        LimeLog.info("XR renderer initialized at "+videoWidth+"x"+videoHeight);
        return true;
    }

    /**
     * Inference is longer than a display frame, so it lives on its own
     * thread with its own context in the render context's share group. The
     * frame loop hands over a captured frame and carries on submitting.
     */
    private void startDepthThread(final Activity activity) {
        depthThread = new Thread() {
            @Override
            public void run() {
                if (!nativeBindDepthContext(nativeCtx)) {
                    return;
                }

                DepthSource source = null;
                try {
                    ByteBuffer input = nativeGetModelInput(nativeCtx);
                    ByteBuffer output = nativeGetModelOutput(nativeCtx);
                    if (input == null || output == null) {
                        LimeLog.severe("Depth staging buffers missing");
                        return;
                    }

                    source = new MidasDepthSource();
                    if (!source.initialize(activity, input, output)) {
                        // The depth texture keeps the flat map it was
                        // initialized with, so zero disparity, and the
                        // stream stays watchable
                        LimeLog.severe("Depth source init failed, stereo will be flat");
                        return;
                    }

                    depthReady = true;
                    runDepthLoop(source);
                } finally {
                    depthReady = false;
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

    private void runDepthLoop(DepthSource source) {
        long runs = 0, skipped = 0;
        long inferenceNs = 0, uploadNs = 0, captureNs = 0, worstNs = 0;

        while (true) {
            synchronized (depthLock) {
                while (!depthPending && !depthExit) {
                    try {
                        depthLock.wait();
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
                if (depthExit) {
                    return;
                }
                depthPending = false;
                depthBusy = true;
            }

            long start = System.nanoTime();
            long upload = 0;
            boolean ok = source.estimate();
            if (ok) {
                upload = nativeUploadDepth(nativeCtx);
                publishedFrameIndex = captureFrameIndex;
                publishedFrameNs = captureFrameNs;
            }

            synchronized (depthLock) {
                depthBusy = false;
                skipped += skippedFrames;
                skippedFrames = 0;
            }

            if (!ok) {
                continue;
            }

            captureNs += lastCaptureNs;
            inferenceNs += (long)(source.getLastInferenceMs() * 1000000.0f);
            lastInferenceMs = source.getLastInferenceMs();
            uploadNs += upload;
            long total = System.nanoTime() - start;
            if (total > worstNs) {
                worstNs = total;
            }
            if (++runs == DEPTH_STATS_INTERVAL) {
                LimeLog.info("Depth stage ("+(source.isGpuAccelerated() ? "GPU" : "CPU")
                        +"): capture "+msPer(captureNs, runs)
                        +" ms, inference "+msPer(inferenceNs, runs)
                        +" ms, upload "+msPer(uploadNs, runs)
                        +" ms, worst "+msPer(worstNs, 1)
                        +" ms, frames skipped while busy "+skipped);
                lastDepthSkips = (int)skipped;
                runs = 0;
                skipped = 0;
                captureNs = inferenceNs = uploadNs = worstNs = 0;
            }
        }
    }

    private void stopDepthThread() {
        if (depthThread == null) {
            return;
        }
        synchronized (depthLock) {
            depthExit = true;
            depthLock.notifyAll();
        }
        try {
            depthThread.join(2000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        if (depthThread.isAlive()) {
            LimeLog.warning("XR depth thread did not stop in time");
        }
        depthThread = null;
    }

    private void runFrameLoop(PreferenceConfiguration prefs) {
        float distance = prefs.vrDistance / 10.0f;
        float quadWidth = prefs.vrScreenSize / 10.0f;
        float curvature = prefs.vrCurvature / 100.0f;
        boolean headLocked = prefs.vrHeadLocked;
        // Stored as tenths of a percent of frame width
        float separation = prefs.vrStereoSeparation / 1000.0f;
        boolean eyeSwap = prefs.vrEyeSwap;
        boolean pointer = prefs.vrPointer;
        boolean gaze = prefs.vrGaze;
        int cadence = Math.max(1, prefs.vrInferenceCadence);

        long ageFrames = 0, ageNs = 0, ageSamples = 0, worstAgeNs = 0;

        while (!stopping) {
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                break;
            }
            if (r == FRAME_IDLE) {
                // Native side slept already while the session is not running
                continue;
            }

            nativeUpdateInput(nativeCtx, distance, quadWidth, curvature, headLocked,
                    pointer, gaze, inputState);
            dispatchInput();

            boolean newFrame = pendingFrames.getAndSet(0) > 0;
            if (newFrame) {
                surfaceTexture.updateTexImage();
                surfaceTexture.getTransformMatrix(texMatrix);

                if (depthReady) {
                    if ((videoFrameIndex % cadence) == 0) {
                        handOffDepthFrame();
                    }
                    if (publishedFrameNs != 0) {
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
            // Upload here rather than from the reporting thread, since this is
            // the thread that owns the GL context
            ByteBuffer overlay = pendingOverlay.getAndSet(null);
            if (overlay != null) {
                nativeUploadOverlay(nativeCtx, overlay, OVERLAY_WIDTH, OVERLAY_HEIGHT);
            }

            ByteBuffer grid = pendingPickerArt.getAndSet(null);
            ByteBuffer button = pendingEnvButton.getAndSet(null);
            if (grid != null || button != null) {
                nativeUploadPicker(nativeCtx, grid, button);
            }

            ByteBuffer screenTab = pendingCogScreenTab.getAndSet(null);
            ByteBuffer displayTab = pendingCogDisplayTab.getAndSet(null);
            ByteBuffer tab3d = pendingCog3dTab.getAndSet(null);
            ByteBuffer roomTab = pendingCogRoomTab.getAndSet(null);
            ByteBuffer cog = pendingCogButton.getAndSet(null);
            if (screenTab != null || displayTab != null || tab3d != null
                    || roomTab != null || cog != null) {
                nativeUploadCog(nativeCtx, screenTab, displayTab, tab3d, roomTab, cog);
            }

            ByteBuffer kbLower = pendingKbLower.getAndSet(null);
            ByteBuffer kbUpper = pendingKbUpper.getAndSet(null);
            ByteBuffer kbSymbols = pendingKbSymbols.getAndSet(null);
            ByteBuffer kbButton = pendingKbButton.getAndSet(null);
            if (kbLower != null || kbUpper != null || kbSymbols != null || kbButton != null) {
                nativeUploadKeyboard(nativeCtx, kbLower, kbUpper, kbSymbols, kbButton,
                        kbKeyRects, kbCodesLower, kbCodesUpper, kbCodesSymbols);
            }

            ByteBuffer shut = pendingLockShut.getAndSet(null);
            ByteBuffer open = pendingLockOpen.getAndSet(null);
            if (shut != null && open != null) {
                nativeUploadLock(nativeCtx, shut, open);
            }

            ByteBuffer background = pendingBackground.getAndSet(null);
            if (background != null) {
                nativeUploadBackground(nativeCtx, background, backgroundWidth, backgroundHeight);
                loadedPhoto = pendingPhoto;
                backgroundArrived = true;
                // Only now is there something to show, so this is where a
                // freshly picked environment actually comes up
                nativeSetEnvironment(nativeCtx, environmentChoice, backgroundVisible());
            }

            nativeEndFrame(nativeCtx, newFrame, texMatrix, distance, quadWidth, curvature,
                    headLocked, separation, eyeSwap, passthroughOn);
        }
    }

    /**
     * Settles on a starting environment, then hands the slow half to another
     * thread: a 4096x2048 photo takes long enough to decode that doing it here
     * would hold up the first frame and hang the shell on its loading screen.
     */
    private void startEnvironment(PreferenceConfiguration prefs) {
        try {
            String[] found = prefsContext.getAssets().list(ENVIRONMENT_DIR);
            if (found != null) {
                Arrays.sort(found);
                environmentFiles = Arrays.copyOf(found, Math.min(found.length, MAX_PHOTOS));
            }
        } catch (IOException e) {
            LimeLog.warning("No environments: " + e);
        }

        int cell = PreferenceManager.getDefaultSharedPreferences(prefsContext)
                .getInt(PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING, -1);
        if (!cellExists(cell)) {
            // Never picked one, so the passthrough checkbox decides: on gives
            // the room, off gives black. A photo only shows once it is chosen.
            cell = prefs.vrPassthrough ? CELL_PASSTHROUGH : CELL_VOID;
        }
        environmentChoice = cell;
        passthroughOn = cell == CELL_PASSTHROUGH;
        nativeSetEnvironment(nativeCtx, cell, false);

        final int startPhoto = isRoomCell(cell) ? -1 : cell - CELL_FIRST_PHOTO;
        Thread loader = new Thread() {
            @Override
            public void run() {
                buildPickerArt();
                buildCogArt();
                buildKeyboardArt();
                if (startPhoto >= 0) {
                    decodePhoto(startPhoto);
                }
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    // A cell that is a fully 3d room rather than a photo or a plain background
    private static boolean isRoomCell(int cell) {
        return cell == CELL_MINIMAL_ROOM;
    }

    // A cell is worth switching to if it is one of the fixed ones or a photo
    // that actually shipped in the assets
    private boolean cellExists(int cell) {
        return cell >= 0
                && (cell < CELL_FIRST_PHOTO + environmentFiles.length || isRoomCell(cell));
    }

    private boolean backgroundVisible() {
        // Only a photo has anything behind it. Asking for it on a room cell
        // would leave whichever photo was decoded last showing through.
        return environmentChoice >= CELL_FIRST_PHOTO
                && environmentChoice < CELL_FIRST_PHOTO + environmentFiles.length
                && backgroundArrived;
    }

    /**
     * A cell was picked in the grid. Switching between two photos keeps the
     * old one up until the new one has been decoded, so the room does not
     * blink to black on the way.
     */
    private void chooseEnvironment(int cell) {
        if (!cellExists(cell)) {
            return;
        }
        environmentChoice = cell;
        passthroughOn = cell == CELL_PASSTHROUGH;

        final int photo = isRoomCell(cell) ? -1 : cell - CELL_FIRST_PHOTO;
        if (photo >= 0 && photo != loadedPhoto) {
            Thread loader = new Thread() {
                @Override
                public void run() {
                    decodePhoto(photo);
                }
            };
            loader.setName("Video - XR Environment");
            loader.start();
        }
        nativeSetEnvironment(nativeCtx, cell, backgroundVisible());

        // The grid is a second way to reach the passthrough switch, so the
        // setting follows it rather than disagreeing with what is on screen
        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putInt(PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING, cell)
                .putBoolean(PreferenceConfiguration.VR_PASSTHROUGH_PREF_STRING, passthroughOn)
                .apply();
    }

    private void decodePhoto(int photo) {
        if (photo < 0 || photo >= environmentFiles.length) {
            return;
        }
        // Picking about quickly can leave more than one of these running, and
        // only the last one asked for should reach the swapchain
        int ticket = photoRequest.incrementAndGet();

        InputStream in = null;
        try {
            in = prefsContext.getAssets().open(ENVIRONMENT_DIR + "/" + environmentFiles[photo]);
            Bitmap bitmap = BitmapFactory.decodeStream(in);
            if (bitmap == null || photoRequest.get() != ticket) {
                return;
            }

            ByteBuffer pixels = ByteBuffer.allocateDirect(
                    bitmap.getWidth() * bitmap.getHeight() * 4);
            bitmap.copyPixelsToBuffer(pixels);
            pixels.rewind();

            backgroundWidth = bitmap.getWidth();
            backgroundHeight = bitmap.getHeight();
            bitmap.recycle();
            pendingPhoto = photo;
            pendingBackground.set(pixels);
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Environment " + environmentFiles[photo] + " failed: " + e);
        } finally {
            closeQuietly(in);
        }
    }

    /**
     * Draws the grid and the button that opens it. Java is the only place
     * Android will lay out text, so the labels have to be baked into the
     * texture here rather than drawn in the shader.
     */
    private void buildPickerArt() {
        final float cellW = PICKER_TEX_W / (float)PICKER_COLS;
        final float cellH = PICKER_TEX_H / (float)PICKER_ROWS;
        final float pad = 7.0f;
        // Matches the radius of the hover ring drawn over it, which is a
        // fraction of the cell rather than a pixel count
        final float radius = cellW * 0.125f;

        Bitmap grid = Bitmap.createBitmap(PICKER_TEX_W, PICKER_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(grid);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xE0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, PICKER_TEX_W - 1.0f, PICKER_TEX_H - 1.0f),
                radius * 0.6f, radius * 0.6f, paint);

        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setColor(Color.WHITE);
        label.setTextSize(21.0f);
        label.setTextAlign(Paint.Align.CENTER);

        for (int cell = 0; cell < PICKER_CELLS; cell++) {
            RectF tile = new RectF(
                    (cell % PICKER_COLS) * cellW + pad,
                    (cell / PICKER_COLS) * cellH + pad,
                    (cell % PICKER_COLS + 1) * cellW - pad,
                    (cell / PICKER_COLS + 1) * cellH - pad);

            String name;
            Bitmap thumb = null;
            if (cell == CELL_PASSTHROUGH) {
                name = "Passthrough";
                paint.setColor(0xFF2A3540);
            }
            else if (cell == CELL_VOID) {
                name = "Black void";
                paint.setColor(0xFF090909);
            }
            else if (isRoomCell(cell)) {
                // No photo to preview, so the room is sketched on the tile
                // below once the base colour is down
                name = "Minimal room";
                paint.setColor(0xFF0B0B0E);
            }
            else if (cell - CELL_FIRST_PHOTO < environmentFiles.length) {
                name = labelFor(environmentFiles[cell - CELL_FIRST_PHOTO]);
                thumb = decodeThumb(environmentFiles[cell - CELL_FIRST_PHOTO], (int)tile.height());
                paint.setColor(0xFF1E1E20);
            }
            else {
                continue;
            }

            if (thumb != null) {
                // Scaled to cover and centred, so the middle of the panorama
                // becomes the preview rather than a squashed whole sphere
                BitmapShader shader = new BitmapShader(thumb, Shader.TileMode.CLAMP,
                                                       Shader.TileMode.CLAMP);
                float scale = Math.max(tile.width() / thumb.getWidth(),
                                       tile.height() / thumb.getHeight());
                Matrix m = new Matrix();
                m.setScale(scale, scale);
                m.postTranslate(tile.centerX() - thumb.getWidth() * scale * 0.5f,
                                tile.centerY() - thumb.getHeight() * scale * 0.5f);
                shader.setLocalMatrix(m);
                paint.setShader(shader);
            }
            paint.setStyle(Paint.Style.FILL);
            canvas.drawRoundRect(tile, radius, radius, paint);
            paint.setShader(null);
            if (thumb != null) {
                thumb.recycle();
            }
            if (isRoomCell(cell)) {
                drawRoomTile(canvas, paint, tile, radius);
            }

            // Dark band under the label, clipped to the bottom of the tile so
            // it keeps the rounded corners it sits in
            canvas.save();
            canvas.clipRect(tile.left, tile.bottom - 44.0f, tile.right, tile.bottom);
            paint.setColor(0xC0000000);
            canvas.drawRoundRect(tile, radius, radius, paint);
            canvas.restore();

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2.0f);
            paint.setColor(0x50FFFFFF);
            canvas.drawRoundRect(tile, radius, radius, paint);
            paint.setStyle(Paint.Style.FILL);

            canvas.drawText(name, tile.centerX(), tile.bottom - 15.0f, label);
        }

        pendingPickerArt.set(toBuffer(grid));
        grid.recycle();

        pendingEnvButton.set(toBuffer(buildEnvButton()));

        Bitmap shut = loadIcon("handtracking_locked.png", LOCK_TEX);
        Bitmap open = loadIcon("handtracking_unlocked.png", LOCK_TEX);
        // Both or neither, since one on its own would leave the button blank
        // in half its states
        if (shut != null && open != null) {
            pendingLockShut.set(toBuffer(shut));
            pendingLockOpen.set(toBuffer(open));
        }
        if (shut != null) {
            shut.recycle();
        }
        if (open != null) {
            open.recycle();
        }
    }

    /**
     * The thumbnail for a room cell, drawn rather than photographed: a lit
     * screen on the wall of a bare dark room, with a faint line low down where
     * the floor meets it.
     */
    private void drawRoomTile(Canvas canvas, Paint paint, RectF tile, float radius) {
        canvas.save();
        // Clipped to the tile so nothing leaks past the rounded corners
        Path clip = new Path();
        clip.addRoundRect(tile, radius, radius, Path.Direction.CW);
        canvas.clipPath(clip);

        final float w = tile.width();
        final float h = tile.height();

        // A 16:9 screen sitting in the upper middle, with a wider soft rect
        // behind it standing in for the light it throws on the wall
        float screenW = w * 0.62f;
        float screenH = screenW * 9.0f / 16.0f;
        float screenTop = tile.top + h * 0.24f;
        RectF screen = new RectF(tile.centerX() - screenW * 0.5f, screenTop,
                tile.centerX() + screenW * 0.5f, screenTop + screenH);

        RectF halo = new RectF(screen);
        halo.inset(-w * 0.07f, -h * 0.07f);
        paint.setColor(0x38A6C4F0);
        canvas.drawRoundRect(halo, radius * 0.7f, radius * 0.7f, paint);
        paint.setColor(0xFFDCE6F4);
        canvas.drawRect(screen, paint);

        // Where the floor meets the wall, faint enough to read as a room
        // rather than as a line across the tile
        paint.setColor(0x28FFFFFF);
        float floorY = tile.top + h * 0.78f;
        canvas.drawRect(new RectF(tile.left, floorY, tile.right, floorY + 1.5f), paint);

        canvas.restore();
    }

    // The padlocks and the cog ship as PNGs. Colour carries the state, so there
    // is nothing to tint or dim here, just a decode and a downscale to whatever
    // the swapchain it is headed for wants.
    private Bitmap loadIcon(String fileName, int size) {
        InputStream in = null;
        try {
            in = prefsContext.getAssets().open(IMAGE_DIR + "/" + fileName);
            Bitmap full = BitmapFactory.decodeStream(in);
            if (full == null) {
                LimeLog.warning("Icon " + fileName + " did not decode");
                return null;
            }
            if (full.getWidth() == size && full.getHeight() == size) {
                return full;
            }
            Bitmap scaled = Bitmap.createScaledBitmap(full, size, size, true);
            if (scaled != full) {
                full.recycle();
            }
            return scaled;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Icon " + fileName + " failed: " + e);
            return null;
        } finally {
            closeQuietly(in);
        }
    }

    // A framed landscape, which is about as much as reads at this size
    private Bitmap buildEnvButton() {
        Bitmap button = Bitmap.createBitmap(ENV_BUTTON_TEX, ENV_BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(14.0f, 14.0f, 114.0f, 114.0f), 22.0f, 22.0f, paint);

        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(46.0f, 46.0f, 9.0f, paint);

        Path hills = new Path();
        hills.moveTo(26.0f, 100.0f);
        hills.lineTo(54.0f, 58.0f);
        hills.lineTo(73.0f, 84.0f);
        hills.lineTo(84.0f, 70.0f);
        hills.lineTo(102.0f, 100.0f);
        hills.close();
        canvas.drawPath(hills, paint);

        return button;
    }

    /**
     * The settings panel and the cog that opens it. A texture per tab and one
     * more for the screen tab in a room, all drawn once here, so changing tab
     * in the session picks another swapchain rather than redrawing anything.
     * Only the labels, tracks and cells live in the texture: thumbs and
     * selection rings are quads of their own, so using the panel costs no
     * upload.
     */
    private void buildCogArt() {
        // Curvature needs a layer type the runtime may not offer, and a slider
        // that cannot do anything is better shown greyed than hidden
        boolean curveOk = nativeCtx != 0 && nativeGetCylinderSupported(nativeCtx);
        // Same for the 3D rows with stereo turned off in settings
        boolean stereoOk = prefConfig != null && prefConfig.vrDepthMode != DEPTH_MODE_OFF;

        Bitmap screenTab = buildCogTab(COG_TAB_SCREEN, curveOk, stereoOk);
        pendingCogScreenTab.set(toBuffer(screenTab));
        screenTab.recycle();

        Bitmap displayTab = buildCogTab(COG_TAB_DISPLAY, curveOk, stereoOk);
        pendingCogDisplayTab.set(toBuffer(displayTab));
        displayTab.recycle();

        Bitmap tab3d = buildCogTab(COG_TAB_3D, curveOk, stereoOk);
        pendingCog3dTab.set(toBuffer(tab3d));
        tab3d.recycle();

        Bitmap roomTab = buildCogRoomTab();
        pendingCogRoomTab.set(toBuffer(roomTab));
        roomTab.recycle();

        // Never blank: the drawn gear stands in if the art does not decode
        Bitmap button = loadIcon("settings_icon.png", ENV_BUTTON_TEX);
        if (button == null) {
            button = buildCogButton();
        }
        pendingCogButton.set(toBuffer(button));
        button.recycle();
    }

    // The screen tab as it reads inside a 3d room: the same chrome, and a note
    // where the rows would be, since the room hangs and sizes the picture
    // itself. The native side shows this sheet in place of the screen tab
    // while a room is on.
    private Bitmap buildCogRoomTab() {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawCogChrome(canvas, COG_TAB_SCREEN);
        drawCogRoomNotice(canvas);
        return bitmap;
    }

    private Bitmap buildCogTab(int tab, boolean curveOk, boolean stereoOk) {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawCogChrome(canvas, tab);
        if (tab == COG_TAB_SCREEN) {
            drawCogSliderRows(canvas, curveOk);
        }
        else if (tab == COG_TAB_3D) {
            drawCog3dRows(canvas, stereoOk);
        }
        else {
            drawCogOptionRows(canvas);
        }
        return bitmap;
    }

    // Background and tab bar, the part both tabs have in common. The tab this
    // texture belongs to is the one drawn as current.
    private void drawCogChrome(Canvas canvas, int tab) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, COG_TEX_W - 1.0f, COG_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(25.0f);
        text.setTextAlign(Paint.Align.CENTER);

        final float barB = COG_TAB_BAR_B * COG_TEX_H;
        final float slotW = COG_TEX_W / (float)COG_TABS.length;
        for (int i = 0; i < COG_TABS.length; i++) {
            boolean current = i == tab;
            RectF slot = new RectF(i * slotW + 12.0f, 12.0f, (i + 1) * slotW - 12.0f, barB - 8.0f);

            if (current) {
                paint.setColor(0x28FFFFFF);
                canvas.drawRoundRect(slot, 14.0f, 14.0f, paint);
            }

            text.setColor(current ? Color.WHITE : 0x60FFFFFF);
            canvas.drawText(COG_TABS[i], slot.centerX(),
                    slot.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

            if (current) {
                // The underline is what carries at a glance, the fill alone is
                // too subtle at this size
                paint.setColor(0xEEFFFFFF);
                canvas.drawRect(slot.left + 24.0f, slot.bottom - 4.0f,
                        slot.right - 24.0f, slot.bottom, paint);
            }
        }

        paint.setColor(0x30FFFFFF);
        canvas.drawRect(20.0f, barB, COG_TEX_W - 20.0f, barB + 2.0f, paint);
    }

    // Screen tab: a label and a track per row, and the reset button under them
    private void drawCogSliderRows(Canvas canvas, boolean curveOk) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);

        for (int row = 0; row < COG_SLIDER_ROWS.length; row++) {
            boolean live = row != COG_ROW_CURVE || curveOk;
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;

            text.setColor(live ? Color.WHITE : 0x30FFFFFF);
            // Centred on the row rather than sitting on it, so the label lines
            // up with the track beside it
            canvas.drawText(COG_SLIDER_ROWS[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            track.setColor(live ? 0x66FFFFFF : 0x30FFFFFF);
            canvas.drawLine(COG_TRACK_L * COG_TEX_W, y, COG_TRACK_R * COG_TEX_W, y, track);

            if (row == COG_ROW_TILT || row == COG_ROW_ROTATE) {
                // Marks level, which is where the middle of these two tracks
                // snaps to. The rows that do not snap stay unmarked.
                float midX = (COG_TRACK_L + COG_TRACK_R) * 0.5f * COG_TEX_W;
                float tickHalf = COG_CELL_HALF * COG_TEX_H;
                canvas.drawRect(midX - 2.0f, y - tickHalf, midX + 2.0f, y + tickHalf, tick);
            }
        }

        // A way back for a screen dragged somewhere unrecoverable
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF reset = new RectF(COG_RESET_L * COG_TEX_W, COG_RESET_T * COG_TEX_H,
                COG_RESET_R * COG_TEX_W, COG_RESET_B * COG_TEX_H);
        canvas.drawRoundRect(reset, 14.0f, 14.0f, paint);

        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("Reset", reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    // What the screen tab carries in a room instead of its rows: the reason
    // there are none, centred in the body under the tab bar
    private void drawCogRoomNotice(Canvas canvas) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(0xC0FFFFFF);

        String[] lines = wrapText(COG_ROOM_NOTICE, text, 0.80f * COG_TEX_W);
        float step = (text.descent() - text.ascent()) * 1.4f;
        float middle = (COG_TAB_BAR_B * COG_TEX_H + COG_TEX_H) * 0.5f;
        float y = middle - (lines.length - 1) * step * 0.5f
                - (text.ascent() + text.descent()) * 0.5f;
        for (String line : lines) {
            canvas.drawText(line, COG_TEX_W * 0.5f, y, text);
            y += step;
        }
    }

    // Greedy word wrap, which is all one fixed sentence on a fixed panel needs
    private static String[] wrapText(String message, Paint paint, float width) {
        ArrayList<String> lines = new ArrayList<>();
        StringBuilder line = new StringBuilder();
        for (String word : message.split(" ")) {
            if (line.length() > 0 && paint.measureText(line + " " + word) > width) {
                lines.add(line.toString());
                line.setLength(0);
            }
            if (line.length() > 0) {
                line.append(' ');
            }
            line.append(word);
        }
        if (line.length() > 0) {
            lines.add(line.toString());
        }
        return lines.toArray(new String[0]);
    }

    // 3D tab: the two values worth reaching mid stream. Depth runs past the
    // comfortable range on purpose, with the far end marked, since where that
    // range ends is a matter of eyes rather than of hardware.
    private void drawCog3dRows(Canvas canvas, boolean stereoOk) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(stereoOk ? Color.WHITE : 0x30FFFFFF);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(stereoOk ? 0xCCFFFFFF : 0x30FFFFFF);

        final float trackL = COG_TRACK_L * COG_TEX_W;
        final float trackR = COG_TRACK_R * COG_TEX_W;
        final float tickHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < COG_SLIDER3D_ROWS.length; row++) {
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(COG_SLIDER3D_ROWS[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            // The default sits a third along the depth track and halfway along
            // convergence, and a tick says so on both
            float markT = row == 0 ? COG_SEP_CAP_T : 0.5f;
            float markX = trackL + markT * (trackR - trackL);

            if (row == 0) {
                // Measured on device: past 0.5 percent the depth stops growing
                // and only the strain does, so the rest of the track is drawn
                // as a place you can go rather than one you should
                track.setColor(stereoOk ? 0x66FFFFFF : 0x30FFFFFF);
                canvas.drawLine(trackL, y, markX, y, track);
                track.setColor(stereoOk ? 0x66FFB74D : 0x30FFB74D);
                canvas.drawLine(markX, y, trackR, y, track);

                Paint caption = new Paint(Paint.ANTI_ALIAS_FLAG);
                caption.setTextSize(15.0f);
                caption.setTextAlign(Paint.Align.CENTER);
                caption.setColor(stereoOk ? 0xA0FFB74D : 0x30FFB74D);
                // Just above the next row's hit band, which starts 0.055 down
                // now the rows sit closer together
                canvas.drawText("harder on the eyes", (markX + trackR) * 0.5f,
                        y + 0.04f * COG_TEX_H, caption);
            }
            else {
                track.setColor(stereoOk ? 0x66FFFFFF : 0x30FFFFFF);
                canvas.drawLine(trackL, y, trackR, y, track);
            }

            canvas.drawRect(markX - 2.0f, y - tickHalf, markX + 2.0f, y + tickHalf, tick);
        }

        // A way back from a pair of values that turned out to be unwatchable
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF reset = new RectF(COG_RESET_L * COG_TEX_W, COG_RESET_T * COG_TEX_H,
                COG_RESET_R * COG_TEX_W, COG_RESET_B * COG_TEX_H);
        canvas.drawRoundRect(reset, 14.0f, 14.0f, paint);

        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("Reset", reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

        if (!stereoOk) {
            // Otherwise two dead sliders with no explanation
            Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
            hint.setTextSize(17.0f);
            hint.setTextAlign(Paint.Align.CENTER);
            hint.setColor(0x50FFFFFF);
            canvas.drawText("3D is off in settings", COG_TEX_W * 0.5f,
                    0.62f * COG_TEX_H, hint);
        }
    }

    // Display tab: a label and a row of cells, one press wide each. Which cell
    // is in force and which is under the ray are rings the native side puts
    // over them, so nothing here has to be redrawn when one is chosen.
    private void drawCogOptionRows(Canvas canvas) {
        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextSize(22.0f);
        label.setTextAlign(Paint.Align.LEFT);
        label.setColor(Color.WHITE);

        Paint cellText = new Paint(Paint.ANTI_ALIAS_FLAG);
        cellText.setTextSize(19.0f);
        cellText.setTextAlign(Paint.Align.CENTER);
        cellText.setColor(Color.WHITE);

        Paint cell = new Paint(Paint.ANTI_ALIAS_FLAG);

        final float trackL = COG_TRACK_L * COG_TEX_W;
        final float trackR = COG_TRACK_R * COG_TEX_W;
        final float cellHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < COG_OPTION_ROWS.length; row++) {
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(COG_OPTION_ROWS[row], 0.06f * COG_TEX_W,
                    y - (label.ascent() + label.descent()) * 0.5f, label);

            String[] names = COG_OPTION_CELLS[row];
            float span = (trackR - trackL) / names.length;
            for (int i = 0; i < names.length; i++) {
                // Inset so neighbours read as separate buttons rather than one
                // long strip
                RectF box = new RectF(trackL + i * span + 3.0f, y - cellHalf,
                        trackL + (i + 1) * span - 3.0f, y + cellHalf);

                cell.setStyle(Paint.Style.FILL);
                cell.setColor(0x28FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, cell);
                cell.setStyle(Paint.Style.STROKE);
                cell.setStrokeWidth(2.0f);
                cell.setColor(0x50FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, cell);

                canvas.drawText(names[i], box.centerX(),
                        box.centerY() - (cellText.ascent() + cellText.descent()) * 0.5f,
                        cellText);
            }
        }

        // How strong the glow is, a track under the cells and the only row on
        // this tab that is dragged rather than pressed
        float y = (COG_ROW_V0 + COG_DISPLAY_SLIDER_ROW * COG_ROW_STEP) * COG_TEX_H;
        canvas.drawText("Glow level", 0.06f * COG_TEX_W,
                y - (label.ascent() + label.descent()) * 0.5f, label);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);
        track.setColor(0x66FFFFFF);
        canvas.drawLine(trackL, y, trackR, y, track);

        // Marks the default, halfway, the same way the 3D tab marks its two
        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);
        float midX = (trackL + trackR) * 0.5f;
        canvas.drawRect(midX - 2.0f, y - cellHalf, midX + 2.0f, y + cellHalf, tick);
    }

    // The fallback cog, drawn only when the icon asset is missing. About as
    // much of a gear as reads at this size.
    private Bitmap buildCogButton() {
        Bitmap button = Bitmap.createBitmap(ENV_BUTTON_TEX, ENV_BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float mid = ENV_BUTTON_TEX * 0.5f;
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(10.0f);
        canvas.drawCircle(mid, mid, 34.0f, paint);

        paint.setStrokeWidth(12.0f);
        paint.setStrokeCap(Paint.Cap.ROUND);
        for (int tooth = 0; tooth < 8; tooth++) {
            double angle = tooth * Math.PI / 4.0;
            float dx = (float)Math.cos(angle);
            float dy = (float)Math.sin(angle);
            canvas.drawLine(mid + dx * 34.0f, mid + dy * 34.0f,
                            mid + dx * 48.0f, mid + dy * 48.0f, paint);
        }

        // A ring rather than a filled dot, which reads as a hole through the
        // middle of the gear the way a real one does
        paint.setStrokeCap(Paint.Cap.BUTT);
        paint.setStrokeWidth(8.0f);
        canvas.drawCircle(mid, mid, 14.0f, paint);

        return button;
    }

    /**
     * The in world keyboard: one sheet of art per state, the button that opens
     * it, and the layout the native side hit tests against. All three sheets
     * share one set of key rectangles, so the art and the hit test are built
     * from the same numbers and cannot drift apart.
     */
    private void buildKeyboardArt() {
        kbKeyRects = buildKeyRects();
        kbCodesLower = flatten(KB_CODES_LOWER);
        kbCodesUpper = flatten(KB_CODES_UPPER);
        kbCodesSymbols = flatten(KB_CODES_SYMBOLS);

        Bitmap lower = buildKeyboardSheet(KB_LABELS_LOWER, kbKeyRects);
        pendingKbLower.set(toBuffer(lower));
        lower.recycle();

        Bitmap upper = buildKeyboardSheet(KB_LABELS_UPPER, kbKeyRects);
        pendingKbUpper.set(toBuffer(upper));
        upper.recycle();

        Bitmap symbols = buildKeyboardSheet(KB_LABELS_SYMBOLS, kbKeyRects);
        pendingKbSymbols.set(toBuffer(symbols));
        symbols.recycle();

        Bitmap button = buildKeyboardButton();
        pendingKbButton.set(toBuffer(button));
        button.recycle();
    }

    // Left, top, right and bottom of every key as fractions of the panel, rows
    // top down, keys left to right. The row widths are in key units, so this is
    // where they turn into a place on the texture.
    private static float[] buildKeyRects() {
        int keys = 0;
        for (float[] row : KB_ROW_WIDTHS) {
            keys += row.length;
        }

        float[] rects = new float[keys * 4];
        float unit = (1.0f - 2.0f * KB_PAD_U) / KB_ROW_UNITS;
        float rowHeight = (1.0f - 2.0f * KB_PAD_V) / KB_ROW_WIDTHS.length;
        int at = 0;
        for (int row = 0; row < KB_ROW_WIDTHS.length; row++) {
            float x = KB_ROW_INDENT[row];
            for (int key = 0; key < KB_ROW_WIDTHS[row].length; key++) {
                float w = KB_ROW_WIDTHS[row][key];
                rects[at++] = KB_PAD_U + x * unit + KB_GAP_U * 0.5f;
                rects[at++] = KB_PAD_V + row * rowHeight + KB_GAP_V * 0.5f;
                rects[at++] = KB_PAD_U + (x + w) * unit - KB_GAP_U * 0.5f;
                rects[at++] = KB_PAD_V + (row + 1) * rowHeight - KB_GAP_V * 0.5f;
                x += w;
            }
        }
        return rects;
    }

    private static int[] flatten(int[][] rows) {
        int keys = 0;
        for (int[] row : rows) {
            keys += row.length;
        }

        int[] flat = new int[keys];
        int at = 0;
        for (int[] row : rows) {
            for (int code : row) {
                flat[at++] = code;
            }
        }
        return flat;
    }

    // One state's worth of keys, drawn as caps on the same dark rounded panel
    // the settings use
    private Bitmap buildKeyboardSheet(String[][] labels, float[] rects) {
        Bitmap bitmap = Bitmap.createBitmap(KB_TEX_W, KB_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, KB_TEX_W - 1.0f, KB_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.WHITE);

        int at = 0;
        for (String[] row : labels) {
            for (String label : row) {
                RectF box = new RectF(rects[at] * KB_TEX_W, rects[at + 1] * KB_TEX_H,
                        rects[at + 2] * KB_TEX_W, rects[at + 3] * KB_TEX_H);
                at += 4;

                paint.setStyle(Paint.Style.FILL);
                paint.setColor(0x28FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(2.0f);
                paint.setColor(0x50FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, paint);

                // A single character is what the key types, so it gets the
                // room. The named keys are wordier and have to fit.
                text.setTextSize(label.length() == 1 ? 34.0f : 22.0f);
                canvas.drawText(label, box.centerX(),
                        box.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
            }
        }

        return bitmap;
    }

    // A keyboard outline with a few keys in it, which is about as much as reads
    // at this size
    private Bitmap buildKeyboardButton() {
        Bitmap button = Bitmap.createBitmap(ENV_BUTTON_TEX, ENV_BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(12.0f, 28.0f, 116.0f, 100.0f), 14.0f, 14.0f, paint);

        paint.setStyle(Paint.Style.FILL);
        for (int row = 0; row < 2; row++) {
            float y = 42.0f + row * 16.0f;
            for (int key = 0; key < 4; key++) {
                float x = 26.0f + key * 20.0f;
                canvas.drawRoundRect(new RectF(x, y, x + 14.0f, y + 12.0f), 3.0f, 3.0f, paint);
            }
        }
        canvas.drawRoundRect(new RectF(44.0f, 74.0f, 84.0f, 86.0f), 3.0f, 3.0f, paint);

        return button;
    }

    private static ByteBuffer toBuffer(Bitmap bitmap) {
        ByteBuffer pixels = ByteBuffer.allocateDirect(
                bitmap.getWidth() * bitmap.getHeight() * 4);
        bitmap.copyPixelsToBuffer(pixels);
        pixels.rewind();
        return pixels;
    }

    // Sampled down on the way out of the JPEG, since a full 4096x2048 decode
    // for a 240 pixel tile would cost 32 MB apiece
    private Bitmap decodeThumb(String fileName, int wanted) {
        InputStream in = null;
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            in = prefsContext.getAssets().open(ENVIRONMENT_DIR + "/" + fileName);
            BitmapFactory.decodeStream(in, null, bounds);
            closeQuietly(in);

            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = 1;
            while (bounds.outHeight / (opts.inSampleSize * 2) >= wanted) {
                opts.inSampleSize *= 2;
            }

            in = prefsContext.getAssets().open(ENVIRONMENT_DIR + "/" + fileName);
            return BitmapFactory.decodeStream(in, null, opts);
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Thumbnail " + fileName + " failed: " + e);
            return null;
        } finally {
            closeQuietly(in);
        }
    }

    // spaichingen_hill.jpg becomes Spaichingen Hill
    private static String labelFor(String fileName) {
        int dot = fileName.lastIndexOf('.');
        String base = dot > 0 ? fileName.substring(0, dot) : fileName;
        StringBuilder out = new StringBuilder(base.length());
        boolean wordStart = true;
        for (int i = 0; i < base.length(); i++) {
            char c = base.charAt(i) == '_' ? ' ' : base.charAt(i);
            out.append(wordStart ? Character.toUpperCase(c) : c);
            wordStart = c == ' ';
        }
        return out.toString();
    }

    private static void closeQuietly(InputStream in) {
        if (in != null) {
            try {
                in.close();
            } catch (IOException ignored) {
            }
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

            // Every real code is 8 or more, so anything at zero or above is a
            // key rather than the sentinel
            int key = (int)inputState[IN_KEY];
            if (key >= 0) {
                inputListener.onVrKey(key);
            }
        }

        // A 3d room forces the picture onto its wall, so what comes back while
        // one is on is the wall's placement rather than the user's. Writing it
        // would lose where they had the screen in every other environment.
        if (inputState[IN_POSE_DIRTY] != 0.0f && !isRoomCell(environmentChoice)) {
            saveScreenPose();
        }

        int pick = (int)inputState[IN_PICKER_PICK];
        if (pick >= 0) {
            chooseEnvironment(pick);
        }

        int setting = (int)inputState[IN_SETTING];
        if (setting >= 0) {
            applySetting(setting, (int)inputState[IN_SETTING_VALUE]);
        }
    }

    /**
     * A row on the panel's display or 3D tab was pressed. The native side has
     * already applied it to the running session, this end only has to make it
     * stick and tell whatever else in the app cares.
     */
    private void applySetting(int setting, int value) {
        if (prefsContext == null) {
            return;
        }

        if (setting == SETTING_SHARPEN) {
            String choice = value == 2 ? "quality" : (value == 1 ? "normal" : "off");
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putString(PreferenceConfiguration.VR_SHARPENING_PREF_STRING, choice)
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
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, value)
                    .apply();
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
            // Both at once, since the reset button moved both
            if (prefConfig != null) {
                prefConfig.vrStereoSeparation = PreferenceConfiguration.DEFAULT_VR_SEPARATION;
                prefConfig.vrConvergence = PreferenceConfiguration.DEFAULT_VR_CONVERGENCE;
            }
            PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                    .putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING,
                            PreferenceConfiguration.DEFAULT_VR_SEPARATION)
                    .putInt(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING,
                            PreferenceConfiguration.DEFAULT_VR_CONVERGENCE)
                    .apply();
        }
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
     * Downscales the frame just latched and wakes the depth thread. Only the
     * capture stays on the frame loop, since it has to sample the video
     * texture this context owns, and it is short.
     */
    private void handOffDepthFrame() {
        synchronized (depthLock) {
            if (depthPending || depthBusy) {
                skippedFrames++;
                return;
            }
        }

        lastCaptureNs = nativeCaptureDepthInput(nativeCtx, texMatrix);
        captureFrameIndex = videoFrameIndex;
        captureFrameNs = System.nanoTime();

        synchronized (depthLock) {
            depthPending = true;
            depthLock.notify();
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
        for (String line : (text + '\n' + rendererStats()).split("\n")) {
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
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("Warp GPU: %.2f ms", nativeGetWarpGpuMs(nativeCtx)));
        if (depthReady) {
            sb.append('\n').append(String.format("Depth inference: %.1f ms", lastInferenceMs));
            sb.append('\n').append(String.format("Depth age: %.0f ms", lastDepthAgeMs));
            sb.append('\n').append("Depth frames skipped: ").append(lastDepthSkips);
        }
        return sb.toString();
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
     * has been released.
     */
    public void cleanup() {
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
