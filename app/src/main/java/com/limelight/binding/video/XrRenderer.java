package com.limelight.binding.video;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.os.Process;
import android.preference.PreferenceManager;
import android.view.Surface;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.ByteArrayOutputStream;
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
    // hit point and a button mask, this side turns that into host events. The
    // slots in that array and the ids the panel reports are the IN_ and
    // SETTING_ values in XrShared, so both sides read them off the same file.
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

    // The baked room that ships with the app, mesh and texture atlas
    private static final String ROOM_DIR = "rooms";
    private static final String ROOM_MESH_FILE = "psx_cinema.room";
    private static final String ROOM_TEXTURE_FILE = "psx_cinema.png";
    // The layout as it shipped before the bands, kept only to read an old saved
    // cell as the environment it meant at the time
    private static final int[] LEGACY_CELL_IDS = {
            PreferenceConfiguration.VR_ENV_PASSTHROUGH,
            PreferenceConfiguration.VR_ENV_VOID,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 1,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 2,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 3,
            PreferenceConfiguration.VR_ENV_MINIMAL_ROOM,
            PreferenceConfiguration.VR_ENV_PSX_CINEMA,
    };

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
    private final AtomicReference<ByteBuffer> pendingCogScreenTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogDisplayTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCog3dTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogRoomTab = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingCogButton = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingLockShut = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingLockOpen = new AtomicReference<>();
    // The baked room, read on the same thread as the art above. The native side
    // shows the minimal room in its place until both of these have landed.
    private final AtomicReference<ByteBuffer> pendingRoomMesh = new AtomicReference<>();
    private final AtomicReference<ByteBuffer> pendingRoomTexture = new AtomicReference<>();
    private volatile int roomMeshBytes;
    private volatile int roomTextureWidth;
    private volatile int roomTextureHeight;
    private String[] environmentFiles = new String[0];
    private XrPanels panels;
    private volatile int environmentChoice = ENV_CELL_VOID;
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
        // The exit prompt was confirmed, so the session is to end
        void onVrExit();
    }

    public void setInputListener(InputListener listener) {
        this.inputListener = listener;
    }

    private static native void nativeSetFileLog(String path, int level);
    // envResTier is the EnvResTier the room renders at: 0 low, 1 standard,
    // 2 high, 3 ultra
    private native long nativeInit(Activity activity, int width, int height, int stereoMode,
                                   boolean depthDebug, int convergence, int depthScale,
                                   boolean handTracking, int sharpenMode, boolean perfOverlay,
                                   boolean ambilight, int ambiLevel, boolean roomLight,
                                   int envResTier);
    private native void nativeSetCaptureDir(long ctx, String dir);
    private native int nativeGetTexId(long ctx);
    private native ByteBuffer nativeGetModelInput(long ctx);
    private native ByteBuffer nativeGetModelOutput(long ctx);
    private native long nativeCaptureDepthInput(long ctx, float[] texMatrix);
    private native long nativeFinishDepthCapture(long ctx);
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
    private native void nativeUploadRoomModel(long ctx, ByteBuffer mesh, int length);
    private native void nativeUploadRoomTexture(long ctx, ByteBuffer pixels, int width, int height);
    private native void nativeUploadPicker(long ctx, ByteBuffer grid, ByteBuffer button);
    private native void nativeUploadCog(long ctx, ByteBuffer screenTab, ByteBuffer displayTab,
                                        ByteBuffer tab3d, ByteBuffer roomTab, ByteBuffer button);
    private native void nativeUploadKeyboard(long ctx, ByteBuffer lower, ByteBuffer upper,
                                             ByteBuffer symbols, ByteBuffer buttonIcon,
                                             float[] keyRects, int[] codesLower,
                                             int[] codesUpper, int[] codesSymbols);
    private native void nativeUploadExit(long ctx, ByteBuffer button, ByteBuffer promptPlain,
                                         ByteBuffer promptExitHot, ByteBuffer promptCancelHot);
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

                nativeCtx = nativeInit(activity, videoWidth, videoHeight, prefs.vrDepthMode,
                        prefs.vrDepthDebug, prefs.vrConvergence, prefs.vrDepthScale,
                        prefs.vrHandTracking, prefs.vrSharpening, prefs.enablePerfOverlay,
                        prefs.vrAmbilight, prefs.vrAmbilightLevel, prefs.vrRoomLight,
                        prefs.vrEnvResTier);
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
     * frame loop hands over a captured frame and carries on submitting.
     */
    private void startDepthThread(final Activity activity) {
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
            // The frame loop only queued the readback. This is where it is
            // waited on and turned into the model input, on the thread that
            // has no frame to miss.
            long finish = nativeFinishDepthCapture(nativeCtx);
            boolean ok = finish >= 0 && source.estimate();
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

            captureNs += lastCaptureNs + finish;
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
        // The context is freed the moment this returns, and the thread uses
        // it, so a slow inference is waited out however long it takes rather
        // than left running on memory that is about to go
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
                    pointer, gaze, inputState);
            dispatchInput();

            boolean newFrame = pendingFrames.getAndSet(0) > 0;
            if (newFrame) {
                surfaceTexture.updateTexImage();
                surfaceTexture.getTransformMatrix(texMatrix);

                if (depthReady) {
                    if ((videoFrameIndex % cadence) == 0) {
                        startDepthCapture();
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

            ByteBuffer roomMesh = pendingRoomMesh.getAndSet(null);
            if (roomMesh != null) {
                nativeUploadRoomModel(nativeCtx, roomMesh, roomMeshBytes);
            }
            ByteBuffer roomTexture = pendingRoomTexture.getAndSet(null);
            if (roomTexture != null) {
                nativeUploadRoomTexture(nativeCtx, roomTexture, roomTextureWidth,
                        roomTextureHeight);
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
            String[] found = prefsContext.getAssets().list(XrPanels.ENVIRONMENT_DIR);
            if (found != null) {
                Arrays.sort(found);
                environmentFiles = Arrays.copyOf(found,
                        Math.min(found.length, XrPanels.MAX_PHOTOS));
            }
        } catch (IOException e) {
            LimeLog.warning("No environments: " + e);
        }
        panels = new XrPanels(prefsContext, environmentFiles);

        SharedPreferences saved = PreferenceManager.getDefaultSharedPreferences(prefsContext);
        int id = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, -1);
        if (id < 0) {
            // An install from before the ids has a cell instead, which only
            // means anything read against the layout it was written under. The
            // old key is left where it is, since nothing costs less than a
            // stale int and an older build can still start on it.
            int legacy = saved.getInt(PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING, -1);
            if (legacy >= 0 && legacy < LEGACY_CELL_IDS.length) {
                id = LEGACY_CELL_IDS[legacy];
                saved.edit()
                        .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, id)
                        .apply();
            }
        }

        int cell = cellForId(id);
        if (!cellExists(cell)) {
            // Never picked one, so the passthrough checkbox decides: on gives
            // the room, off gives black. A photo only shows once it is chosen.
            cell = prefs.vrPassthrough ? ENV_CELL_PASSTHROUGH : ENV_CELL_VOID;
        }
        environmentChoice = cell;
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;
        nativeSetEnvironment(nativeCtx, cell, false);

        final int startPhoto = isRoomCell(cell) ? -1 : cell - ENV_CELL_FIRST_PHOTO;
        Thread loader = new Thread() {
            @Override
            public void run() {
                buildPanelArt();
                loadRoomAssets();
                if (startPhoto >= 0) {
                    decodePhoto(startPhoto);
                }
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
        // Same for the 3D rows with stereo turned off in settings
        boolean stereoOk = prefConfig != null && prefConfig.vrDepthMode != DEPTH_MODE_OFF;
        ByteBuffer[] tabs = panels.buildCogTabs(curveOk, stereoOk);
        pendingCogScreenTab.set(tabs[0]);
        pendingCogDisplayTab.set(tabs[1]);
        pendingCog3dTab.set(tabs[2]);
        pendingCogRoomTab.set(tabs[3]);
        pendingCogButton.set(panels.buildCogButton());

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

    // A cell that is a fully 3d room rather than a photo or a plain background
    private static boolean isRoomCell(int cell) {
        return cell == ENV_CELL_MINIMAL_ROOM || cell == ENV_CELL_PSX_CINEMA;
    }

    // A cell is worth switching to if it is one of the fixed ones or a photo
    // that actually shipped in the assets. The fixed cells come first, so one
    // bound covers both.
    private boolean cellExists(int cell) {
        return cell >= 0 && cell < ENV_CELL_FIRST_PHOTO + environmentFiles.length;
    }

    // The two places where cells and saved ids meet. Everything else in here
    // works in cells, and only the preference speaks ids.
    private static int idForCell(int cell) {
        if (cell >= ENV_CELL_FIRST_PHOTO
                && cell < ENV_CELL_FIRST_PHOTO + XrPanels.MAX_PHOTOS) {
            return PreferenceConfiguration.VR_ENV_FIRST_PHOTO + (cell - ENV_CELL_FIRST_PHOTO);
        }
        switch (cell) {
            case ENV_CELL_PASSTHROUGH: return PreferenceConfiguration.VR_ENV_PASSTHROUGH;
            case ENV_CELL_VOID: return PreferenceConfiguration.VR_ENV_VOID;
            case ENV_CELL_MINIMAL_ROOM: return PreferenceConfiguration.VR_ENV_MINIMAL_ROOM;
            case ENV_CELL_PSX_CINEMA: return PreferenceConfiguration.VR_ENV_PSX_CINEMA;
            default: return -1;
        }
    }

    private static int cellForId(int id) {
        if (id >= PreferenceConfiguration.VR_ENV_FIRST_PHOTO
                && id < PreferenceConfiguration.VR_ENV_FIRST_PHOTO + XrPanels.MAX_PHOTOS) {
            return ENV_CELL_FIRST_PHOTO + (id - PreferenceConfiguration.VR_ENV_FIRST_PHOTO);
        }
        switch (id) {
            case PreferenceConfiguration.VR_ENV_PASSTHROUGH: return ENV_CELL_PASSTHROUGH;
            case PreferenceConfiguration.VR_ENV_VOID: return ENV_CELL_VOID;
            case PreferenceConfiguration.VR_ENV_MINIMAL_ROOM: return ENV_CELL_MINIMAL_ROOM;
            case PreferenceConfiguration.VR_ENV_PSX_CINEMA: return ENV_CELL_PSX_CINEMA;
            default: return -1;
        }
    }

    private boolean backgroundVisible() {
        // Only a photo has anything behind it. Asking for it on a room cell
        // would leave whichever photo was decoded last showing through.
        return environmentChoice >= ENV_CELL_FIRST_PHOTO
                && environmentChoice < ENV_CELL_FIRST_PHOTO + environmentFiles.length
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
        passthroughOn = cell == ENV_CELL_PASSTHROUGH;

        final int photo = isRoomCell(cell) ? -1 : cell - ENV_CELL_FIRST_PHOTO;
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
                .putInt(PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING, idForCell(cell))
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
            // A very large panorama is downsampled on decode: past 4096 across
            // the swapchain gains nothing and the raw bitmap can reach the
            // gigabyte that kills the process
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            in = prefsContext.getAssets().open(
                    XrPanels.ENVIRONMENT_DIR + "/" + environmentFiles[photo]);
            BitmapFactory.decodeStream(in, null, bounds);
            XrPanels.closeQuietly(in);

            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = 1;
            while (Math.max(bounds.outWidth, bounds.outHeight) / opts.inSampleSize > 4096) {
                opts.inSampleSize *= 2;
            }

            in = prefsContext.getAssets().open(
                    XrPanels.ENVIRONMENT_DIR + "/" + environmentFiles[photo]);
            Bitmap bitmap = BitmapFactory.decodeStream(in, null, opts);
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
            XrPanels.closeQuietly(in);
        }
    }

    /**
     * The baked room and its texture atlas. Both are parked for the frame loop
     * to hand over, since that thread owns the GL context and is the one that
     * builds the geometry. Either failing leaves the pair unset, and the cell
     * shows the minimal room instead of anything broken.
     */
    private void loadRoomAssets() {
        ByteBuffer mesh = readAsset(ROOM_DIR + "/" + ROOM_MESH_FILE);
        if (mesh == null) {
            return;
        }

        InputStream in = null;
        try {
            in = prefsContext.getAssets().open(ROOM_DIR + "/" + ROOM_TEXTURE_FILE);
            Bitmap atlas = BitmapFactory.decodeStream(in);
            if (atlas == null) {
                LimeLog.warning("Room texture " + ROOM_TEXTURE_FILE + " did not decode");
                return;
            }
            roomTextureWidth = atlas.getWidth();
            roomTextureHeight = atlas.getHeight();
            ByteBuffer pixels = XrPanels.toBuffer(atlas);
            atlas.recycle();

            roomMeshBytes = mesh.remaining();
            pendingRoomMesh.set(mesh);
            pendingRoomTexture.set(pixels);
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Room texture " + ROOM_TEXTURE_FILE + " failed: " + e);
        } finally {
            XrPanels.closeQuietly(in);
        }
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
     * Asks the GPU for a downscaled copy of the frame just latched and wakes
     * the depth thread. Only this stays on the frame loop, since it has to
     * sample the video texture this context owns, and it only queues work:
     * the depth thread waits for the pixels itself, in nativeFinishDepthCapture.
     */
    private void startDepthCapture() {
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
        float warpMs;
        synchronized (nativeLock) {
            if (nativeCtx == 0) {
                return "";
            }
            warpMs = nativeGetWarpGpuMs(nativeCtx);
        }
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("Warp GPU: %.2f ms", warpMs));
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
