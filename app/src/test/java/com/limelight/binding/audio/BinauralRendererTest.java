package com.limelight.binding.audio;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.IOException;
import java.util.Random;

/**
 * The render against the filters it should be using, the rear treatment, and
 * what a block does with the head moving. The host has no native library, so
 * this is the Java loop; test_audio.c holds the C kernels to the same sum.
 */
public class BinauralRendererTest {

    static final int RATE = 48000;
    // A 5 ms packet, what the stream sends
    static final int FRAMES = 240;
    static final float SIDE_GAIN = (float) Math.sqrt(0.5);
    private static final double AMPLITUDE = 8000.0;

    static short[][] silence(int channels, int frames) {
        return new short[channels][frames];
    }

    static short[] noise(int length, long seed) {
        Random random = new Random(seed);
        short[] out = new short[length];
        for (int i = 0; i < length; i++) {
            out[i] = (short) (random.nextInt(16001) - 8000);
        }
        return out;
    }

    private static short[] sine(int length, double hz) {
        short[] out = new short[length];
        for (int i = 0; i < length; i++) {
            out[i] = (short) Math.round(AMPLITUDE * Math.sin(2.0 * Math.PI * hz * i / RATE));
        }
        return out;
    }

    static short[] interleave(short[][] channels) {
        int count = channels.length;
        int frames = channels[0].length;
        short[] in = new short[count * frames];
        for (int frame = 0; frame < frames; frame++) {
            for (int channel = 0; channel < count; channel++) {
                in[frame * count + channel] = channels[channel][frame];
            }
        }
        return in;
    }

    static short[] render(BinauralRenderer renderer, short[][] channels, float yawRadians) {
        int frames = channels[0].length;
        short[] out = new short[2 * frames];
        renderer.render(interleave(channels), frames, yawRadians, out);
        return out;
    }

    private static BinauralRenderer configured(int channels) throws IOException {
        BinauralRenderer renderer = new BinauralRenderer();
        renderer.configure(channels, RATE, HrtfTest.kemar());
        return renderer;
    }

    // What the block should be, from the definition of convolution
    private static double convolved(short[] signal, float[] filter, int at) {
        double sum = 0.0;
        for (int tap = 0; tap < filter.length; tap++) {
            if (at - tap >= 0) {
                sum += filter[tap] * signal[at - tap];
            }
        }
        return sum;
    }

    private static void checkAgainstFilters(short[] signal, float[][] pair, float gain,
                                            short[] out) {
        for (int frame = 0; frame < signal.length; frame++) {
            assertEquals("left " + frame, gain * convolved(signal, pair[0], frame),
                    out[2 * frame], 1.0);
            assertEquals("right " + frame, gain * convolved(signal, pair[1], frame),
                    out[2 * frame + 1], 1.0);
        }
    }

    @Test
    public void frontLeftIsPlacedAtThirtyDegreesLeft() throws IOException {
        Hrtf hrtf = HrtfTest.kemar();
        BinauralRenderer renderer = new BinauralRenderer();
        renderer.configure(6, RATE, hrtf);
        short[] signal = noise(3 * FRAMES, 7);
        short[] out = new short[6 * FRAMES];
        // Three blocks, so the tail has to carry from one to the next
        for (int block = 0; block < 3; block++) {
            short[][] channels = silence(6, FRAMES);
            System.arraycopy(signal, block * FRAMES, channels[0], 0, FRAMES);
            short[] rendered = render(renderer, channels, 0.0f);
            System.arraycopy(rendered, 0, out, block * 2 * FRAMES, 2 * FRAMES);
        }
        checkAgainstFilters(signal, hrtf.pairFor(-30.0f, RATE), 1.0f, out);
    }

    @Test
    public void sideLeftIsPlacedAtNinetyDegreesLeftInSevenOne() throws IOException {
        Hrtf hrtf = HrtfTest.kemar();
        BinauralRenderer renderer = new BinauralRenderer();
        renderer.configure(8, RATE, hrtf);
        short[] signal = noise(FRAMES, 11);
        short[][] channels = silence(8, FRAMES);
        System.arraycopy(signal, 0, channels[6], 0, FRAMES);
        short[] out = render(renderer, channels, 0.0f);
        checkAgainstFilters(signal, hrtf.pairFor(-90.0f, RATE), SIDE_GAIN, out);
    }

    @Test
    public void theHeadYawTurnsTheSpeakers() throws IOException {
        Hrtf hrtf = HrtfTest.kemar();
        BinauralRenderer renderer = new BinauralRenderer();
        renderer.configure(6, RATE, hrtf);
        short[] signal = noise(FRAMES, 17);
        short[][] channels = silence(6, FRAMES);
        System.arraycopy(signal, 0, channels[2], 0, FRAMES);
        // The head 40 degrees left of the screen hears the centre 40 degrees
        // to its right
        short[] out = render(renderer, channels, (float) Math.toRadians(40.0));
        checkAgainstFilters(signal, hrtf.pairFor(40.0f, RATE), SIDE_GAIN, out);
    }

