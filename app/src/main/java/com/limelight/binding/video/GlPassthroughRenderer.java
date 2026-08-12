package com.limelight.binding.video;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.view.Surface;

import com.limelight.LimeLog;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Routes decoder output through a SurfaceTexture and draws it back to the
 * real display surface with a passthrough shader. Visually identical to the
 * direct path, but it gives us the decoded frame as an OES texture that
 * later stages can actually sample from.
 */
public class GlPassthroughRenderer implements SurfaceTexture.OnFrameAvailableListener {

    private static final int STATS_LOG_INTERVAL_FRAMES = 300;

    private HandlerThread renderThread;
    private Handler handler;

    private EGLDisplay eglDisplay = EGL14.EGL_NO_DISPLAY;
    private EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
    private EGLSurface eglSurface = EGL14.EGL_NO_SURFACE;

    private int oesTextureId;
    private int program;
    private int positionAttrib;
    private int texCoordAttrib;
    private int texMatrixUniform;

    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;

    private final float[] texMatrix = new float[16];
    private volatile boolean stopping;

    private long lastDrawnTimestamp;
    private boolean drawnAnyFrame;

    // Stats over the last logging window
    private long statFrames;
    private long statTotalNs;
    private long statMaxNs;

    private static final String VERTEX_SHADER =
            "attribute vec4 a_position;\n" +
            "attribute vec4 a_texcoord;\n" +
            "uniform mat4 u_texmatrix;\n" +
            "varying vec2 v_texcoord;\n" +
            "void main() {\n" +
            "    gl_Position = a_position;\n" +
            "    v_texcoord = (u_texmatrix * a_texcoord).xy;\n" +
            "}\n";

    private static final String FRAGMENT_SHADER =
            "#extension GL_OES_EGL_image_external : require\n" +
            "precision mediump float;\n" +
            "varying vec2 v_texcoord;\n" +
            "uniform samplerExternalOES u_texture;\n" +
            "void main() {\n" +
            "    gl_FragColor = texture2D(u_texture, v_texcoord);\n" +
            "}\n";

    // Fullscreen triangle strip. The texture matrix from SurfaceTexture
    // handles the vertical flip that video frames need.
    private static final float[] VERTEX_DATA = {
            // x, y, u, v
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
    };

    private FloatBuffer vertexBuffer;

    /**
     * Brings up the GL thread, EGL context and SurfaceTexture. Blocks until
     * initialization finishes. Returns false if anything failed, in which
     * case the caller should fall back to the direct surface path.
     */
    public boolean start(final Surface windowSurface, final int videoWidth, final int videoHeight) {
        renderThread = new HandlerThread("Video - GL Passthrough", Process.THREAD_PRIORITY_DISPLAY);
        renderThread.start();
        handler = new Handler(renderThread.getLooper());

        final CountDownLatch initLatch = new CountDownLatch(1);
        final boolean[] initOk = new boolean[1];

        handler.post(new Runnable() {
            @Override
            public void run() {
                try {
                    initOk[0] = initEgl(windowSurface) && initGl(videoWidth, videoHeight);
                } catch (Exception e) {
                    e.printStackTrace();
                    initOk[0] = false;
                } finally {
                    initLatch.countDown();
                }
            }
        });

        boolean initFinished;
        try {
            initFinished = initLatch.await(2, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            initFinished = false;
        }

        if (!initFinished || !initOk[0]) {
            LimeLog.severe("GL passthrough init failed");
            prepareForStop();
            cleanup();
            return false;
        }

        LimeLog.info("GL passthrough initialized at "+videoWidth+"x"+videoHeight);
        return true;
    }

    public Surface getInputSurface() {
        return inputSurface;
    }

    private boolean initEgl(Surface windowSurface) {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            LimeLog.severe("eglGetDisplay failed");
            return false;
        }

        int[] version = new int[2];
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            LimeLog.severe("eglInitialize failed: "+EGL14.eglGetError());
            return false;
        }

