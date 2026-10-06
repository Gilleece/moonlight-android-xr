package com.limelight.binding.input;

import org.junit.Test;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** When the eye tracking permission is asked for, and what an answer means. */
public class EyeTrackingPermissionTest {

    private static final int GRANTED = 0;
    private static final int DENIED = -1;

    @Test
    public void asksOnlyInAVrSessionWithGazeOnAndSomethingToGrant() {
        assertTrue(EyeTrackingPermission.shouldAsk(true, true, true, false, false));
        // A flat stream, the setting off, nothing that is a runtime permission
        // here, already granted, already asked this run
        assertFalse(EyeTrackingPermission.shouldAsk(false, true, true, false, false));
        assertFalse(EyeTrackingPermission.shouldAsk(true, false, true, false, false));
        assertFalse(EyeTrackingPermission.shouldAsk(true, true, false, false, false));
        assertFalse(EyeTrackingPermission.shouldAsk(true, true, true, true, false));
        assertFalse(EyeTrackingPermission.shouldAsk(true, true, true, false, true));
    }

    // A headset's system features, as hasSystemFeature would answer for them
    private static EyeTrackingPermission.Features features(String... names) {
        Set<String> declared = new HashSet<>(Arrays.asList(names));
        return declared::contains;
    }

    // Everything else in favour of asking: a VR session, gaze on, a runtime
    // permission, not granted and not asked yet
    private static boolean asks(EyeTrackingPermission.Features features) {
        return EyeTrackingPermission.shouldAskForEyes(features, true, true, true, false, false);
    }

    @Test
    public void aHeadsetWithoutEyeTrackingIsNeverAsked() {
        // Quest 3 and Quest 2: head and hand tracking and passthrough, no eyes
        assertFalse(asks(features("android.hardware.vr.headtracking",
                "oculus.software.handtracking", "com.oculus.feature.PASSTHROUGH")));
        assertFalse(asks(features("pvr.software.handtracking")));
        assertFalse(asks(features("android.software.xr.api.openxr",
                "android.hardware.xr.input.controller",
                "android.hardware.xr.input.hand_tracking")));
        assertFalse(asks(features()));
    }

    @Test
    public void aHeadsetThatTracksEyesIsAskedUnderAnyPlatformsName() {
        assertTrue(asks(features("oculus.software.handtracking", "oculus.software.eye_tracking")));
        assertTrue(asks(features("pvr.software.handtracking", "pvr.software.eyetracking")));
        assertTrue(asks(features("android.software.xr.api.openxr",
                "android.hardware.xr.input.eye_tracking")));
        // Eye tracking does not override the rest: gaze off, or already granted
        EyeTrackingPermission.Features eyes = features("oculus.software.eye_tracking");
        assertFalse(EyeTrackingPermission.shouldAskForEyes(eyes, true, false, true, false, false));
        assertFalse(EyeTrackingPermission.shouldAskForEyes(eyes, true, true, true, true, false));
    }

    @Test
    public void theFeaturesCheckedAreTheOnesTheManifestLists() throws IOException {
        String manifest = new String(Files.readAllBytes(Paths.get("src/main/AndroidManifest.xml")),
                StandardCharsets.UTF_8);
        for (String feature : EyeTrackingPermission.FEATURES) {
            assertTrue(feature, manifest.contains("android:name=\"" + feature + "\""));
        }
    }

    @Test
    public void theEyesPointUnlessARuntimePermissionIsRefused() {
        assertTrue(EyeTrackingPermission.gazeAllowed(false, false));
        assertTrue(EyeTrackingPermission.gazeAllowed(true, true));
        assertFalse(EyeTrackingPermission.gazeAllowed(true, false));
    }

    @Test
    public void onlyTheRuntimeNamesCountInAnAnswer() {
        List<String> meta = Collections.singletonList(EyeTrackingPermission.META);
        String[] both = { EyeTrackingPermission.META, EyeTrackingPermission.PICO };

        // The other store's name refused is not a refusal here
        assertEquals(EyeTrackingPermission.ANSWER_GRANTED,
                EyeTrackingPermission.answer(both, new int[] { GRANTED, DENIED }, meta));
        assertEquals(EyeTrackingPermission.ANSWER_DENIED,
                EyeTrackingPermission.answer(both, new int[] { DENIED, GRANTED }, meta));
        assertEquals(EyeTrackingPermission.ANSWER_NONE,
                EyeTrackingPermission.answer(both, new int[] { DENIED, GRANTED },
                        Collections.<String>emptyList()));
        // A request cut short comes back empty
        assertEquals(EyeTrackingPermission.ANSWER_NONE,
                EyeTrackingPermission.answer(new String[0], new int[0], meta));
        assertEquals(EyeTrackingPermission.ANSWER_NONE,
                EyeTrackingPermission.answer(null, null, meta));
        // Either name granted is a grant
        assertEquals(EyeTrackingPermission.ANSWER_GRANTED,
                EyeTrackingPermission.answer(both, new int[] { DENIED, GRANTED },
                        Arrays.asList(both)));
    }

    @Test
    public void picksTheEyeTrackingNamesOutOfTheManifest() {
        String[] requested = {
                "android.permission.INTERNET", EyeTrackingPermission.PICO,
                "com.oculus.permission.HAND_TRACKING", EyeTrackingPermission.META
        };
        assertEquals(Arrays.asList(EyeTrackingPermission.META, EyeTrackingPermission.PICO),
                EyeTrackingPermission.declared(requested));
        assertTrue(EyeTrackingPermission.declared(null).isEmpty());
        assertTrue(EyeTrackingPermission.declared(new String[] { "x" }).isEmpty());
    }

    @Test
    public void androidXrsEyeAndHandNamesAreKeptApart() {
        String[] requested = {
                HandTrackingPermission.ANDROID_XR, EyeTrackingPermission.ANDROID_XR,
                EyeTrackingPermission.META
        };
        assertEquals(Arrays.asList(EyeTrackingPermission.META, EyeTrackingPermission.ANDROID_XR),
                EyeTrackingPermission.declared(requested));
        assertEquals(Collections.singletonList(HandTrackingPermission.ANDROID_XR),
                EyeTrackingPermission.declared(requested, HandTrackingPermission.NAMES));
    }

    @Test
    public void eachHalfOfAJointRequestIsAnsweredOnItsOwn() {
        List<String> eyes = Collections.singletonList(EyeTrackingPermission.ANDROID_XR);
        List<String> hands = Arrays.asList(HandTrackingPermission.NAMES);
        String[] both = { EyeTrackingPermission.ANDROID_XR, HandTrackingPermission.ANDROID_XR };
        int[] eyesRefused = { DENIED, GRANTED };

        assertEquals(EyeTrackingPermission.ANSWER_DENIED,
                EyeTrackingPermission.answer(both, eyesRefused, eyes));
        assertEquals(EyeTrackingPermission.ANSWER_GRANTED,
                EyeTrackingPermission.answer(both, eyesRefused, hands));

        // A request that carried only the hands was not about the eyes
        String[] handsOnly = { HandTrackingPermission.ANDROID_XR };
        assertFalse(EyeTrackingPermission.asked(handsOnly, eyes));
        assertTrue(EyeTrackingPermission.asked(handsOnly, hands));
        assertTrue(EyeTrackingPermission.asked(both, eyes));
        assertFalse(EyeTrackingPermission.asked(null, eyes));
    }
}
