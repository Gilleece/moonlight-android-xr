package com.limelight.binding.audio;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;
import java.util.Arrays;

/**
 * Renders a 5.1 or 7.1 block to two ears: each speaker but the LFE is
 * convolved with the KEMAR filters for where it stands around the listener,
 * and the lot is summed into a left and a right channel. The LFE has no
 * direction worth hearing, so it goes to both ears at 0.707.
 *
 * <p>Speaker azimuths are clockwise from the middle of the screen and the head
 * yaw is added to them, so turning the head left swings the speakers right
 * and the sound stays with the picture. When the head has moved since the
 * last block, the block is rendered through both the old filters and the new
 * and crossfaded, so a turn never clicks.
 *
 * <p>Every frame handed in comes back out of the same call: there is no block
 * of latency, only the filters' own onset of about a millisecond.
 *
 * <p>The rear pair is low passed and turned down slightly, which is most of
 * what the ear reads as behind. There is room for a short delay on it as well,
 * but that is off by default here (see {@link #REAR_DELAY_MS}).
 *
 * <p>Samples and filters sit in direct buffers that the C kernel reads in
 * place. The FFT kernel keeps the spectra of the whole filter ring and is only
 * told which two ring entries each speaker falls between. If that context
 * cannot be built the direct C loop runs instead, and without the library at
 * all the same sum runs in Java, which is how the host tests work.
 */
public final class BinauralRenderer {

    /**
     * How late the rear pair arrives, in ms. A few ms more would push the
     * rear channels further behind, but a game's rear cues (footsteps, a
     * shot from behind) have to land with the picture and with everything
     * else, so it is off. Debug builds can try values over a property.
     */
    static final float REAR_DELAY_MS = 0.0f;
    /** The most the rear delay may be set to. */
    static final float REAR_DELAY_MAX_MS = 20.0f;
    /** Where the rear pair's low pass sits. */
    static final float REAR_LOWPASS_HZ = 5000.0f;
    /** And how far the rear pair is turned down. */
    static final float REAR_GAIN_DB = -1.5f;

    /** Everything but the front pair at -3 dB, as Android's own downmix has it. */
    private static final float SIDE_GAIN = (float) Math.sqrt(0.5);

    /** The unplaced channel, the same slot in both layouts. */
    static final int LFE = 3;

    // The pair that sits behind: the surrounds in 5.1 and the backs in 7.1,
    // which share these two slots
    private static final int REAR_LEFT = 4;
    private static final int REAR_RIGHT = 5;

    // FL FR FC LFE BL BR, the order the stream decodes to
    private static final float[] AZIMUTHS_6 = { -30.0f, 30.0f, 0.0f, 0.0f, -135.0f, 135.0f };
    // FL FR FC LFE BL BR SL SR
    private static final float[] AZIMUTHS_8 = { -30.0f, 30.0f, 0.0f, 0.0f, -150.0f, 150.0f,
            -90.0f, 90.0f };

    /** Below this much head movement a block keeps its filters without a crossfade. */
    private static final float CROSSFADE_DEGREES = 0.25f;

    private final boolean useNative = NativeConvolver.available();

    private Hrtf hrtf;
    private long fft;
    private int channelCount;
    private int sampleRate;
    private int taps;
    private float[] azimuths;
    private float[] gains;

    // Per channel, the last block's tail then this block's samples, every
    // channel in one buffer
    private FloatBuffer history;
    private int historyStride;

    // Where each speaker falls in the ring, which is all the FFT kernel needs
    private IntBuffer filterLow;
    private IntBuffer filterHigh;
    private FloatBuffer filterFar;
    private final int[] entries = new int[2];

    // The filters in use and the previous block's, reversed, a row per
    // channel. Only the direct and Java paths build these.
    private FloatBuffer filterLeft;
    private FloatBuffer filterRight;
    private FloatBuffer oldLeft;
    private FloatBuffer oldRight;
    private FloatBuffer gainsBuffer;
    private boolean haveFilters;
    private float filterYawDegrees;

    // The rear treatment's state, one of each per rear channel
    private float[][] rearDelay;
    private int[] rearDelayAt;
    private float[] rearLowpass;
    private float rearCoefficient;
    private float rearGain;

    // Scratch, sized for the longest block so far. Data moves between arrays
    // and buffers a block at a time.
    private int capacity;
    private float[] channelSamples;
    private float[] renderedLeft;
    private float[] renderedRight;
    private float[] fadedLeft;
    private float[] fadedRight;
    private float[] tailSamples;
    private float[] zeros;
    private FloatBuffer binauralLeft;
    private FloatBuffer binauralRight;
    private FloatBuffer fadingLeft;
    private FloatBuffer fadingRight;

