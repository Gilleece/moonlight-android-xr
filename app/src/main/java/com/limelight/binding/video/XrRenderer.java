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
import android.os.Process;
import android.preference.PreferenceManager;
import android.text.TextUtils;
import android.view.Surface;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.binding.input.EyeTrackingPermission;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.preferences.XrDisplayRates;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
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
    // Whether the 3D is on, as the frame before said. The bar and the 3D tab
    // can switch it off for the rest of the session, and while it is off the
    // model is not fed, so it sits idle until it comes back on.
    private volatile boolean stereoLive = true;
    // The eye tracking permission is not refused, or not the platform's to
    // grant. Look to point only works with it.
    private volatile boolean gazeAllowed = true;

    // Controller pointer. The native side does the ray maths and hands back a
    // hit point and a button mask, this side turns that into host events. The
    // slots in that array and the ids the panel reports are the IN_ and
    // SETTING_ values in XrShared, so both sides read them off the same file.
    private final float[] inputState = new float[IN_SLOTS];
    private int heldButtons;
    // The head's yaw against the screen, for the virtual surround. Written by
    // the frame loop and read by the audio thread once a block.
    private volatile float headYaw;
    private InputListener inputListener;
    private Context prefsContext;
    private PreferenceConfiguration prefConfig;

    // The baked rooms that ship with the app, a mesh and its atlases each,
    // named by the picker cell that shows them in roomMeshFile and
    // roomAtlasFiles below
    private static final String ROOM_DIR = "rooms";

    // Panel art on its way to the GPU. XrPanels draws it on the loader thread
    // and it waits here for the frame loop, which owns the GL context.
    private final AtomicReference<ByteBuffer> pendingKbLower = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbUpper = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbSymbols = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingKbButton = new AtomicReference<>();
    // Built next to the art and read on the frame loop when it uploads
    private volatile float[] kbKeyRects;
    private volatile int[] kbCodesLower;
    private volatile int[] kbCodesUpper;
    private volatile int[] kbCodesSymbols;

    private final AtomicReference<ByteBuffer> pendingExitButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitPlain = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingExitHot = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCancelHot = new AtomicReference<>();

    private final AtomicReference<ByteBuffer> pendingPickerArt = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingEnvButton = new AtomicReference<>();
    // Every sheet of the settings panel, in COG_ART_ order
    private final AtomicReference<ByteBuffer[]> pendingCogSheets = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogButton = new AtomicReference<>();
    // The percents beside the Room tab's tracks, drawn on the frame loop when
    // the frame says one has moved, and the values last drawn
    private XrPanels.Readout roomReadout;
    private final int[] readoutDrawn = { -1, -1, -1 };
    private final int[] readoutWanted = new int[READOUT_VALUES];
    private final AtomicReference<ByteBuffer> pendingLockShut = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingLockOpen = new AtomicReference<>();
    // The 3D switch's two faces, only drawn in a session with stereo to switch
    private final AtomicReference<ByteBuffer> pendingStereoOff = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingStereoOn = new AtomicReference<>();
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
        void onVrButton(int button, boolean down);
        void onVrScroll(int clicks);
        // A key from the in world keyboard. Unicode with the shift already
        // applied, or backspace, tab, enter and space as their control codes.
        void onVrKey(int code);
        // The exit prompt was confirmed, so the session is to end
        void onVrExit();
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
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
     * Told when a VR session could not be started at all, so the activity can
     * do something visible about it rather than stream into a window the
     * headset's shell never shows. Called off the main thread.
     */
    public interface SessionListener {
        void onVrUnavailable();
    }

    private static native void nativeSetFileLog(String path, int level);
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
                                          float[] out);
    private native void nativeSetScreenPose(long ctx, float[] pose);
    // The room's assets name the picker cell they belong to, which the native
    // side turns into its own room style
    private native void nativeUploadRoomModel(long ctx, ByteBuffer mesh, int length, int cell);
    private native void nativeUploadRoomAtlas(long ctx, ByteBuffer atlas, int cell, int slot);
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
    private native void nativeUploadKeyboard(long ctx, ByteBuffer lower, ByteBuffer upper,
                                             ByteBuffer symbols, ByteBuffer buttonIcon,
                                             float[] keyRects, int[] codesLower,
                                             int[] codesUpper, int[] codesSymbols);
    private native void nativeUploadExit(long ctx, ByteBuffer button, ByteBuffer promptPlain,
                                         ByteBuffer promptExitHot, ByteBuffer promptCancelHot);
    private native boolean nativeGetCylinderSupported(long ctx);
    private native void nativeUploadLock(long ctx, ByteBuffer shut, ByteBuffer open);
    private native void nativeUploadStereoButton(long ctx, ByteBuffer off, ByteBuffer on);
    private native void nativeSetEnvironment(long ctx, int choice);
    private native void nativeUploadOverlay(long ctx, ByteBuffer pixels, int width, int height);
    private native float nativeGetWarpGpuMs(long ctx);
    // The rate the display is on, and the one the session last asked for, 0
    // when it has not asked
    private native float nativeGetDisplayRate(long ctx);
    private native float nativeGetAskedRate(long ctx);
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
                    FileLog.event("depth model "+depthSpec.name+", "+depthRoute.label()
                            +", map "+mapSize);
                    startDepthThread(activity, depthSpec, depthRoute);
                }

                initOk[0] = true;
                initLatch.countDown();

                runFrameLoop(prefs);

                stopDepthThread();

                // Tear down on the same thread that owns the GL context, and
                // under the lock so a stats report cannot land on a context
                // that is halfway through being freed. The SurfaceTexture
                // and Surface stay alive for the codec until cleanup().
                synchronized (nativeLock) {
                    long ctx = nativeCtx;
                    nativeCtx = 0;
                    nativeDestroy(ctx);
                }
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
                        +" maps/s, frames skipped while busy "+skipped);
                lastDepthSkips = (int)skipped;
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

    private void runFrameLoop(PreferenceConfiguration prefs) {
        float distance = prefs.vrDistance / 10.0f;
        float quadWidth = prefs.vrScreenSize / 10.0f;
        float curvature = prefs.vrCurvature / 100.0f;
        // Stored as tenths of a percent of frame width
        float separation = prefs.vrStereoSeparation / 1000.0f;
        boolean eyeSwap = prefs.vrEyeSwap;
        boolean pointer = prefs.vrPointer;
        boolean gaze = prefs.vrGaze;
        int cadence = Math.max(1, prefs.vrInferenceCadence);

        long ageFrames = 0, ageNs = 0, ageSamples = 0, worstAgeNs = 0;
        // When the 3D last came back on. Until a map captured since then is
        // up, the live one is from before it went off, and its age says how
        // long the 3D was off rather than how far behind the depth is.
        long stereoBackNs = 0;

        while (!stopping) {
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                break;
            }
            if (r == FRAME_IDLE) {
                // Native side slept already while the session is not running
                continue;
            }

            // Read fresh each frame rather than once on the way in: the panel's
            // row writes it back to this same object, and the space is picked
            // from it on both sides of the frame, so a press takes effect on
            // the next one with no native state to keep in step.
            boolean headLocked = prefs.vrHeadLocked;

            nativeUpdateInput(nativeCtx, distance, quadWidth, curvature, headLocked,
                    pointer, gaze && gazeAllowed, inputState);
            headYaw = inputState[IN_HEAD_YAW];
            dispatchInput();
            updateRoomReadout();

            // Switched off, the warp draws flat and the model is left idle.
            // Back on, it wants a map of what is showing now, so the frame in
            // hand is captured at once whatever the cadence says.
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
                    if ((videoFrameIndex % cadence) == 0 || stereoBack) {
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
                startDepthCapture();
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

            ByteBuffer kbLower = pendingKbLower.getAndSet(null);
            ByteBuffer kbUpper = pendingKbUpper.getAndSet(null);
            ByteBuffer kbSymbols = pendingKbSymbols.getAndSet(null);
            ByteBuffer kbButton = pendingKbButton.getAndSet(null);
            if (kbLower != null || kbUpper != null || kbSymbols != null || kbButton != null) {
                nativeUploadKeyboard(nativeCtx, kbLower, kbUpper, kbSymbols, kbButton,
                        kbKeyRects, kbCodesLower, kbCodesUpper, kbCodesSymbols);
            }

            ByteBuffer exitButton = pendingExitButton.getAndSet(null);
            ByteBuffer exitPlain = pendingExitPlain.getAndSet(null);
            ByteBuffer exitHot = pendingExitHot.getAndSet(null);
            ByteBuffer cancelHot = pendingCancelHot.getAndSet(null);
            if (exitButton != null || exitPlain != null || exitHot != null || cancelHot != null) {
                nativeUploadExit(nativeCtx, exitButton, exitPlain, exitHot, cancelHot);
            }

            ByteBuffer shut = pendingLockShut.getAndSet(null);
            ByteBuffer open = pendingLockOpen.getAndSet(null);
            if (shut != null && open != null) {
                nativeUploadLock(nativeCtx, shut, open);
            }

            ByteBuffer stereoOff = pendingStereoOff.getAndSet(null);
            ByteBuffer stereoOnArt = pendingStereoOn.getAndSet(null);
            if (stereoOff != null && stereoOnArt != null) {
                nativeUploadStereoButton(nativeCtx, stereoOff, stereoOnArt);
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
    }

    /**
     * Settles on a starting environment, then hands the slow half to another
     * thread: reading a room and drawing the panels take long enough that doing
     * it here would hold up the first frame and hang the shell on its loading
     * screen.
     */
    private void startEnvironment(PreferenceConfiguration prefs) {
        panels = new XrPanels(prefsContext);

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
        roomReadout = new XrPanels.Readout();

        final int startRoom = cell;
        final int roomTicketAtStart;
        synchronized (roomLock) {
            roomTicketAtStart = ++roomTicket;
        }
        Thread loader = new Thread() {
            @Override
            public void run() {
                buildPanelArt();
                loadRoomAssets(startRoom, roomTicketAtStart);
            }
        };
        loader.setName("Video - XR Environment");
        loader.start();
    }

    // Every panel, drawn once and parked for the frame loop
    private void buildPanelArt() {
        pendingPickerArt.set(panels.buildPickerGrid());
        pendingEnvButton.set(panels.buildEnvButton());
        ByteBuffer[] locks = panels.buildLockIcons();
        if (locks != null) {
            pendingLockShut.set(locks[0]);
            pendingLockOpen.set(locks[1]);
        }

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

        XrPanels.Keyboard keyboard = panels.buildKeyboard();
        kbKeyRects = keyboard.keyRects;
        kbCodesLower = keyboard.codesLower;
        kbCodesUpper = keyboard.codesUpper;
        kbCodesSymbols = keyboard.codesSymbols;
        pendingKbLower.set(keyboard.lower);
        pendingKbUpper.set(keyboard.upper);
        pendingKbSymbols.set(keyboard.symbols);
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
        // setting follows it rather than disagreeing with what is on screen
        PreferenceManager.getDefaultSharedPreferences(prefsContext).edit()
                .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING,
                        EnvironmentIds.idForCell(cell))
                .putBoolean(PreferenceConfiguration.VR_PASSTHROUGH_PREF_STRING, passthroughOn)
                .apply();
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

            // Cleared here as well as being written once natively, so a frame
            // that lands while the activity is on its way out cannot ask twice
            if (inputState[IN_EXIT] != 0.0f) {
                inputState[IN_EXIT] = 0.0f;
                inputListener.onVrExit();
            }
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

        int setting = (int)inputState[IN_SETTING];
        if (setting >= 0) {
            applySetting(setting, (int)inputState[IN_SETTING_VALUE],
                    (int)inputState[IN_SETTING_ROOM]);
        }
    }

    // Redraws the percents beside the Room tab's tracks when the frame says
    // one has moved, and hands the strip straight up, since this is the thread
    // with the GL context. The native side only shows a strip drawn from the
    // values in force, so a stale one never reaches the panel.
    private void updateRoomReadout() {
        if (inputState[IN_READOUT] < 0.0f || roomReadout == null) {
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
        nativeUploadCogReadout(nativeCtx, roomReadout.draw(readoutWanted), readoutWanted);
        System.arraycopy(readoutWanted, 0, readoutDrawn, 0, READOUT_VALUES);
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
        float warpMs, displayHz, askedHz;
        synchronized (nativeLock) {
            if (nativeCtx == 0) {
                return "";
            }
            warpMs = nativeGetWarpGpuMs(nativeCtx);
            displayHz = nativeGetDisplayRate(nativeCtx);
            askedHz = nativeGetAskedRate(nativeCtx);
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
