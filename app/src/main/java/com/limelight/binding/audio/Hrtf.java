package com.limelight.binding.audio;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.FloatBuffer;
import java.util.Locale;

/**
 * The MIT KEMAR head related impulse responses, prepared for the virtual
 * surround to convolve with.
 *
 * <p>The compact set has 37 measurements on the horizontal plane, azimuth 0
 * to 180 in 5 degree steps, 128 taps at 44.1 kHz each, channel 0 the left ear
 * and channel 1 the right. Azimuth runs clockwise, so 90 is to the listener's
 * right. The head is taken as symmetrical, so an azimuth past 180 uses the
 * measurement at 360 minus it with the ears swapped.
 *
 * <p>Those responses are only corrected for the measuring speaker, so they
 * still carry KEMAR's ear canal resonance, and a listener's own ears add
 * theirs again through headset speakers or headphones, which sounds harsh.
 * Loading applies the usual diffuse field equalisation: the average power
 * spectrum of all 74 responses, smoothed to a third of an octave, is inverted
 * within 12 dB either way and applied to each response as a zero phase
 * filter. That filter rings before the onset too, so every response is moved
 * 32 samples later and grows by 32 taps.
 *
 * <p>{@link #prepare} resamples to the output rate and lays out the full ring
 * of 72 azimuths, each filter reversed for the convolution's inner loop.
 * After that {@link #filterFor} blends the two nearest azimuths into arrays
 * the caller owns without allocating, so it is safe per block on the audio
 * thread.
 *
 * <p>Not thread safe: one instance per audio stream.
 */
public final class Hrtf {

    /** Supplies the WAV files: assets in the app, plain files in tests. */
    public interface Loader {
        InputStream open(String name) throws IOException;
    }

    /** The rate of the measurements. */
    public static final int SOURCE_RATE = 44100;

    /** Measured azimuths, 0 to 180 in 5 degree steps. */
    private static final int MEASURED = 37;
    private static final float STEP_DEGREES = 5.0f;
    /** The whole circle, once the mirrored side is filled in. */
    private static final int RING = 72;

    // Zero crossings of the resampling sinc on each side of an output sample.
    // Generous, but it only runs once per stream.
    private static final int ZERO_CROSSINGS = 32;

    // The equalisation's transform, four times the data so the correction's
    // own ringing has room
    static final int FFT_SIZE = 512;
    // Samples of that ringing kept ahead of the response
    private static final int PRE_RING = 32;
    // Smoothing band, a sixth of an octave either side
    private static final double SMOOTH_OCTAVE = 1.0 / 6.0;
    // The furthest the correction may go either way
    private static final double LIMIT_DB = 12.0;
    // The band the correction flattens. Below it the responses carry little
    // energy, and above it nothing much is localised.
    private static final double BAND_LOW_HZ = 300.0;
    private static final double BAND_HIGH_HZ = 15000.0;

    private final float[][] measuredLeft = new float[MEASURED][];
    private final float[][] measuredRight = new float[MEASURED][];
    private final int sourceTaps;

    // Where the most recent azimuth landed in the ring
    private final int[] entries = new int[2];

    // The ring at the rate last prepared, filters reversed
    private float[][] ringLeft;
    private float[][] ringRight;
    private int ringRate;
    private int ringTaps;

    /** Loads the 37 files and equalises them. */
    public Hrtf(Loader loader) throws IOException {
        this(loader, true);
    }

    /**
     * Loads the 37 files.
     *
     * @param equalise false to keep the measurements untouched, which the tests
     *                 compare the equalised set against.
     * @throws IOException if a file is missing or not the expected WAV.
     */
    public Hrtf(Loader loader, boolean equalise) throws IOException {
        int taps = 0;
        for (int i = 0; i < MEASURED; i++) {
            String name = String.format(Locale.US, "H0e%03da.wav", i * 5);
            float[][] pair = readWav(loader, name);
            if (i == 0) {
                taps = pair[0].length;
            }
            else if (pair[0].length != taps) {
                throw new IOException(name + " is " + pair[0].length + " frames, not " + taps);
            }
            measuredLeft[i] = pair[0];
            measuredRight[i] = pair[1];
        }
        sourceTaps = equalise ? diffuseField(taps) : taps;
    }

    /** Filter length at a rate. */
    public int taps(int sampleRate) {
        if (sampleRate == SOURCE_RATE) {
            return sourceTaps;
        }
        return (int) Math.ceil((double) sourceTaps * sampleRate / SOURCE_RATE);
    }