        int[] configAttribs = {
                EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_NONE
        };
        EGLConfig[] configs = new EGLConfig[1];
        int[] numConfigs = new int[1];
        if (!EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfigs, 0) ||
                numConfigs[0] < 1) {
            LimeLog.severe("eglChooseConfig failed: "+EGL14.eglGetError());
            return false;
        }

        int[] contextAttribs = {
                EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
                EGL14.EGL_NONE
        };
        eglContext = EGL14.eglCreateContext(eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT, contextAttribs, 0);
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            LimeLog.severe("eglCreateContext failed: "+EGL14.eglGetError());
            return false;
        }

        int[] surfaceAttribs = { EGL14.EGL_NONE };
        eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, configs[0], windowSurface, surfaceAttribs, 0);
        if (eglSurface == EGL14.EGL_NO_SURFACE) {
            LimeLog.severe("eglCreateWindowSurface failed: "+EGL14.eglGetError());
            return false;
        }

        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            LimeLog.severe("eglMakeCurrent failed: "+EGL14.eglGetError());
            return false;
        }

        return true;
    }

    private boolean initGl(int videoWidth, int videoHeight) {
        program = buildProgram();
        if (program == 0) {
            return false;
        }

        positionAttrib = GLES20.glGetAttribLocation(program, "a_position");
        texCoordAttrib = GLES20.glGetAttribLocation(program, "a_texcoord");
        texMatrixUniform = GLES20.glGetUniformLocation(program, "u_texmatrix");

        vertexBuffer = ByteBuffer.allocateDirect(VERTEX_DATA.length * 4)
                .order(ByteOrder.nativeOrder())
                .asFloatBuffer();
        vertexBuffer.put(VERTEX_DATA).position(0);

        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        oesTextureId = textures[0];
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);

        surfaceTexture = new SurfaceTexture(oesTextureId);
        surfaceTexture.setDefaultBufferSize(videoWidth, videoHeight);
        surfaceTexture.setOnFrameAvailableListener(this, handler);
        inputSurface = new Surface(surfaceTexture);

        return true;
    }

    private int buildProgram() {
        int vs = compileShader(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER);
        int fs = compileShader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
        if (vs == 0 || fs == 0) {
            return 0;
        }

        int prog = GLES20.glCreateProgram();
        GLES20.glAttachShader(prog, vs);
        GLES20.glAttachShader(prog, fs);
        GLES20.glLinkProgram(prog);

        int[] linked = new int[1];
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, linked, 0);

        // Shaders are owned by the program after linking
        GLES20.glDeleteShader(vs);
        GLES20.glDeleteShader(fs);

        if (linked[0] == 0) {
            LimeLog.severe("Program link failed: "+GLES20.glGetProgramInfoLog(prog));
            GLES20.glDeleteProgram(prog);
            return 0;
        }

        return prog;
    }

    private int compileShader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);

        int[] compiled = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, compiled, 0);
        if (compiled[0] == 0) {
            LimeLog.severe("Shader compile failed: "+GLES20.glGetShaderInfoLog(shader));
            GLES20.glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

    // Runs on the render thread since we registered with our own handler
    @Override
    public void onFrameAvailable(SurfaceTexture st) {
        if (stopping) {
            return;
        }

        long startNs = System.nanoTime();

        // Latches the newest queued buffer, dropping any we fell behind on
        surfaceTexture.updateTexImage();

        // If we coalesced a batch above, the leftover callbacks land here
        // with nothing new latched. Skip the redundant redraw.
        long frameTimestamp = surfaceTexture.getTimestamp();
        if (drawnAnyFrame && frameTimestamp == lastDrawnTimestamp) {
            return;
        }
        lastDrawnTimestamp = frameTimestamp;
        drawnAnyFrame = true;

        surfaceTexture.getTransformMatrix(texMatrix);

        // Query each frame in case the SurfaceView gets resized under us
        int[] dims = new int[2];
        EGL14.eglQuerySurface(eglDisplay, eglSurface, EGL14.EGL_WIDTH, dims, 0);
        EGL14.eglQuerySurface(eglDisplay, eglSurface, EGL14.EGL_HEIGHT, dims, 1);
        GLES20.glViewport(0, 0, dims[0], dims[1]);

        GLES20.glUseProgram(program);

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);

        GLES20.glUniformMatrix4fv(texMatrixUniform, 1, false, texMatrix, 0);

        vertexBuffer.position(0);
        GLES20.glVertexAttribPointer(positionAttrib, 2, GLES20.GL_FLOAT, false, 16, vertexBuffer);
        GLES20.glEnableVertexAttribArray(positionAttrib);
        vertexBuffer.position(2);
        GLES20.glVertexAttribPointer(texCoordAttrib, 2, GLES20.GL_FLOAT, false, 16, vertexBuffer);
        GLES20.glEnableVertexAttribArray(texCoordAttrib);

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);

        // Forward the producer's timestamp so SurfaceFlinger keeps the same
        // latch and drop behavior the direct path had
        EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, frameTimestamp);

        if (!EGL14.eglSwapBuffers(eglDisplay, eglSurface)) {
            // Expected once during teardown when the window surface dies
            LimeLog.warning("eglSwapBuffers failed: "+EGL14.eglGetError());
            return;
        }

        long elapsedNs = System.nanoTime() - startNs;
        statFrames++;
        statTotalNs += elapsedNs;
        if (elapsedNs > statMaxNs) {
            statMaxNs = elapsedNs;
        }

        if (statFrames == STATS_LOG_INTERVAL_FRAMES) {
            LimeLog.info(String.format(
                    "GL passthrough: %d frames, avg %.2f ms, max %.2f ms (updateTexImage to swap)",
                    statFrames, statTotalNs / (double)statFrames / 1e6, statMaxNs / 1e6));
            statFrames = 0;
            statTotalNs = 0;
            statMaxNs = 0;
        }
    }

    /**
     * Stops rendering and tears down the EGL objects. Called while the codec
     * still holds the input surface, so that stays alive until cleanup().
     * Safe to call from the UI thread, the join is bounded.
     */
    public void prepareForStop() {
        stopping = true;

        if (handler != null) {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    releaseEgl();
                }
            });
        }

        if (renderThread != null) {
            renderThread.quitSafely();
            try {
                renderThread.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    // Render thread only
    private void releaseEgl() {
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (eglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, eglSurface);
                eglSurface = EGL14.EGL_NO_SURFACE;
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext);
                eglContext = EGL14.EGL_NO_CONTEXT;
            }
            EGL14.eglReleaseThread();
            // Not calling eglTerminate, the display is shared process-wide
            eglDisplay = EGL14.EGL_NO_DISPLAY;
        }
    }

    /**
     * Releases the surface handed to MediaCodec. Only call after the codec
     * itself has been released.
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
