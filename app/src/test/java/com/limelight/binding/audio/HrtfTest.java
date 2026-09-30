package com.limelight.binding.audio;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.FloatBuffer;
import java.util.Locale;

/** The KEMAR set as shipped: reading it, mirroring, interpolation, equalisation, resampling. */
public class HrtfTest {

    private static final int MEASURED = 37;
    private static final int RING = 72;
    // 128 measured taps plus the equalisation's 32 in front
    private static final int TAPS = 160;

    // The band the equalisation flattens, and how many points it is read at
    private static final double BAND_LOW_HZ = 300.0;
    private static final double BAND_HIGH_HZ = 15000.0;
    private static final int POINTS = 400;

    /** Gradle runs unit tests from the module directory; a run from the root needs the prefix. */
    static File kemarDir() {
        File local = new File("src/main/assets/hrtf/kemar");
        return local.isDirectory() ? local : new File("app/src/main/assets/hrtf/kemar");
    }

    static Hrtf.Loader fileLoader() {
        final File dir = kemarDir();
        return name -> new FileInputStream(new File(dir, name));
    }

    static Hrtf kemar() throws IOException {
        return kemar(true);
    }

    static Hrtf kemar(boolean equalise) throws IOException {
        return new Hrtf(fileLoader(), equalise);
    }

    private static double hzAt(int point) {
        return BAND_LOW_HZ * Math.pow(BAND_HIGH_HZ / BAND_LOW_HZ, point / (POINTS - 1.0));
    }

    // One filter's power at one frequency, from the definition
    private static double power(float[] filter, double hz) {
        double real = 0.0;
        double imaginary = 0.0;
        for (int i = 0; i < filter.length; i++) {
            double angle = -2.0 * Math.PI * hz * i / Hrtf.SOURCE_RATE;
            real += filter[i] * Math.cos(angle);
            imaginary += filter[i] * Math.sin(angle);
        }
        return real * real + imaginary * imaginary;
    }

    /**
     * The ring's average spectrum in dB over the band, smoothed over the same
     * third of an octave the equalisation uses. Anything narrower is one
     * direction's own notch, which equalisation should leave alone.
     */
    private static double[] meanSpectrumDb(Hrtf hrtf) {
        float[][][] ring = new float[RING][][];
        for (int i = 0; i < RING; i++) {
            ring[i] = hrtf.pairFor(i * 5.0f, Hrtf.SOURCE_RATE);
        }
        double[] power = new double[POINTS];
        for (int point = 0; point < POINTS; point++) {
            double hz = hzAt(point);
            double sum = 0.0;
            for (float[][] pair : ring) {
                sum += power(pair[0], hz) + power(pair[1], hz);
            }
            power[point] = sum / (2 * RING);
        }
        double band = Math.pow(2.0, 1.0 / 6.0);
        double[] db = new double[POINTS];
        for (int point = 0; point < POINTS; point++) {
            double hz = hzAt(point);
            double sum = 0.0;
            int count = 0;
            for (int at = 0; at < POINTS; at++) {
                if (hzAt(at) >= hz / band && hzAt(at) <= hz * band) {
                    sum += power[at];
                    count++;
                }
            }
            db[point] = 10.0 * Math.log10(sum / count);
        }
        return db;
    }

    // The largest departure from the spectrum's own mean, and where it is
    private static double[] worstInBand(double[] db) {
        double mean = 0.0;
        for (double at : db) {
            mean += at / POINTS;
        }
        double worst = 0.0;
        double hz = 0.0;
        for (int point = 0; point < POINTS; point++) {
            if (Math.abs(db[point] - mean) > worst) {
                worst = Math.abs(db[point] - mean);
                hz = hzAt(point);
            }
        }
        return new double[] { worst, hz };
    }