    /**
     * Builds the ring at a rate. Done once when the stream is set up, so the
     * audio thread never resamples.
     */
    public void prepare(int sampleRate) {
        if (sampleRate == ringRate) {
            return;
        }
        int taps = taps(sampleRate);
        float[][] left = new float[MEASURED][];
        float[][] right = new float[MEASURED][];
        for (int i = 0; i < MEASURED; i++) {
            left[i] = resample(measuredLeft[i], sampleRate, taps);
            right[i] = resample(measuredRight[i], sampleRate, taps);
        }
        float[][] ringL = new float[RING][];
        float[][] ringR = new float[RING][];
        for (int i = 0; i < RING; i++) {
            // Beyond 180 the source is on the other side, which is the
            // measurement at 360 minus the azimuth heard with the ears swapped
            boolean mirrored = i >= MEASURED;
            int m = mirrored ? RING - i : i;
            ringL[i] = reversed(mirrored ? right[m] : left[m]);
            ringR[i] = reversed(mirrored ? left[m] : right[m]);
        }
        ringLeft = ringL;
        ringRight = ringR;
        ringTaps = taps;
        ringRate = sampleRate;
    }

    /**
     * The filter pair for an azimuth, reversed, into arrays of at least
     * {@link #taps} floats. No allocation once the rate is prepared.
     */
    public void filterFor(float azimuthDegrees, int sampleRate, float[] left, float[] right) {
        prepare(sampleRate);
        float far = between(azimuthDegrees, entries);
        float near = 1.0f - far;
        float[] lowLeft = ringLeft[entries[0]];
        float[] highLeft = ringLeft[entries[1]];
        float[] lowRight = ringRight[entries[0]];
        float[] highRight = ringRight[entries[1]];
        for (int i = 0; i < ringTaps; i++) {
            left[i] = near * lowLeft[i] + far * highLeft[i];
            right[i] = near * lowRight[i] + far * highRight[i];
        }
    }

    /** Azimuths in the ring. */
    public int ringSize() {
        return RING;
    }

    /**
     * The two ring entries an azimuth lies between, into {@code into}, and
     * the fraction of the way from the first to the second. The FFT kernel
     * mixes those two entries by that fraction, which is the filter
     * {@link #filterFor} would build.
     */
    public float interpolationFor(float azimuthDegrees, int sampleRate, int[] into) {
        prepare(sampleRate);
        return between(azimuthDegrees, into);
    }

    /**
     * The whole ring at a rate, reversed, entry i starting at i times taps in
     * each buffer. The FFT kernel reads it once to take its spectra.
     */
    public void writeRing(int sampleRate, FloatBuffer left, FloatBuffer right) {
        prepare(sampleRate);
        for (int i = 0; i < RING; i++) {
            left.position(i * ringTaps);
            left.put(ringLeft[i], 0, ringTaps);
            right.position(i * ringTaps);
            right.put(ringRight[i], 0, ringTaps);
        }
        left.rewind();
        right.rewind();
    }

    private static float between(float azimuthDegrees, int[] into) {
        float azimuth = azimuthDegrees % 360.0f;
        if (azimuth < 0.0f) {
            azimuth += 360.0f;
        }
        float step = azimuth / STEP_DEGREES;
        int low = (int) step;
        if (low >= RING) {
            low = RING - 1;
        }
        into[0] = low;
        into[1] = low + 1 == RING ? 0 : low + 1;
        return step - low;
    }

    /**
     * The pair in time order in new arrays, {@code [0]} the left ear. For the
     * tests, and anything else off the audio thread.
     */
    public float[][] pairFor(float azimuthDegrees, int sampleRate) {
        int taps = taps(sampleRate);
        float[] left = new float[taps];
        float[] right = new float[taps];
        filterFor(azimuthDegrees, sampleRate, left, right);
        return new float[][] { reversed(left), reversed(right) };
    }

    /**
     * Flattens what every response has in common, which is the ear canal,
     * and returns their new length.
     */
    private int diffuseField(int taps) {
        double[] real = new double[FFT_SIZE];
        double[] imaginary = new double[FFT_SIZE];
        double[] power = new double[FFT_SIZE / 2 + 1];
        for (int i = 0; i < MEASURED; i++) {
            addPower(measuredLeft[i], real, imaginary, power);
            addPower(measuredRight[i], real, imaginary, power);
        }
        double[] gain = correction(power, 2 * MEASURED);
        for (int i = 0; i < MEASURED; i++) {
            measuredLeft[i] = corrected(measuredLeft[i], real, imaginary, gain, taps);
            measuredRight[i] = corrected(measuredRight[i], real, imaginary, gain, taps);
        }
        return PRE_RING + taps;
    }

