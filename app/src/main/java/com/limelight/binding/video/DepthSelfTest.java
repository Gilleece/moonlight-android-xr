package com.limelight.binding.video;

import android.content.Context;

import com.limelight.BuildConfig;
import com.limelight.FileLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.Locale;

/**
 * Times the depth models on a headset without a stream. Debug builds only,
 * and only when debug.moonlight.depth_selftest is 1 as the app starts: each
 * model this headset offers is loaded on its own route and run on a fixed
 * synthetic image, and one line per model goes to logcat and the file log.
 */
public final class DepthSelfTest {
    private static final String PROP = "debug.moonlight.depth_selftest";
    private static final int WARMUP_RUNS = 5;
    private static final int TIMED_RUNS = 30;

    private DepthSelfTest() {
    }

    public static void startIfAsked(Context context) {
        if (!BuildConfig.DEBUG) {
            return;
        }
        final Context app = context.getApplicationContext();
        // Off the main thread from the start, since even reading the property
        // means running getprop
        Thread thread = new Thread() {
            @Override
            public void run() {
                if (!"1".equals(readProp(PROP))) {
                    return;
                }
                boolean gen1 = PreferenceConfiguration.isXr2Gen1Headset();
                for (MidasDepthSource.Spec spec : MidasDepthSource.offeredSpecs(gen1)) {
                    runOne(app, spec, spec.route(gen1));
                }
            }
        };
        thread.setName("Depth self test");
        thread.start();
    }

    private static void runOne(Context context, MidasDepthSource.Spec spec,
                               MidasDepthSource.Route route) {
        DepthSize size = route.size;
        ByteBuffer input = ByteBuffer.allocateDirect(size.pixels() * 3 * 4)
                .order(ByteOrder.nativeOrder());
        ByteBuffer output = ByteBuffer.allocateDirect(size.pixels() * 4)
                .order(ByteOrder.nativeOrder());
        fillScene(input.asFloatBuffer(), size);

        MidasDepthSource source = new MidasDepthSource(route);
        try {
            if (!source.initialize(context, input, output)) {
                FileLog.event("depth selftest: "+spec.key+" "+size+" failed to load");
                return;
            }
            for (int i = 0; i < WARMUP_RUNS; i++) {
                source.estimate();
            }
            float min = Float.MAX_VALUE, max = 0.0f, total = 0.0f;
            for (int i = 0; i < TIMED_RUNS; i++) {
                if (!source.estimate()) {
                    FileLog.event("depth selftest: "+spec.key+" "+size+" inference failed");
                    return;
                }
                float ms = source.getLastInferenceMs();
                min = Math.min(min, ms);
                max = Math.max(max, ms);
                total += ms;
            }
            // The map's spread, so a model that loads but puts out nothing
            // useful shows up here too. A run leaves the buffer's position
            // at its end, and a duplicate does not keep the byte order.
            ByteBuffer map = output.duplicate().order(ByteOrder.nativeOrder());
            map.rewind();
            FloatBuffer depth = map.asFloatBuffer();
            float lo = Float.MAX_VALUE, hi = -Float.MAX_VALUE;
            for (int i = 0; i < depth.capacity(); i++) {
                lo = Math.min(lo, depth.get(i));
                hi = Math.max(hi, depth.get(i));
            }
            FileLog.event(String.format(Locale.US, "depth selftest: %s %s %s, load %d ms, "
                            +"inference avg %.1f ms min %.1f max %.1f, output %.3g to %.3g",
                    spec.key, size, source.runtimeLabel(), source.getLoadMs(),
                    total / TIMED_RUNS, min, max, lo, hi));
        } finally {
            source.release();
        }
    }

    // Sky over ground with a near object in the middle, the same every run
    private static void fillScene(FloatBuffer rgb, DepthSize size) {
        for (int y = 0; y < size.height; y++) {
            float fy = y / (float)(size.height - 1);
            for (int x = 0; x < size.width; x++) {
                float fx = x / (float)(size.width - 1);
                float dx = (fx - 0.5f) * size.width / size.height;
                float dy = fy - 0.6f;
                boolean object = dx * dx + dy * dy < 0.04f;
                rgb.put(object ? 0.8f : 0.3f + 0.4f * fy);
                rgb.put(object ? 0.2f : 0.5f + 0.2f * fx);
                rgb.put(object ? 0.1f : 0.9f - 0.6f * fy);
            }
        }
    }

    private static String readProp(String name) {
        Process process = null;
        try {
            process = new ProcessBuilder("/system/bin/getprop", name).start();
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()));
            String line = reader.readLine();
            return line != null ? line.trim() : "";
        } catch (Exception e) {
            return "";
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }
}