    @Test
    public void everyFrameComesBackInTheSameCall() throws IOException {
        Hrtf hrtf = HrtfTest.kemar();
        int onset = peakOf(hrtf.pairFor(0.0f, RATE)[0]);
        for (int frames : new int[] { 240, 480 }) {
            BinauralRenderer renderer = new BinauralRenderer();
            renderer.configure(6, RATE, hrtf, BinauralRenderer.REAR_DELAY_MS, frames);
            short[][] channels = silence(6, frames);
            channels[2][0] = (short) AMPLITUDE;
            short[] in = interleave(channels);
            short[] out = new short[2 * frames];
            renderer.render(in, frames, 0.0f, out);
            // An impulse at the first frame answers inside this block, at
            // the filter's own onset, not a block later
            int at = 0;
            for (int frame = 1; frame < frames; frame++) {
                if (Math.abs(out[2 * frame]) > Math.abs(out[2 * at])) {
                    at = frame;
                }
            }
            assertEquals(frames + " frames", onset, at);
            assertTrue("level " + out[2 * at], Math.abs(out[2 * at]) > 100);
            // Well inside the first 5 ms
            assertTrue("onset " + onset, onset < FRAMES / 2);
        }
    }

    @Test
    public void aTurningHeadDoesNotClick() throws IOException {
        BinauralRenderer renderer = configured(6);
        // Slow enough that the sine's own steps are small, so a filter change
        // without a crossfade would stand out
        short[] signal = sine(2 * FRAMES, 100.0);
        short[] out = new short[4 * FRAMES];
        float[] yaws = { 0.0f, (float) Math.toRadians(10.0) };
        for (int block = 0; block < 2; block++) {
            short[][] channels = silence(6, FRAMES);
            System.arraycopy(signal, block * FRAMES, channels[0], 0, FRAMES);
            System.arraycopy(signal, block * FRAMES, channels[1], 0, FRAMES);
            short[] rendered = render(renderer, channels, yaws[block]);
            System.arraycopy(rendered, 0, out, block * 2 * FRAMES, 2 * FRAMES);
        }
        checkNoStepAtTheJoin(out);
    }

    @Test
    public void theRearChannelsAreNotDelayed() throws IOException {
        Hrtf hrtf = HrtfTest.kemar();
        assertEquals(0.0f, BinauralRenderer.REAR_DELAY_MS, 0.0f);
        // An untreated channel peaks where its filter does
        assertEquals("front", peakOf(hrtf.pairFor(-30.0f, RATE)[0]),
                impulsePeak(hrtf, 0, BinauralRenderer.REAR_DELAY_MS));
        // and a rear one in the same place, give or take the sample or two
        // the low pass drags a filter this short along
        int rear = impulsePeak(hrtf, 4, BinauralRenderer.REAR_DELAY_MS);
        int expected = peakOf(lowPassed(hrtf.pairFor(-135.0f, RATE)[0]));
        assertTrue("rear at " + rear + ", expected " + expected, Math.abs(rear - expected) <= 1);
    }

    @Test
    public void aRearDelayCanStillBeAskedFor() throws IOException {
        Hrtf hrtf = HrtfTest.kemar();
        int unDelayed = peakOf(lowPassed(hrtf.pairFor(-135.0f, RATE)[0]));
        int rear = impulsePeak(hrtf, 4, 8.0f);
        int expected = unDelayed + Math.round(8.0f * RATE / 1000.0f);
        assertTrue("rear at " + rear + ", expected " + expected, Math.abs(rear - expected) <= 1);
        // and no further than the most the property may set
        int most = impulsePeak(hrtf, 4, 50.0f);
        int capped = unDelayed + Math.round(BinauralRenderer.REAR_DELAY_MAX_MS * RATE / 1000.0f);
        assertTrue("rear at " + most + ", expected " + capped, Math.abs(most - capped) <= 1);
    }

    @Test
    public void theRearChannelsLoseTheirTopEnd() throws IOException {
        Hrtf hrtf = HrtfTest.kemar();
        // The low pass hardly touches 500 Hz and halves 10 kHz, and the rear
        // gain is in both
        double at500 = predictedLevel(hrtf, -135.0f, 500.0, SIDE_GAIN, true);
        assertEquals("rear at 500 Hz", at500, measuredLevel(hrtf, 4, 500.0), 0.03 * at500);
        double at10k = predictedLevel(hrtf, -135.0f, 10000.0, SIDE_GAIN, true);
        assertEquals("rear at 10 kHz", at10k, measuredLevel(hrtf, 4, 10000.0), 0.03 * at10k);
        // The front pair is left alone
        double front = predictedLevel(hrtf, -30.0f, 10000.0, 1.0f, false);
        assertEquals("front at 10 kHz", front, measuredLevel(hrtf, 0, 10000.0), 0.03 * front);
    }

