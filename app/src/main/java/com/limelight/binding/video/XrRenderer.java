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
    private static final int IN_SLOTS = 20;
    // Ids the panel reports, matching the SETTING_ constants in xr_renderer.c
    private static final int SETTING_SHARPEN = 0;
    private static final int SETTING_STATS = 1;
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
    // the rest are the photos in the assets folder, in name order. Must match
    // the PICKER_ constants in xr_renderer.c.
    private static final String ENVIRONMENT_DIR = "environments";
    private static final String IMAGE_DIR = "images";
    private static final int PICKER_COLS = 3;
    private static final int PICKER_ROWS = 2;
    private static final int PICKER_CELLS = PICKER_COLS * PICKER_ROWS;
    private static final int PICKER_TEX_W = 768;
    private static final int PICKER_TEX_H = 512;
    private static final int ENV_BUTTON_TEX = 128;
    // The padlock that locks the hands out. Must match LOCK_TEX in xr_renderer.c.
    private static final int LOCK_TEX = 384;
    private static final int CELL_PASSTHROUGH = 0;
    private static final int CELL_VOID = 1;
    private static final int CELL_FIRST_PHOTO = 2;
    private static final int MAX_PHOTOS = PICKER_CELLS - CELL_FIRST_PHOTO;

    // The settings panel behind the cog button. Drawn here, placed and dragged
    // natively, so the layout has to be agreed between the two: these must
    // match the COG_ constants in xr_renderer.c.
    private static final int COG_TEX_W = 768;
    private static final int COG_TEX_H = 640;
    private static final float COG_TRACK_L = 0.42f;
    private static final float COG_TRACK_R = 0.93f;
    private static final float COG_TAB_BAR_B = 0.16f;
    private static final float COG_ROW_V0 = 0.27f;
    private static final float COG_ROW_STEP = 0.125f;
    private static final float COG_CELL_HALF = 0.045f;
    private static final float COG_RESET_L = 0.35f;
    private static final float COG_RESET_R = 0.65f;
    private static final float COG_RESET_T = 0.86f;
    private static final float COG_RESET_B = 0.97f;
    // Two tabs, a texture each, both uploaded once so switching is free
    private static final int COG_TAB_SCREEN = 0;
    private static final int COG_TAB_DISPLAY = 1;
    private static final String[] COG_TABS = { "Screen", "Display" };
    private static final String[] COG_SLIDER_ROWS =
            { "Distance", "Height", "Tilt", "Curve", "Size" };
    private static final int COG_ROW_CURVE = 3;
    // Display tab: a label and a row of cells, one of which is in force
    private static final String[] COG_OPTION_ROWS = { "Sharpen", "Stats" };
    private static final String[][] COG_OPTION_CELLS = {
            { "Off", "Normal", "Quality" },
            { "Off", "On" }
    };

    private final AtomicReference<ByteBuffer> pendingPickerArt = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingEnvButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogScreenTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogDisplayTab = new AtomicReference<>();
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
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    private static native void nativeSetFileLog(String path, int level);
    private native long nativeInit(Activity activity, int width, int height, int stereoMode,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean handTracking, int sharpenMode, boolean perfOverlay);
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
                                        ByteBuffer button);
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
                        prefs.vrHandTracking, prefs.vrSharpening, prefs.enablePerfOverlay);
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
            ByteBuffer cog = pendingCogButton.getAndSet(null);
            if (screenTab != null || displayTab != null || cog != null) {
                nativeUploadCog(nativeCtx, screenTab, displayTab, cog);
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
        if (cell < 0 || cell >= CELL_FIRST_PHOTO + environmentFiles.length) {
            // Never picked one, so the passthrough checkbox decides: on gives
            // the room, off gives black. A photo only shows once it is chosen.
            cell = prefs.vrPassthrough ? CELL_PASSTHROUGH : CELL_VOID;
        }
        environmentChoice = cell;
        passthroughOn = cell == CELL_PASSTHROUGH;
        nativeSetEnvironment(nativeCtx, cell, false);

        final int startPhoto = cell - CELL_FIRST_PHOTO;
        Thread loader = new Thread() {
            @Override
            public void run() {
                buildPickerArt();
                buildCogArt();
                if (startPhoto >= 0) {
                    decodePhoto(startPhoto);
                }
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    private boolean backgroundVisible() {
        return environmentChoice >= CELL_FIRST_PHOTO && backgroundArrived;
    }

    /**
     * A cell was picked in the grid. Switching between two photos keeps the
     * old one up until the new one has been decoded, so the room does not
     * blink to black on the way.
     */
    private void chooseEnvironment(int cell) {
        if (cell < 0 || cell >= CELL_FIRST_PHOTO + environmentFiles.length) {
            return;
        }
        environmentChoice = cell;
        passthroughOn = cell == CELL_PASSTHROUGH;

        final int photo = cell - CELL_FIRST_PHOTO;
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
     * The settings panel and the cog that opens it. A texture per tab, both
     * drawn once here, so changing tab in the session picks the other
     * swapchain rather than redrawing anything. Only the labels, tracks and
     * cells live in the texture: thumbs and selection rings are quads of their
     * own, so using the panel costs no upload.
     */
    private void buildCogArt() {
        // Curvature needs a layer type the runtime may not offer, and a slider
        // that cannot do anything is better shown greyed than hidden
        boolean curveOk = nativeCtx != 0 && nativeGetCylinderSupported(nativeCtx);

        Bitmap screenTab = buildCogTab(COG_TAB_SCREEN, curveOk);
        pendingCogScreenTab.set(toBuffer(screenTab));
        screenTab.recycle();

        Bitmap displayTab = buildCogTab(COG_TAB_DISPLAY, curveOk);
        pendingCogDisplayTab.set(toBuffer(displayTab));
        displayTab.recycle();

        // Never blank: the drawn gear stands in if the art does not decode
        Bitmap button = loadIcon("settings_icon.png", ENV_BUTTON_TEX);
        if (button == null) {
            button = buildCogButton();
        }
        pendingCogButton.set(toBuffer(button));
        button.recycle();
    }

    private Bitmap buildCogTab(int tab, boolean curveOk) {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawCogChrome(canvas, tab);
        if (tab == COG_TAB_SCREEN) {
            drawCogSliderRows(canvas, curveOk);
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
        final float half = COG_TEX_W * 0.5f;
        for (int i = 0; i < COG_TABS.length; i++) {
            boolean current = i == tab;
            RectF slot = new RectF(i * half + 12.0f, 12.0f, (i + 1) * half - 12.0f, barB - 8.0f);

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
        }

        if (inputState[IN_POSE_DIRTY] != 0.0f) {
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
     * A row on the panel's display tab was pressed. The native side has
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