    // The Java loop's own scratch, unused with the library
    private float[] samples;
    private float[] earLeft;
    private float[] earRight;
    private float[] sumLeft;
    private float[] sumRight;

    /** Sets up a layout with the default rear delay and no block size hint. */
    public void configure(int channelCount, int sampleRate, Hrtf hrtf) {
        configure(channelCount, sampleRate, hrtf, REAR_DELAY_MS, 0);
    }

    /**
     * Sets up a layout. Call before the first block.
     *
     * @param rearDelayMs how late the rear pair arrives, clamped to 0 to
     *                    {@link #REAR_DELAY_MAX_MS}.
     * @param blockFrames the expected block length, so the scratch is
     *                    allocated here rather than on the first block. 0 if
     *                    unknown.
     */
    public void configure(int channelCount, int sampleRate, Hrtf hrtf, float rearDelayMs,
                          int blockFrames) {
        if (channelCount != 6 && channelCount != 8) {
            throw new IllegalArgumentException("Not a surround layout: " + channelCount);
        }
        this.hrtf = hrtf;
        this.channelCount = channelCount;
        this.sampleRate = sampleRate;
        this.taps = hrtf.taps(sampleRate);
        this.azimuths = channelCount == 6 ? AZIMUTHS_6 : AZIMUTHS_8;
        this.gains = new float[channelCount];
        gainsBuffer = buffer(channelCount);
        for (int channel = 0; channel < channelCount; channel++) {
            gains[channel] = channel < 2 ? 1.0f : SIDE_GAIN;
            gainsBuffer.put(channel, gains[channel]);
        }
        hrtf.prepare(sampleRate);
        historyStride = taps - 1;
        history = buffer(channelCount * historyStride);
        tailSamples = new float[taps - 1];
        release();
        if (useNative) {
            fft = createFft();
        }
        if (fft != 0) {
            filterLow = intBuffer(channelCount);
            filterHigh = intBuffer(channelCount);
            filterFar = buffer(channelCount);
        }
        else {
            // Only the direct and Java paths hold filters of their own
            filterLeft = buffer(channelCount * taps);
            filterRight = buffer(channelCount * taps);
            oldLeft = buffer(channelCount * taps);
            oldRight = buffer(channelCount * taps);
            earLeft = new float[taps];
            earRight = new float[taps];
        }

        float delayMs = Math.max(0.0f, Math.min(REAR_DELAY_MAX_MS, rearDelayMs));
        int delay = Math.round(delayMs * sampleRate / 1000.0f);
        rearDelay = new float[][] { new float[delay], new float[delay] };
        rearDelayAt = new int[2];
        rearLowpass = new float[2];
        rearCoefficient = (float) (1.0 - Math.exp(-2.0 * Math.PI * REAR_LOWPASS_HZ / sampleRate));
        rearGain = (float) Math.pow(10.0, REAR_GAIN_DB / 20.0);

        capacity = 0;
        if (blockFrames > 0) {
            ensureCapacity(blockFrames);
        }
        reset();
    }

    /** Filter length at the configured rate. */
    public int taps() {
        return taps;
    }

    /** The kernel doing the work: FFT, direct or Java. */
    public String kernel() {
        return fft != 0 ? "FFT" : useNative ? "direct" : "Java";
    }

    /** Frees the C context. Configure builds a new one. */
    public void release() {
        if (fft != 0) {
            NativeConvolver.destroyFft(fft);
            fft = 0;
        }
    }

    // The ring is only read while the context takes its spectra, so these two
    // buffers can go as soon as it is built
    private long createFft() {
        int ring = hrtf.ringSize();
        FloatBuffer ringLeft = buffer(ring * taps);
        FloatBuffer ringRight = buffer(ring * taps);
        hrtf.writeRing(sampleRate, ringLeft, ringRight);
        return NativeConvolver.createFft(taps, channelCount, LFE, ring, ringLeft, ringRight);
    }

    /** Forgets everything carried between blocks. */
    public void reset() {
        if (history != null) {
            if (zeros == null || zeros.length < history.capacity()) {
                zeros = new float[history.capacity()];
            }
            history.position(0);
            history.put(zeros, 0, history.capacity());
            history.rewind();
        }
        if (fft != 0) {
            NativeConvolver.resetFft(fft);
        }
        if (rearDelay != null) {
            for (float[] ring : rearDelay) {
                Arrays.fill(ring, 0.0f);
            }
            Arrays.fill(rearDelayAt, 0);
            Arrays.fill(rearLowpass, 0.0f);
        }
        haveFilters = false;
        filterYawDegrees = 0.0f;
    }