    private static void addPower(float[] response, double[] real, double[] imaginary,
                                 double[] power) {
        load(response, real, imaginary);
        fft(real, imaginary, false);
        for (int bin = 0; bin < power.length; bin++) {
            power[bin] += real[bin] * real[bin] + imaginary[bin] * imaginary[bin];
        }
    }

    /**
     * The inverse of the smoothed average spectrum, levelled over the band and
     * kept within the limit. Real and symmetric, so it adds no phase.
     */
    static double[] correction(double[] power, int responses) {
        int bins = power.length - 1;
        double[] smooth = new double[bins + 1];
        double spread = Math.pow(2.0, SMOOTH_OCTAVE);
        for (int bin = 1; bin <= bins; bin++) {
            int from = Math.max(1, (int) Math.floor(bin / spread));
            int to = Math.min(bins, (int) Math.ceil(bin * spread));
            double sum = 0.0;
            for (int at = from; at <= to; at++) {
                sum += power[at];
            }
            smooth[bin] = sum / (to - from + 1) / responses;
        }
        smooth[0] = smooth[1];
        int low = bin(BAND_LOW_HZ);
        int high = Math.min(bins, bin(BAND_HIGH_HZ));
        double logSum = 0.0;
        for (int bin = low; bin <= high; bin++) {
            logSum += 0.5 * Math.log(smooth[bin]);
        }
        double reference = Math.exp(logSum / (high - low + 1));
        double most = Math.pow(10.0, LIMIT_DB / 20.0);
        double[] gain = new double[bins + 1];
        for (int bin = 0; bin <= bins; bin++) {
            double at = reference / Math.sqrt(smooth[bin]);
            gain[bin] = Math.max(1.0 / most, Math.min(most, at));
        }
        // Outside the band the measurements are mostly the speaker's noise
        // floor, so the correction stays at its band edge value instead of
        // running up to the limit after it
        for (int bin = 0; bin < low; bin++) {
            gain[bin] = gain[low];
        }
        for (int bin = high + 1; bin <= bins; bin++) {
            gain[bin] = gain[high];
        }
        return gain;
    }

    // The correction rings on both sides, so the result is rotated to bring
    // what lands before the onset along with it
    private static float[] corrected(float[] response, double[] real, double[] imaginary,
                                     double[] gain, int taps) {
        load(response, real, imaginary);
        fft(real, imaginary, false);
        int bins = gain.length - 1;
        for (int bin = 0; bin < FFT_SIZE; bin++) {
            double at = gain[bin <= bins ? bin : FFT_SIZE - bin];
            real[bin] *= at;
            imaginary[bin] *= at;
        }
        fft(real, imaginary, true);
        float[] out = new float[PRE_RING + taps];
        for (int i = 0; i < out.length; i++) {
            out[i] = (float) real[(i + FFT_SIZE - PRE_RING) % FFT_SIZE];
        }
        return out;
    }

    private static void load(float[] response, double[] real, double[] imaginary) {
        for (int i = 0; i < FFT_SIZE; i++) {
            real[i] = i < response.length ? response[i] : 0.0;
            imaginary[i] = 0.0;
        }
    }

    static int bin(double hz) {
        return (int) Math.round(hz * FFT_SIZE / SOURCE_RATE);
    }

