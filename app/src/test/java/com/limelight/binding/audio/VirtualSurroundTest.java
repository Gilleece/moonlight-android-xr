package com.limelight.binding.audio;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.IOException;

/**
 * The virtual surround as the audio renderer drives it: what it applies to,
 * what a block turns into, and whether each speaker is heard from the side it
 * should be, with the head still and turned.
 *
 * <p>Nobody has listened to it yet, so the placement is checked by the two
 * cues the ear uses most: the near ear is louder (the level difference) and
 * hears it first (the time difference, read off where the two ears' signals
 * line up best).
 */
public class VirtualSurroundTest {

    private static final int RATE = BinauralRendererTest.RATE;
    private static final int FRAMES = BinauralRendererTest.FRAMES;
    // A tenth of a second of 5 ms blocks, and the first two left out of the
    // measurement while the filters fill
    private static final int BLOCKS = 20;
    private static final int SETTLE_FRAMES = 2 * FRAMES;
    // Wider than a head: about a millisecond either way
    private static final int MAX_LAG = 48;

    private static final int LEFT = -1;
    private static final int CENTRE = 0;
    private static final int RIGHT = 1;

    private static VirtualSurround build(int channels, int frames, HeadYaw yaw)
            throws IOException {
        return VirtualSurround.create(channels, RATE, frames, HrtfTest.fileLoader(), yaw,
                BinauralRenderer.REAR_DELAY_MS);
    }

    @Test
    public void onlyFiveOneAndSevenOneWithTheSettingOnAreRendered() {
        // With the setting off nothing is built for any stream, and the
        // audio renderer opens the track it always has
        for (int channels : new int[] { 2, 4, 6, 8 }) {
            assertFalse("off, " + channels, VirtualSurround.appliesTo(false, channels));
        }
        // Stereo is left alone with it on, and so is anything but 5.1 and 7.1
        assertFalse(VirtualSurround.appliesTo(true, 2));
        assertFalse(VirtualSurround.appliesTo(true, 4));
        assertTrue(VirtualSurround.appliesTo(true, 6));
        assertTrue(VirtualSurround.appliesTo(true, 8));
    }

    @Test
    public void aBlockComesBackAsStereoOfTheSameLength() throws IOException {
        for (int channels : new int[] { 6, 8 }) {
            for (int frames : new int[] { 240, 480 }) {
                VirtualSurround surround = build(channels, frames, () -> 0.0f);
                short[] decoded = BinauralRendererTest.noise(channels * frames, frames);
                for (int block = 0; block < 3; block++) {
                    short[] stereo = surround.render(decoded);
                    assertEquals(channels + " channels, " + frames + " frames", 2 * frames,
                            stereo.length);
                }
                // The host tests have no native library
                assertEquals("Java", surround.kernel());
                surround.release();
            }
        }
    }

    @Test
    public void theHeadIsReadOnceABlock() throws IOException {
        final int[] reads = new int[1];
        VirtualSurround surround = build(6, FRAMES, () -> {
            reads[0]++;
            return 0.1f;
        });
        short[] decoded = new short[6 * FRAMES];
        for (int block = 0; block < 5; block++) {
            surround.render(decoded);
        }
        assertEquals(5, reads[0]);
    }

    @Test
    public void aYawThatIsNotANumberFacesTheScreen() throws IOException {
        short[] decoded = BinauralRendererTest.noise(8 * FRAMES, 5);
        short[] facing = build(8, FRAMES, () -> 0.0f).render(decoded).clone();
        short[] broken = build(8, FRAMES, () -> Float.NaN).render(decoded).clone();
        assertArrayEquals(facing, broken);
    }

    // Noise on one channel through the glue, block by block, and the ears
    private static short[] earsFor(int channels, int channel, float yawRadians)
            throws IOException {
        VirtualSurround surround = build(channels, FRAMES, () -> yawRadians);
        short[] signal = BinauralRendererTest.noise(BLOCKS * FRAMES, 100 + channel);
        short[] ears = new short[2 * BLOCKS * FRAMES];
        for (int block = 0; block < BLOCKS; block++) {
            short[][] split = BinauralRendererTest.silence(channels, FRAMES);
            System.arraycopy(signal, block * FRAMES, split[channel], 0, FRAMES);
            short[] stereo = surround.render(BinauralRendererTest.interleave(split));
            System.arraycopy(stereo, 0, ears, block * 2 * FRAMES, 2 * FRAMES);
        }
        return ears;
    }