    // The files have plain 44 byte headers, so they can be read the blunt way
    // here to check the parser
    private static float[][] rawPair(String name) throws IOException {
        byte[] bytes = new byte[4096];
        int length = 0;
        InputStream in = new FileInputStream(new File(kemarDir(), name));
        try {
            int read;
            while ((read = in.read(bytes, length, bytes.length - length)) > 0) {
                length += read;
            }
        }
        finally {
            in.close();
        }
        int frames = (length - 44) / 4;
        float[] left = new float[frames];
        float[] right = new float[frames];
        for (int i = 0; i < frames; i++) {
            left[i] = (short) ((bytes[44 + 4 * i] & 0xFF) | (bytes[45 + 4 * i] << 8)) / 32768.0f;
            right[i] = (short) ((bytes[46 + 4 * i] & 0xFF) | (bytes[47 + 4 * i] << 8)) / 32768.0f;
        }
        return new float[][] { left, right };
    }

    private static float peak(float[] filter) {
        float most = 0.0f;
        for (float value : filter) {
            most = Math.max(most, Math.abs(value));
        }
        return most;
    }

    private static double energy(float[] filter) {
        double sum = 0.0;
        for (float value : filter) {
            sum += (double) value * value;
        }
        return sum;
    }

    private static double gain(float[] filter) {
        double sum = 0.0;
        for (float value : filter) {
            sum += value;
        }
        return sum;
    }

    @Test
    public void readsEveryMeasurement() throws IOException {
        Hrtf hrtf = kemar();
        assertEquals(TAPS, hrtf.taps(Hrtf.SOURCE_RATE));
        for (int i = 0; i < MEASURED; i++) {
            float[][] pair = hrtf.pairFor(i * 5.0f, Hrtf.SOURCE_RATE);
            String at = String.format(Locale.US, "azimuth %d", i * 5);
            assertEquals(at, TAPS, pair[0].length);
            assertEquals(at, TAPS, pair[1].length);
            assertTrue(at, peak(pair[0]) > 0.0f);
            assertTrue(at, peak(pair[1]) > 0.0f);
        }
    }

    @Test
    public void theRightEarIsLouderAtNinety() throws IOException {
        float[][] pair = kemar().pairFor(90.0f, Hrtf.SOURCE_RATE);
        assertTrue("left " + peak(pair[0]) + " right " + peak(pair[1]),
                peak(pair[1]) > peak(pair[0]));
    }

    @Test
    public void aMeasuredAzimuthComesBackAsMeasured() throws IOException {
        float[][] measured = rawPair("H0e090a.wav");
        float[][] pair = kemar(false).pairFor(90.0f, Hrtf.SOURCE_RATE);
        for (int i = 0; i < measured[0].length; i++) {
            assertEquals("left " + i, measured[0][i], pair[0][i], 1e-7f);
            assertEquals("right " + i, measured[1][i], pair[1][i], 1e-7f);
        }
    }

    @Test
    public void pastOneEightyIsTheMirror() throws IOException {
        Hrtf hrtf = kemar();
        float[][] right = hrtf.pairFor(90.0f, Hrtf.SOURCE_RATE);
        float[][] left = hrtf.pairFor(270.0f, Hrtf.SOURCE_RATE);
        for (int i = 0; i < right[0].length; i++) {
            assertEquals("left ear " + i, right[1][i], left[0][i], 0.0f);
            assertEquals("right ear " + i, right[0][i], left[1][i], 0.0f);
        }
    }

    @Test
    public void negativeAzimuthsWrapRound() throws IOException {
        Hrtf hrtf = kemar();
        float[][] behind = hrtf.pairFor(330.0f, Hrtf.SOURCE_RATE);
        float[][] front = hrtf.pairFor(-30.0f, Hrtf.SOURCE_RATE);
        for (int i = 0; i < behind[0].length; i++) {
            assertEquals("left " + i, behind[0][i], front[0][i], 0.0f);
            assertEquals("right " + i, behind[1][i], front[1][i], 0.0f);
        }
    }

    @Test
    public void interpolatesBetweenMeasurements() throws IOException {
        Hrtf hrtf = kemar();
        float[][] low = hrtf.pairFor(30.0f, Hrtf.SOURCE_RATE);
        float[][] high = hrtf.pairFor(35.0f, Hrtf.SOURCE_RATE);
        float[][] middle = hrtf.pairFor(32.5f, Hrtf.SOURCE_RATE);
        for (int i = 0; i < middle[0].length; i++) {
            assertEquals("left " + i, 0.5f * (low[0][i] + high[0][i]), middle[0][i], 1e-6f);
            assertEquals("right " + i, 0.5f * (low[1][i] + high[1][i]), middle[1][i], 1e-6f);
        }
    }