    /**
     * Reads {@code frames} interleaved frames from {@code in} and writes as
     * many stereo frames to {@code out}.
     *
     * @param yawRadians how far the head has turned from the screen, positive
     *                   to the left.
     */
    public void render(short[] in, int frames, float yawRadians, short[] out) {
        ensureCapacity(frames);
        int tail = taps - 1;
        // Into the history a channel at a time, the rear pair through its
        // treatment on the way
        for (int channel = 0; channel < channelCount; channel++) {
            float[] into = channelSamples;
            if (channel == REAR_LEFT || channel == REAR_RIGHT) {
                int index = channel - REAR_LEFT;
                for (int frame = 0; frame < frames; frame++) {
                    into[frame] = rear(index, in[frame * channelCount + channel]);
                }
            }
            else {
                for (int frame = 0; frame < frames; frame++) {
                    into[frame] = in[frame * channelCount + channel];
                }
            }
            history.position(channel * historyStride + tail);
            history.put(into, 0, frames);
        }
        history.rewind();

        float yawDegrees = (float) Math.toDegrees(yawRadians);
        boolean crossfade = false;
        if (fft != 0) {
            // A head that has moved gets both sets of filters and a
            // crossfade, so the change never steps the waveform
            crossfade = haveFilters
                    && Math.abs(yawDegrees - filterYawDegrees) > CROSSFADE_DEGREES;
            buildKeys(yawDegrees);
            NativeConvolver.convolveFft(fft, history, historyStride, filterLow, filterHigh,
                    filterFar, gainsBuffer, frames, crossfade, binauralLeft, binauralRight,
                    fadingLeft, fadingRight);
            haveFilters = true;
        }
        else {
            if (!haveFilters) {
                buildFilters(filterLeft, filterRight, yawDegrees);
                haveFilters = true;
            }
            else if (yawDegrees != filterYawDegrees) {
                crossfade = Math.abs(yawDegrees - filterYawDegrees) > CROSSFADE_DEGREES;
                FloatBuffer wasLeft = filterLeft;
                FloatBuffer wasRight = filterRight;
                filterLeft = oldLeft;
                filterRight = oldRight;
                oldLeft = wasLeft;
                oldRight = wasRight;
                buildFilters(filterLeft, filterRight, yawDegrees);
            }
            convolve(filterLeft, filterRight, binauralLeft, binauralRight, frames);
            if (crossfade) {
                convolve(oldLeft, oldRight, fadingLeft, fadingRight, frames);
            }
        }
        filterYawDegrees = yawDegrees;
        read(binauralLeft, 0, renderedLeft, frames);
        read(binauralRight, 0, renderedRight, frames);
        if (crossfade) {
            read(fadingLeft, 0, fadedLeft, frames);
            read(fadingRight, 0, fadedRight, frames);
        }

        for (int frame = 0; frame < frames; frame++) {
            float left = renderedLeft[frame];
            float right = renderedRight[frame];
            if (crossfade) {
                float was = 1.0f - (frame + 1.0f) / frames;
                left += was * (fadedLeft[frame] - left);
                right += was * (fadedRight[frame] - right);
            }
            out[2 * frame] = clamp(left);
            out[2 * frame + 1] = clamp(right);
        }

        for (int channel = 0; channel < channelCount; channel++) {
            int base = channel * historyStride;
            history.position(base + frames);
            history.get(tailSamples, 0, tail);
            history.position(base);
            history.put(tailSamples, 0, tail);
        }
        history.rewind();
    }

    // Where each speaker falls in the ring with the head where it is. The
    // LFE is not placed, so its slot is left alone.
    private void buildKeys(float yawDegrees) {
        for (int channel = 0; channel < channelCount; channel++) {
            if (channel == LFE) {
                continue;
            }
            float far = hrtf.interpolationFor(azimuths[channel] + yawDegrees, sampleRate, entries);
            filterLow.put(channel, entries[0]);
            filterHigh.put(channel, entries[1]);
            filterFar.put(channel, far);
        }
    }

    // A rear speaker is duller and a little quieter, and later if a delay is
    // set, which together tell the ear it is behind
    private float rear(int index, float sample) {
        float[] ring = rearDelay[index];
        float delayed = sample;
        if (ring.length > 0) {
            int at = rearDelayAt[index];
            delayed = ring[at];
            ring[at] = sample;
            rearDelayAt[index] = at + 1 == ring.length ? 0 : at + 1;
        }
        float low = rearLowpass[index] + rearCoefficient * (delayed - rearLowpass[index]);
        rearLowpass[index] = low;
        return rearGain * low;
    }