    // In place radix 2, only used on 512 points while loading
    private static void fft(double[] real, double[] imaginary, boolean inverse) {
        int n = real.length;
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                double swap = real[i];
                real[i] = real[j];
                real[j] = swap;
                swap = imaginary[i];
                imaginary[i] = imaginary[j];
                imaginary[j] = swap;
            }
        }
        for (int length = 2; length <= n; length <<= 1) {
            double step = (inverse ? 2.0 : -2.0) * Math.PI / length;
            int half = length / 2;
            for (int start = 0; start < n; start += length) {
                for (int k = 0; k < half; k++) {
                    double angle = step * k;
                    double cos = Math.cos(angle);
                    double sin = Math.sin(angle);
                    int a = start + k;
                    int b = a + half;
                    double oddReal = real[b] * cos - imaginary[b] * sin;
                    double oddImaginary = real[b] * sin + imaginary[b] * cos;
                    real[b] = real[a] - oddReal;
                    imaginary[b] = imaginary[a] - oddImaginary;
                    real[a] += oddReal;
                    imaginary[a] += oddImaginary;
                }
            }
        }
        if (inverse) {
            for (int i = 0; i < n; i++) {
                real[i] /= n;
                imaginary[i] /= n;
            }
        }
    }

    private static float[] reversed(float[] filter) {
        float[] out = new float[filter.length];
        for (int i = 0; i < filter.length; i++) {
            out[i] = filter[filter.length - 1 - i];
        }
        return out;
    }

    /**
     * Windowed sinc resampling of one filter, scaled by the rate ratio so its
     * gain holds: the same response sampled more densely would otherwise sum
     * to more.
     */
    private float[] resample(float[] source, int rate, int taps) {
        if (rate == SOURCE_RATE) {
            return source.clone();
        }
        double ratio = (double) rate / SOURCE_RATE;
        // Cut off at the lower of the two Nyquists, in units of the source's
        double cutoff = Math.min(1.0, ratio);
        double half = ZERO_CROSSINGS / cutoff;
        float[] out = new float[taps];
        for (int n = 0; n < taps; n++) {
            double at = n / ratio;
            int from = (int) Math.ceil(at - half);
            int to = (int) Math.floor(at + half);
            if (from < 0) {
                from = 0;
            }
            if (to > source.length - 1) {
                to = source.length - 1;
            }
            double sum = 0.0;
            for (int m = from; m <= to; m++) {
                sum += source[m] * kernel(at - m, cutoff, half);
            }
            out[n] = (float) (sum / ratio);
        }
        return out;
    }

    private static double kernel(double at, double cutoff, double half) {
        double x = cutoff * at;
        double sinc = Math.abs(x) < 1e-9 ? 1.0 : Math.sin(Math.PI * x) / (Math.PI * x);
        double window = 0.42 + 0.5 * Math.cos(Math.PI * at / half)
                + 0.08 * Math.cos(2.0 * Math.PI * at / half);
        return cutoff * sinc * window;
    }

    // The shipped files have plain 44 byte headers, but the chunks are found
    // rather than assumed, so a re-exported set still reads
    private static float[][] readWav(Loader loader, String name) throws IOException {
        byte[] bytes = readAll(loader, name);
        if (bytes.length < 12 || !tagAt(bytes, 0, "RIFF") || !tagAt(bytes, 8, "WAVE")) {
            throw new IOException(name + " is not a RIFF WAVE file");
        }
        int fmt = -1;
        int data = -1;
        int dataLength = 0;
        int at = 12;
        while (at + 8 <= bytes.length) {
            int length = intAt(bytes, at + 4);
            if (length < 0 || at + 8 + length > bytes.length) {
                length = bytes.length - at - 8;
            }
            if (tagAt(bytes, at, "fmt ")) {
                fmt = at + 8;
            }
            else if (tagAt(bytes, at, "data")) {
                data = at + 8;
                dataLength = length;
            }
            at += 8 + length + (length & 1);
        }
        if (fmt < 0 || data < 0) {
            throw new IOException(name + " has no fmt or data chunk");
        }
        int format = shortAt(bytes, fmt);
        int channels = shortAt(bytes, fmt + 2);
        int rate = intAt(bytes, fmt + 4);
        int bits = shortAt(bytes, fmt + 14);
        if (format != 1 || channels != 2 || rate != SOURCE_RATE || bits != 16) {
            throw new IOException(name + " is format " + format + ", " + channels + " channels, "
                    + rate + " Hz, " + bits + " bit, not 44.1 kHz stereo 16 bit PCM");
        }
        int frames = dataLength / 4;
        if (frames <= 0) {
            throw new IOException(name + " has no samples");
        }
        float[] left = new float[frames];
        float[] right = new float[frames];
        for (int i = 0; i < frames; i++) {
            left[i] = shortAt(bytes, data + 4 * i) / 32768.0f;
            right[i] = shortAt(bytes, data + 4 * i + 2) / 32768.0f;
        }
        return new float[][] { left, right };
    }

    private static byte[] readAll(Loader loader, String name) throws IOException {
        InputStream in = loader.open(name);
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream(1024);
            byte[] chunk = new byte[1024];
            int read;
            while ((read = in.read(chunk)) > 0) {
                out.write(chunk, 0, read);
            }
            return out.toByteArray();
        }
        finally {
            in.close();
        }
    }

    private static boolean tagAt(byte[] bytes, int at, String tag) {
        for (int i = 0; i < 4; i++) {
            if (bytes[at + i] != (byte) tag.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    private static short shortAt(byte[] bytes, int at) {
        return (short) ((bytes[at] & 0xFF) | (bytes[at + 1] << 8));
    }

    private static int intAt(byte[] bytes, int at) {
        return (bytes[at] & 0xFF) | ((bytes[at + 1] & 0xFF) << 8) | ((bytes[at + 2] & 0xFF) << 16)
                | (bytes[at + 3] << 24);
    }
}
