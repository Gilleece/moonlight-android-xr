package com.limelight.binding.audio;

import java.nio.FloatBuffer;
import java.nio.IntBuffer;

/**
 * The convolution in C, in libxr-audio. The Java loop in BinauralRenderer is
 * far too slow for 7.1 on a headset's audio thread.
 *
 * <p>Both sides share direct buffers, so a block is one call and no copies.
 * The library is loaded the first time a renderer is built, which only
 * happens with virtual surround on and a 5.1 or 7.1 stream. Host tests have
 * no library and run the Java loop instead, hence {@link #available}.
 */
final class NativeConvolver {

    private static final boolean LOADED;

    static {
        boolean loaded;
        try {
            System.loadLibrary("xr-audio");
            loaded = true;
        }
        catch (UnsatisfiedLinkError e) {
            loaded = false;
        }
        LOADED = loaded;
    }

    private NativeConvolver() {
    }

    /** Whether the library is there, and so whether anything below may be called. */
    static boolean available() {
        return LOADED;
    }

    /**
     * The direct kernel: every channel through its filters, summed to two ears.
     *
     * @param history      every channel back to back, historyStride floats
     *                     each: the previous block's tail, then this block.
     * @param filtersLeft  channelCount rows of taps floats, reversed.
     * @param lfeChannel   the channel sent to both ears unfiltered.
     * @param outLeft      the ears, overwritten over frames.
     */
    static native void convolve(FloatBuffer history, int historyStride, FloatBuffer filtersLeft,
                                FloatBuffer filtersRight, int taps, FloatBuffer gains,
                                int channelCount, int lfeChannel, int frames, FloatBuffer outLeft,
                                FloatBuffer outRight);

    /**
     * Builds the FFT kernel's context, with the spectrum of every filter.
     *
     * @param ringLeft the ring for one ear, ringSize filters of taps floats,
     *                 reversed. Read here and not kept.
     * @return a handle, or 0 when the filters are too long for the transform
     *         or memory ran out, and the caller uses {@link #convolve}.
     */
    static native long createFft(int taps, int channelCount, int lfeChannel, int ringSize,
                                 FloatBuffer ringLeft, FloatBuffer ringRight);

    static native void destroyFft(long handle);

    /** Forgets the filters built so far. */
    static native void resetFft(long handle);

    /**
     * The same sum as {@link #convolve} through the transform. Every frame
     * passed in comes back in the same call.
     *
     * @param low     per channel, the ring entries the speaker lies between,
     *                with far the fraction of the way. The LFE's are unread.
     * @param withOld also run the block through the previous block's
     *                filters, into oldLeft and oldRight, for a crossfade.
     */
    static native void convolveFft(long handle, FloatBuffer history, int historyStride,
                                   IntBuffer low, IntBuffer high, FloatBuffer far,
                                   FloatBuffer gains, int frames, boolean withOld,
                                   FloatBuffer outLeft, FloatBuffer outRight, FloatBuffer oldLeft,
                                   FloatBuffer oldRight);
}
