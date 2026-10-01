package com.limelight.utils;

import android.content.Context;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The headset features the manifest declares, Meta's, Pico's and Android
 * XR's, and whether this device says it has each, for the top of the log. A
 * report from a headset nobody here has then starts with what it claims to
 * be, which says a lot about why a session would or would not start on it.
 */
public final class HeadsetFeatures {

    private static final String[] PREFIXES = {
            "android.hardware.vr.", "android.hardware.xr.", "android.software.xr.",
            "oculus.", "com.oculus.", "pvr."
    };

    private HeadsetFeatures() {
    }

    static boolean isHeadsetFeature(String name) {
        if (name == null) {
            return false;
        }
        for (String prefix : PREFIXES) {
            if (name.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }

    /**
     * One line: each declared headset feature, then yes or no, and the
     * version the device gives where it gives one. device holds the features
     * the device has, each with its version, 0 for none.
     */
    static String describe(List<String> declared, Map<String, Integer> device) {
        StringBuilder line = new StringBuilder("headset features:");
        boolean first = true;
        for (String name : declared) {
            line.append(first ? " " : ", ").append(name);
            first = false;
            Integer version = device.get(name);
            if (version == null) {
                line.append(" no");
                continue;
            }
            line.append(" yes");
            if (version != 0) {
                // OpenXR's packs major and minor into the two halves
                line.append(" v").append(version > 0xFFFF
                        ? String.format(Locale.US, "0x%08x", version) : Integer.toString(version));
            }
        }
        if (first) {
            line.append(" none declared");
        }
        return line.toString();
    }

    /** The line for this device. */
    public static String line(Context context) {
        PackageManager pm = context.getPackageManager();
        List<String> declared = new ArrayList<>();
        try {
            PackageInfo info = pm.getPackageInfo(context.getPackageName(),
                    PackageManager.GET_CONFIGURATIONS);
            if (info.reqFeatures != null) {
                for (FeatureInfo f : info.reqFeatures) {
                    if (isHeadsetFeature(f.name) && !declared.contains(f.name)) {
                        declared.add(f.name);
                    }
                }
            }
        }
        catch (PackageManager.NameNotFoundException e) {
            // Nothing declared to ask about
        }

        // Versions only come with the whole list
        Map<String, Integer> versions = new HashMap<>();
        FeatureInfo[] available = pm.getSystemAvailableFeatures();
        if (available != null) {
            for (FeatureInfo f : available) {
                if (f.name != null) {
                    versions.put(f.name, Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                            ? f.version : 0);
                }
            }
        }
        Map<String, Integer> device = new HashMap<>();
        for (String name : declared) {
            if (pm.hasSystemFeature(name)) {
                Integer version = versions.get(name);
                device.put(name, version != null ? version : 0);
            }
        }
        return describe(declared, device);
    }
}
