package com.limelight.binding.video;

import android.app.Activity;
import android.graphics.SurfaceTexture;
import android.view.Surface;

import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

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

    private long nativeCtx;
    private Thread renderThread;
    private Thread depthThread;
    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;

    private final AtomicInteger pendingFrames = new AtomicInteger(0);
    private final float[] texMatrix = new float[16];
    private volatile boolean stopping;

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

    private native long nativeInit(Activity activity, int width, int height, int stereoMode,
                                   boolean depthDebug);
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
                                       boolean headLocked, float separation, boolean eyeSwap);
    private native void nativeDestroy(long ctx);

    public boolean start(final Activity activity, final int videoWidth, final int videoHeight,
                         final PreferenceConfiguration prefs) {
        final CountDownLatch initLatch = new CountDownLatch(1);
        final boolean[] initOk = new boolean[1];

        renderThread = new Thread() {
            @Override
            public void run() {
                nativeCtx = nativeInit(activity, videoWidth, videoHeight, prefs.vrDepthMode,
                        prefs.vrDepthDebug);
                if (nativeCtx == 0) {
                    initLatch.countDown();
                    return;
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
        int cadence = Math.max(1, prefs.vrInferenceCadence);

        long videoFrames = 0;

        while (!stopping) {
            int r = nativeWaitBeginFrame(nativeCtx);
            if (r == FRAME_EXIT) {
                break;
            }
            if (r == FRAME_IDLE) {
                // Native side slept already while the session is not running
                continue;
            }

            boolean newFrame = pendingFrames.getAndSet(0) > 0;
            if (newFrame) {
                surfaceTexture.updateTexImage();
                surfaceTexture.getTransformMatrix(texMatrix);

                if (depthReady && (videoFrames++ % cadence) == 0) {
                    handOffDepthFrame();
                }
            }
            nativeEndFrame(nativeCtx, newFrame, texMatrix, distance, quadWidth, curvature,
                    headLocked, separation, eyeSwap);
        }
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

        synchronized (depthLock) {
            depthPending = true;
            depthLock.notify();
        }
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
