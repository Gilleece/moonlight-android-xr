package com.limelight.utils;

import org.junit.Test;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** The line at the top of the log saying which headset features the device claims. */
public class HeadsetFeaturesTest {

    @Test
    public void onlyHeadsetFeaturesCount() {
        assertTrue(HeadsetFeatures.isHeadsetFeature("android.hardware.vr.headtracking"));
        assertTrue(HeadsetFeatures.isHeadsetFeature("android.software.xr.api.openxr"));
        assertTrue(HeadsetFeatures.isHeadsetFeature("android.hardware.xr.input.hand_tracking"));
        assertTrue(HeadsetFeatures.isHeadsetFeature("oculus.software.handtracking"));
        assertTrue(HeadsetFeatures.isHeadsetFeature("com.oculus.feature.PASSTHROUGH"));
        assertTrue(HeadsetFeatures.isHeadsetFeature("pvr.software.eyetracking"));
        assertFalse(HeadsetFeatures.isHeadsetFeature("android.hardware.touchscreen"));
        assertFalse(HeadsetFeatures.isHeadsetFeature("android.software.leanback"));
        assertFalse(HeadsetFeatures.isHeadsetFeature(null));
    }

    @Test
    public void saysYesOrNoWithTheVersionWhereThereIsOne() {
        Map<String, Integer> device = new HashMap<>();
        device.put("android.software.xr.api.openxr", 0x00010001);
        device.put("android.hardware.xr.input.controller", 0);
        device.put("android.hardware.vr.headtracking", 1);

        assertEquals("headset features: android.hardware.vr.headtracking yes v1,"
                        + " oculus.software.handtracking no,"
                        + " android.software.xr.api.openxr yes v0x00010001,"
                        + " android.hardware.xr.input.controller yes",
                HeadsetFeatures.describe(Arrays.asList("android.hardware.vr.headtracking",
                        "oculus.software.handtracking", "android.software.xr.api.openxr",
                        "android.hardware.xr.input.controller"), device));
        assertEquals("headset features: none declared",
                HeadsetFeatures.describe(Collections.<String>emptyList(), device));
    }
}