    @Test
    public void aResetForgetsTheTail() throws IOException {
        BinauralRenderer renderer = configured(6);
        short[] signal = noise(FRAMES, 13);
        short[][] channels = silence(6, FRAMES);
        System.arraycopy(signal, 0, channels[0], 0, FRAMES);
        short[] first = render(renderer, channels, 0.0f);
        renderer.reset();
        short[] again = render(renderer, channels, 0.0f);
        for (int i = 0; i < first.length; i++) {
            assertEquals("sample " + i, first[i], again[i]);
        }
    }

    @Test(expected = IllegalArgumentException.class)
    public void stereoIsNotASurroundLayout() throws IOException {
        new BinauralRenderer().configure(2, RATE, HrtfTest.kemar());
    }

    // The filter behind the rear treatment's one pole
    private static float[] lowPassed(float[] filter) {
        double a = 1.0 - Math.exp(-2.0 * Math.PI * BinauralRenderer.REAR_LOWPASS_HZ / RATE);
        float[] out = new float[filter.length + 64];
        double state = 0.0;
        for (int i = 0; i < out.length; i++) {
            state += a * ((i < filter.length ? filter[i] : 0.0) - state);
            out[i] = (float) state;
        }
        return out;
    }

    static int peakOf(float[] filter) {
        int at = 0;
        for (int i = 1; i < filter.length; i++) {
            if (Math.abs(filter[i]) > Math.abs(filter[at])) {
                at = i;
            }
        }
        return at;
    }

    // Where the left ear peaks after a single sample on one channel
    private static int impulsePeak(Hrtf hrtf, int channel, float rearDelayMs) {
        int frames = 2048;
        BinauralRenderer renderer = new BinauralRenderer();
        renderer.configure(6, RATE, hrtf, rearDelayMs, 0);
        short[][] channels = silence(6, frames);
        channels[channel][0] = (short) AMPLITUDE;
        short[] out = render(renderer, channels, 0.0f);
        int at = 0;
        for (int frame = 1; frame < frames; frame++) {
            if (Math.abs(out[2 * frame]) > Math.abs(out[2 * at])) {
                at = frame;
            }
        }
        return at;
    }

    // The steady level of a sine on one channel once the filters have filled
    private static double measuredLevel(Hrtf hrtf, int channel, double hz) {
        int frames = 4096;
        int from = 1024;
        BinauralRenderer renderer = new BinauralRenderer();
        renderer.configure(6, RATE, hrtf);
        short[][] channels = silence(6, frames);
        channels[channel] = sine(frames, hz);
        short[] out = render(renderer, channels, 0.0f);
        double sum = 0.0;
        for (int frame = from; frame < frames; frame++) {
            sum += (double) out[2 * frame] * out[2 * frame];
        }
        return Math.sqrt(sum / (frames - from));
    }

    // What that level should be: the filter at that frequency and the
    // channel's gain, and for a rear channel the one pole and the rear gain
    private static double predictedLevel(Hrtf hrtf, float azimuth, double hz, float gain,
                                         boolean rear) {
        float[] filter = hrtf.pairFor(azimuth, RATE)[0];
        double real = 0.0;
        double imaginary = 0.0;
        for (int i = 0; i < filter.length; i++) {
            double angle = -2.0 * Math.PI * hz * i / RATE;
            real += filter[i] * Math.cos(angle);
            imaginary += filter[i] * Math.sin(angle);
        }
        double level = Math.hypot(real, imaginary) * gain * AMPLITUDE / Math.sqrt(2.0);
        if (rear) {
            double a = 1.0 - Math.exp(-2.0 * Math.PI * BinauralRenderer.REAR_LOWPASS_HZ / RATE);
            double turn = 2.0 * Math.PI * hz / RATE;
            double onePole = a / Math.hypot(1.0 - (1.0 - a) * Math.cos(turn),
                    (1.0 - a) * Math.sin(turn));
            level *= onePole * Math.pow(10.0, BinauralRenderer.REAR_GAIN_DB / 20.0);
        }
        return level;
    }

    // No step at the join between blocks bigger than the signal takes on its
    // own anywhere else
    private static void checkNoStepAtTheJoin(short[] out) {
        for (int ear = 0; ear < 2; ear++) {
            int join = FRAMES;
            int biggest = 0;
            // Skip the start, where the filters are still filling
            for (int frame = 200; frame < 2 * FRAMES; frame++) {
                if (frame == join) {
                    continue;
                }
                int step = Math.abs(out[2 * frame + ear] - out[2 * (frame - 1) + ear]);
                biggest = Math.max(biggest, step);
            }
            int atJoin = Math.abs(out[2 * join + ear] - out[2 * (join - 1) + ear]);
            assertTrue("ear " + ear + " step " + atJoin + " against " + biggest,
                    atJoin <= biggest);
        }
    }
}
