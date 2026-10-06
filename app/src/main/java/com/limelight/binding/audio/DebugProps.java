package com.limelight.binding.audio;

import com.limelight.BuildConfig;

import java.io.BufferedReader;
import java.io.InputStreamReader;

/**
 * The debug.moonlight.* properties the audio side reads. Debug builds only: a
 * release build always gets the empty string. Each read runs getprop, so keep
 * it off the audio thread.
 */
final class DebugProps {
    static final String SURROUND_REAR_MS = "debug.moonlight.surround_rear_ms";
    static final String AUDIO_SELFTEST = "debug.moonlight.audio_selftest";

    private DebugProps() {
    }

    static String get(String name) {
        if (!BuildConfig.DEBUG) {
            return "";
        }
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

    /** A number, or the fallback when unset or unreadable. */
    static float getFloat(String name, float fallback) {
        String value = get(name);
        if (value.isEmpty()) {
            return fallback;
        }
        try {
            return Float.parseFloat(value);
        } catch (NumberFormatException e) {
            return fallback;
        }
    }
}
