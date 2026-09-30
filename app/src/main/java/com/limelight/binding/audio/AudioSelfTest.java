package com.limelight.binding.audio;

import android.content.Context;
import android.os.Process;

import com.limelight.BuildConfig;
import com.limelight.FileLog;

import java.io.IOException;
import java.util.Arrays;
import java.util.Locale;

/**
 * Times the virtual surround on a headset without a stream. Debug builds
 * only, and only when debug.moonlight.audio_selftest is 1 as the app starts:
 * ten seconds of noise go through the renderer for 5.1 and 7.1, in 5 ms and
 * 10 ms blocks, with the head still and with it sweeping, on a thread at
 * audio priority. One line per case goes to logcat and the file log.
 *
 * <p>The cases run after a warm up of a few seconds, since the app is still
 * starting and the JIT has not seen this code yet. The warm up gets a line
 * of its own, as a rough idea of the first blocks of a fresh stream.
 */
public final class AudioSelfTest {
    private static final int RATE = 48000;
    private static final int SECONDS = 10;
    // Untimed blocks first, so the JIT has settled before the clock starts
    private static final int WARMUP_BLOCKS = 200;
    // Side to side, 90 degrees each way every 2 s, which moves the head far
    // enough between nearly every pair of blocks to take the crossfade
    private static final double SWEEP_RADIANS = Math.toRadians(90.0);
    private static final double SWEEP_SECONDS = 2.0;
    // Wall clock time for the warm up, and how many of its first blocks are
    // averaged for its line
    private static final long WARMUP_MS = 3000;
    private static final int COLD_BLOCKS = 200;

    private AudioSelfTest() {
    }

    public static void startIfAsked(Context context) {
        if (!BuildConfig.DEBUG) {
            return;
        }
        final Context app = context.getApplicationContext();
        // Off the main thread from the start, since reading the property
        // means running getprop
        Thread thread = new Thread() {
            @Override
            public void run() {
                if (!"1".equals(DebugProps.get(DebugProps.AUDIO_SELFTEST))) {
                    return;
                }
                try {
                    Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
                } catch (RuntimeException e) {
                    FileLog.event("audio selftest: could not raise priority: "+e);
                }
                warmUp(app);
                for (int channels : new int[] { 6, 8 }) {
                    for (int frames : new int[] { 240, 480 }) {
                        runOne(app, channels, frames, false);
                        runOne(app, channels, frames, true);
                    }
                }
            }
        };
        thread.setName("Audio self test");
        thread.start();
    }

    // 7.1 in 5 ms blocks with the head sweeping, for WARMUP_MS of wall clock
    private static void warmUp(Context context) {
        final int channels = 8;
        final int frames = 240;
        final float[] yaw = new float[1];
        long buildStart = System.nanoTime();
        VirtualSurround surround;
        try {
            surround = VirtualSurround.create(channels, RATE, frames,
                    AndroidAudioRenderer.assetLoader(context), () -> yaw[0],
                    BinauralRenderer.REAR_DELAY_MS);
        } catch (IOException | RuntimeException | LinkageError e) {
            FileLog.event("audio selftest: warm up failed to build: "+e);
            return;
        }
        long buildMs = (System.nanoTime() - buildStart) / 1000000;
        try {
            short[] block = new short[channels * frames];
            int seed = 1;
            long firstNs = 0;
            long coldNs = 0;
            int blocks = 0;
            long start = System.nanoTime();
            while (System.nanoTime() - start < WARMUP_MS * 1000000L) {
                seed = fillNoise(block, seed);
                yaw[0] = sweepAt((double) blocks * frames / RATE);
                long from = System.nanoTime();
                surround.render(block);
                long took = System.nanoTime() - from;
                if (blocks == 0) {
                    firstNs = took;
                }
                if (blocks < COLD_BLOCKS) {
                    coldNs += took;
                }
                blocks++;
            }
            FileLog.event(String.format(Locale.US, "audio selftest: warm up, 7.1, 240 frame"
                            +" blocks, head sweeping, %s kernel: first block %d us, first %d"
                            +" blocks avg %d us, %d blocks in %d ms, built in %d ms",
                    surround.kernel(), firstNs / 1000, COLD_BLOCKS,
                    coldNs / Math.min(blocks, COLD_BLOCKS) / 1000, blocks, WARMUP_MS, buildMs));
        } finally {
            surround.release();
        }
    }

    private static float sweepAt(double seconds) {
        return (float) (SWEEP_RADIANS * Math.sin(2.0 * Math.PI * seconds / SWEEP_SECONDS));
    }

    private static void runOne(Context context, int channels, int frames, boolean sweeping) {
        String layout = channels == 6 ? "5.1" : "7.1";
        final float[] yaw = new float[1];
        long buildStart = System.nanoTime();
        VirtualSurround surround;
        try {
            surround = VirtualSurround.create(channels, RATE, frames,
                    AndroidAudioRenderer.assetLoader(context), () -> yaw[0],
                    BinauralRenderer.REAR_DELAY_MS);
        } catch (IOException | RuntimeException | LinkageError e) {
            FileLog.event("audio selftest: "+layout+" "+frames+" frames failed to build: "+e);
            return;
        }
        long buildMs = (System.nanoTime() - buildStart) / 1000000;
        try {
            short[] block = new short[channels * frames];
            int blocks = SECONDS * RATE / frames;
            long[] times = new long[blocks];
            int seed = 1;
            for (int i = -WARMUP_BLOCKS; i < blocks; i++) {
                seed = fillNoise(block, seed);
                double seconds = (double) i * frames / RATE;
                yaw[0] = sweeping ? sweepAt(seconds) : 0.0f;
                long from = System.nanoTime();
                surround.render(block);
                long took = System.nanoTime() - from;
                if (i >= 0) {
                    times[i] = took;
                }
            }
            Arrays.sort(times);
            double total = 0.0;
            for (long t : times) {
                total += t;
            }
            double avgUs = total / blocks / 1000.0;
            double p99Us = times[Math.min(blocks - 1, (int) Math.ceil(blocks * 0.99) - 1)] / 1000.0;
            double worstUs = times[blocks - 1] / 1000.0;
            double blockUs = frames * 1000000.0 / RATE;
            FileLog.event(String.format(Locale.US, "audio selftest: %s, %d frame blocks (%.0f ms),"
                            +" head %s, %s kernel: avg %.0f us (%.2f%%), p99 %.0f us (%.2f%%),"
                            +" worst %.0f us (%.2f%%) over %d blocks, built in %d ms, priority %d",
                    layout, frames, blockUs / 1000.0, sweeping ? "sweeping" : "still",
                    surround.kernel(), avgUs, 100.0 * avgUs / blockUs, p99Us,
                    100.0 * p99Us / blockUs, worstUs, 100.0 * worstUs / blockUs, blocks, buildMs,
                    Process.getThreadPriority(Process.myTid())));
        } finally {
            surround.release();
        }
    }

    // Uniform noise over about a quarter of full scale, the same sequence
    // every run
    private static int fillNoise(short[] block, int seed) {
        for (int i = 0; i < block.length; i++) {
            seed = seed * 1664525 + 1013904223;
            block[i] = (short) ((seed >> 16) % 8000);
        }
        return seed;
    }
}
