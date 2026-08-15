package com.limelight.binding.video;

import android.content.Context;

import java.nio.ByteBuffer;

/**
 * Produces a depth map for the current video frame. Conceptually this is
 * "RGB frame in, single channel depth out": both textures live on the native
 * renderer side, and the two direct buffers handed to initialize() are the
 * staging areas those textures are filled from and read into. Nothing here
 * knows about the model or the runtime, so either can be swapped without
 * touching the render path.
 */
public interface DepthSource {
    /** Square edge length of both the model input and the depth map. */
    int DEPTH_SIZE = 256;

    /**
     * @param input  RGB, DEPTH_SIZE squared, float in 0..1, row 0 at the top
     * @param output single channel depth, DEPTH_SIZE squared, float, larger
     *               is nearer, arbitrary scale
     */
    boolean initialize(Context context, ByteBuffer input, ByteBuffer output);

    /** Runs one inference over the current contents of the input buffer. */
    boolean estimate();

    /** Wall time of the last estimate() call. */
    float getLastInferenceMs();

    boolean isGpuAccelerated();

    void release();
}
