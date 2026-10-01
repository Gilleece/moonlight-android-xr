package com.limelight.binding.input;

import android.content.Context;

import com.limelight.FileLog;

import java.util.ArrayList;
import java.util.List;

/**
 * The hand tracking permission, which Android XR makes a runtime one for the
 * hand joints. Without it the runtime's own pinch still points and clicks,
 * but the deliberate pinch and the ring finger lock, which read the joints,
 * have nothing to read. Meta's hand permission is granted at install and
 * Pico has none, so only Android XR's name is ever asked for. The request
 * goes up with the eye tracking one. The joints are only looked for as a
 * session starts, so a grant that lands after that counts from the next one.
 */
final class HandTrackingPermission {

    static final String ANDROID_XR = "android.permission.HAND_TRACKING";
    static final String[] NAMES = { ANDROID_XR };

    // Once per run, the same as the eyes
    private static boolean askedThisRun;

    private HandTrackingPermission() {
    }

    /** What to add to this session's request, if anything, saying why not where not. */
    static List<String> toAsk(Context context, boolean handsOn) {
        List<String> out = new ArrayList<>();
        if (!handsOn) {
            return out;
        }
        List<String> names = EyeTrackingPermission.runtimeNames(context, NAMES);
        boolean granted = EyeTrackingPermission.anyGranted(context, names);
        if (EyeTrackingPermission.shouldAsk(true, true, !names.isEmpty(), granted, askedThisRun)) {
            askedThisRun = true;
            out.addAll(names);
        }
        else if (granted) {
            FileLog.event("hand tracking permission already granted");
        }
        // Not a runtime permission is every headset but Android XR, and not
        // worth a line on each of them
        return out;
    }

    /** Logs the hands' half of an answer, where the request carried them. */
    static void onResult(Context context, String[] permissions, int[] results) {
        List<String> names = EyeTrackingPermission.runtimeNames(context, NAMES);
        if (!EyeTrackingPermission.asked(permissions, names)) {
            return;
        }
        int answer = EyeTrackingPermission.answer(permissions, results, names);
        if (answer == EyeTrackingPermission.ANSWER_GRANTED) {
            FileLog.event("hand tracking permission granted");
        }
        else if (answer == EyeTrackingPermission.ANSWER_DENIED) {
            FileLog.event("hand tracking permission denied, the runtime's pinch only");
        }
    }
}
