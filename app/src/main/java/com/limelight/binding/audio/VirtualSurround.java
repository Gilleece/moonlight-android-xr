package com.limelight.binding.audio;

import java.io.IOException;

/**
 * The virtual surround as AndroidAudioRenderer drives it: a decoded 5.1 or
 * 7.1 block in, a stereo block of the same length in frames out, with the
 * head yaw read once per block. No Android classes, so the tests drive it the
 * same way.
 */
final class VirtualSurround {

    /** Where the KEMAR set lives in the assets. */
    static final String ASSET_DIR = "hrtf/kemar/";

    private final BinauralRenderer renderer;
    private final int channelCount;
    private final HeadYaw headYaw;
    private short[] stereo;

    private VirtualSurround(BinauralRenderer renderer, int channelCount, int samplesPerFrame,
                            HeadYaw headYaw) {
        this.renderer = renderer;
        this.channelCount = channelCount;
        this.headYaw = headYaw;
        this.stereo = new short[2 * samplesPerFrame];
    }

    /**
     * Whether a stream is rendered at all. With the setting off, or a stream
     * that is not 5.1 or 7.1, the audio plays exactly as it always has and
     * nothing here is loaded.
     */
    static boolean appliesTo(boolean enabled, int channelCount) {
        return enabled && (channelCount == 6 || channelCount == 8);
    }

    /**
     * Loads the filters and builds the renderer, at stream setup. The first
     * build also loads the native library.
     *
     * @param rearDelayMs the rear pair's delay, BinauralRenderer.REAR_DELAY_MS
     *                    unless a debug property says otherwise.
     * @throws IOException if the filters cannot be read.
     */
    static VirtualSurround create(int channelCount, int sampleRate, int samplesPerFrame,
                                  Hrtf.Loader loader, HeadYaw headYaw, float rearDelayMs)
            throws IOException {
        Hrtf hrtf = new Hrtf(loader);
        BinauralRenderer renderer = new BinauralRenderer();
        renderer.configure(channelCount, sampleRate, hrtf, rearDelayMs, samplesPerFrame);
        return new VirtualSurround(renderer, channelCount, samplesPerFrame, headYaw);
    }

    /**
     * Renders one decoded block and returns it as stereo. The array is reused
     * for the next block of the same length, so it has to be written out
     * before this is called again.
     */
    short[] render(short[] decoded) {
        int frames = decoded.length / channelCount;
        if (stereo.length != 2 * frames) {
            stereo = new short[2 * frames];
        }
        float yaw = headYaw != null ? headYaw.headYawRadians() : 0.0f;
        if (Float.isNaN(yaw) || Float.isInfinite(yaw)) {
            yaw = 0.0f;
        }
        renderer.render(decoded, frames, yaw, stereo);
        return stereo;
    }

    /** FFT, direct or Java. */
    String kernel() {
        return renderer.kernel();
    }

    int taps() {
        return renderer.taps();
    }

    void release() {
        renderer.release();
    }
}