    private void buildFilters(FloatBuffer left, FloatBuffer right, float yawDegrees) {
        for (int channel = 0; channel < channelCount; channel++) {
            if (channel == LFE) {
                continue;
            }
            hrtf.filterFor(azimuths[channel] + yawDegrees, sampleRate, earLeft, earRight);
            left.position(channel * taps);
            left.put(earLeft, 0, taps);
            left.rewind();
            right.position(channel * taps);
            right.put(earRight, 0, taps);
            right.rewind();
        }
    }

    private void convolve(FloatBuffer left, FloatBuffer right, FloatBuffer outLeft,
                          FloatBuffer outRight, int frames) {
        if (useNative) {
            NativeConvolver.convolve(history, historyStride, left, right, taps, gainsBuffer,
                    channelCount, LFE, frames, outLeft, outRight);
            return;
        }
        javaConvolve(left, right, outLeft, outRight, frames);
    }

    // The direct kernel's sum in Java, over arrays copied out of the buffers.
    // Filters are reversed and history runs forwards, so both indices climb
    // together in the inner loop.
    private void javaConvolve(FloatBuffer left, FloatBuffer right, FloatBuffer outLeft,
                              FloatBuffer outRight, int frames) {
        Arrays.fill(sumLeft, 0, frames, 0.0f);
        Arrays.fill(sumRight, 0, frames, 0.0f);
        int length = taps;
        for (int channel = 0; channel < channelCount; channel++) {
            read(history, channel * historyStride, samples, length - 1 + frames);
            float gain = gains[channel];
            if (channel == LFE) {
                for (int frame = 0; frame < frames; frame++) {
                    float sample = gain * samples[length - 1 + frame];
                    sumLeft[frame] += sample;
                    sumRight[frame] += sample;
                }
                continue;
            }
            read(left, channel * length, earLeft, length);
            read(right, channel * length, earRight, length);
            for (int frame = 0; frame < frames; frame++) {
                float atLeft = 0.0f;
                float atRight = 0.0f;
                for (int tap = 0; tap < length; tap++) {
                    float sample = samples[frame + tap];
                    atLeft += earLeft[tap] * sample;
                    atRight += earRight[tap] * sample;
                }
                sumLeft[frame] += gain * atLeft;
                sumRight[frame] += gain * atRight;
            }
        }
        write(outLeft, sumLeft, frames);
        write(outRight, sumRight, frames);
    }

    private static void read(FloatBuffer from, int at, float[] into, int length) {
        from.position(at);
        from.get(into, 0, length);
        from.rewind();
    }

    private static void write(FloatBuffer into, float[] from, int length) {
        into.position(0);
        into.put(from, 0, length);
        into.rewind();
    }

    private void ensureCapacity(int frames) {
        if (frames <= capacity) {
            return;
        }
        int tail = taps - 1;
        int stride = tail + frames;
        FloatBuffer grown = buffer(channelCount * stride);
        for (int channel = 0; channel < channelCount; channel++) {
            for (int i = 0; i < tail; i++) {
                grown.put(channel * stride + i, history.get(channel * historyStride + i));
            }
        }
        history = grown;
        historyStride = stride;
        channelSamples = new float[frames];
        renderedLeft = new float[frames];
        renderedRight = new float[frames];
        fadedLeft = new float[frames];
        fadedRight = new float[frames];
        binauralLeft = buffer(frames);
        binauralRight = buffer(frames);
        fadingLeft = buffer(frames);
        fadingRight = buffer(frames);
        if (!useNative) {
            samples = new float[stride];
            sumLeft = new float[frames];
            sumRight = new float[frames];
        }
        capacity = frames;
    }

    private static FloatBuffer buffer(int floats) {
        return ByteBuffer.allocateDirect(floats * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
    }

    private static IntBuffer intBuffer(int ints) {
        return ByteBuffer.allocateDirect(ints * 4).order(ByteOrder.nativeOrder()).asIntBuffer();
    }

    private static short clamp(float value) {
        int rounded = Math.round(value);
        if (rounded > Short.MAX_VALUE) {
            return Short.MAX_VALUE;
        }
        if (rounded < Short.MIN_VALUE) {
            return Short.MIN_VALUE;
        }
        return (short) rounded;
    }
}