    @Test
    public void theCorrectionFlattensWhatTheResponsesShare() throws IOException {
        double[] raw = worstInBand(meanSpectrumDb(kemar(false)));
        double[] equalised = worstInBand(meanSpectrumDb(kemar(true)));
        // The ear canal resonance stands out in the raw set
        assertTrue("raw is flat to " + raw[0] + " dB", raw[0] > 3.0);
        assertTrue("raw peaks at " + raw[1] + " Hz", raw[1] >= 2000.0 && raw[1] <= 5000.0);
        assertTrue("equalised wanders " + equalised[0] + " dB at " + equalised[1] + " Hz",
                equalised[0] <= 1.5);
    }

    @Test
    public void theCorrectionHoldsOutsideTheBand() {
        double[] power = new double[Hrtf.FFT_SIZE / 2 + 1];
        for (int bin = 0; bin < power.length; bin++) {
            // Sloped all the way, so a correction that kept following it
            // could not be flat anywhere
            power[bin] = 1.0 + bin;
        }
        double[] gain = Hrtf.correction(power, 1);
        int low = Hrtf.bin(BAND_LOW_HZ);
        int high = Hrtf.bin(BAND_HIGH_HZ);
        assertEquals("DC", gain[low], gain[0], 0.0);
        assertEquals("under the band", gain[low], gain[low - 1], 0.0);
        assertEquals("over the band", gain[high], gain[gain.length - 1], 0.0);
        assertTrue("inside the band, " + gain[low] + " down to " + gain[high],
                gain[low] - gain[high] > 0.01);
    }

    @Test
    public void theRingAndTheInterpolationBuildTheFilterTheKernelWould() throws IOException {
        Hrtf hrtf = kemar();
        int rate = 48000;
        int taps = hrtf.taps(rate);
        assertEquals(RING, hrtf.ringSize());
        FloatBuffer left = FloatBuffer.allocate(RING * taps);
        FloatBuffer right = FloatBuffer.allocate(RING * taps);
        hrtf.writeRing(rate, left, right);
        float[] wantLeft = new float[taps];
        float[] wantRight = new float[taps];
        int[] entries = new int[2];
        // The FFT kernel mixes two ring entries where filterFor mixes two
        // filters; the results must match
        for (float azimuth : new float[] { -30.0f, 0.0f, 32.5f, 123.4f, 359.9f }) {
            hrtf.filterFor(azimuth, rate, wantLeft, wantRight);
            float far = hrtf.interpolationFor(azimuth, rate, entries);
            for (int tap = 0; tap < taps; tap++) {
                String at = "azimuth " + azimuth + " tap " + tap;
                assertEquals(at, wantLeft[tap], (1.0f - far) * left.get(entries[0] * taps + tap)
                        + far * left.get(entries[1] * taps + tap), 1e-7f);
                assertEquals(at, wantRight[tap], (1.0f - far) * right.get(entries[0] * taps + tap)
                        + far * right.get(entries[1] * taps + tap), 1e-7f);
            }
        }
    }

    @Test
    public void resamplesToFortyEightThousand() throws IOException {
        Hrtf hrtf = kemar();
        // Under half the 512 point transform the C kernel works in
        assertEquals(175, hrtf.taps(48000));
        float[][] measured = hrtf.pairFor(30.0f, Hrtf.SOURCE_RATE);
        float[][] resampled = hrtf.pairFor(30.0f, 48000);
        assertEquals(175, resampled[0].length);
        assertEquals(175, resampled[1].length);
        for (int ear = 0; ear < 2; ear++) {
            // The gain is kept, so audio comes out at the same level at
            // either rate
            assertEquals("ear " + ear + " gain", gain(measured[ear]), gain(resampled[ear]),
                    0.02 * Math.abs(gain(measured[ear])));
            // Energy is per sample, so it scales with the rate
            double was = energy(measured[ear]) * Hrtf.SOURCE_RATE;
            double now = energy(resampled[ear]) * 48000;
            assertEquals("ear " + ear + " energy", was, now, 0.02 * was);
        }
    }
}
