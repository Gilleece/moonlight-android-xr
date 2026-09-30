package com.limelight.binding.video;

import android.content.Context;

import java.nio.ByteBuffer;

/**
 * Produces a depth map for the current video frame. Conceptually this is
 * "RGB frame in, single channel depth out": both textures live on the native
 * renderer side, and the direct buffers handed to initialize() are the
 * staging areas those textures are filled from and read into, one input and
 * one output for each pair of staging the renderer cycles through. Nothing
 * here knows about the model or the runtime, so either can be swapped
 * without touching the render path.
 */
public interface DepthSource {
    /**
     * Every buffer is at the session's depth size, which is the input size
     * of the model this source loads, one input and one output per pair.
     *
     * @param inputs  RGB, width by height, float in 0..1, row 0 at the top
     * @param outputs single channel depth, width by height, float, larger is
     *                nearer, arbitrary scale
     */
    boolean initialize(Context context, ByteBuffer[] inputs, ByteBuffer[] outputs);

    /** Runs one inference from that pair's input buffer into its output. */
    boolean estimate(int pair);

    /** Wall time of the last estimate() call. */
    float getLastInferenceMs();

    boolean isGpuAccelerated();

    void release();
}