    // Left over right, in dB
    private static double levelDifference(short[] ears) {
        double left = 0.0;
        double right = 0.0;
        for (int frame = SETTLE_FRAMES; frame < ears.length / 2; frame++) {
            left += (double) ears[2 * frame] * ears[2 * frame];
            right += (double) ears[2 * frame + 1] * ears[2 * frame + 1];
        }
        return 10.0 * Math.log10(left / right);
    }

    // How many frames later the right ear hears it than the left: the lag at
    // which the right ear lines up best with the left
    private static int timeDifference(short[] ears) {
        int frames = ears.length / 2;
        int best = 0;
        double bestSum = -Double.MAX_VALUE;
        for (int lag = -MAX_LAG; lag <= MAX_LAG; lag++) {
            double sum = 0.0;
            for (int frame = SETTLE_FRAMES; frame < frames - MAX_LAG; frame++) {
                sum += (double) ears[2 * frame] * ears[2 * (frame + lag) + 1];
            }
            if (sum > bestSum) {
                bestSum = sum;
                best = lag;
            }
        }
        return best;
    }

    private static void checkSide(String what, short[] ears, int side) {
        double level = levelDifference(ears);
        int lag = timeDifference(ears);
        String cues = what + ": left over right " + String.format("%.2f", level) + " dB, right "
                + lag + " frames later";
        if (side == CENTRE) {
            assertTrue(cues, Math.abs(level) < 0.5);
            assertTrue(cues, Math.abs(lag) <= 1);
        }
        else {
            // The near ear louder by more than a decibel, and first by more
            // than a few samples
            assertTrue(cues, side * level < -1.0);
            assertTrue(cues, side * lag < -3);
        }
    }

    @Test
    public void everySevenOneSpeakerIsHeardFromItsSide() throws IOException {
        String[] names = { "front left", "front right", "centre", "", "back left", "back right",
                "side left", "side right" };
        int[] sides = { LEFT, RIGHT, CENTRE, CENTRE, LEFT, RIGHT, LEFT, RIGHT };
        for (int channel = 0; channel < 8; channel++) {
            if (channel == BinauralRenderer.LFE) {
                continue;
            }
            checkSide("7.1 " + names[channel], earsFor(8, channel, 0.0f), sides[channel]);
        }
    }

    @Test
    public void everyFiveOneSpeakerIsHeardFromItsSide() throws IOException {
        String[] names = { "front left", "front right", "centre", "", "surround left",
                "surround right" };
        int[] sides = { LEFT, RIGHT, CENTRE, CENTRE, LEFT, RIGHT };
        for (int channel = 0; channel < 6; channel++) {
            if (channel == BinauralRenderer.LFE) {
                continue;
            }
            checkSide("5.1 " + names[channel], earsFor(6, channel, 0.0f), sides[channel]);
        }
    }

    @Test
    public void theLfeIsTheSameInBothEars() throws IOException {
        short[] ears = earsFor(8, BinauralRenderer.LFE, 0.0f);
        for (int frame = 0; frame < ears.length / 2; frame++) {
            assertEquals("frame " + frame, ears[2 * frame], ears[2 * frame + 1]);
        }
    }

    @Test
    public void aQuarterTurnMovesTheImageTheOtherWay() throws IOException {
        float quarter = (float) Math.toRadians(90.0);
        // Turned left of the screen, the centre speaker is off to the right
        checkSide("centre, head turned left", earsFor(8, 2, quarter), RIGHT);
        // and turned right, off to the left
        checkSide("centre, head turned right", earsFor(8, 2, -quarter), LEFT);
        // The front left speaker, at 30 degrees left, ends up 60 degrees right
        checkSide("front left, head turned left", earsFor(8, 0, quarter), RIGHT);
        // and a side speaker turned toward ends up in front
        checkSide("side right, head turned right", earsFor(8, 7, -quarter), CENTRE);
    }
}
