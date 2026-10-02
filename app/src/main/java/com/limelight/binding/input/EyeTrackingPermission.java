package com.limelight.binding.input;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.os.Build;
import android.text.TextUtils;

import com.limelight.FileLog;

import java.util.ArrayList;
import java.util.List;

/**
 * The eye tracking permission. The manifest declares Meta's, Pico's and
 * Android XR's, but declaring is not enough where the platform makes it a
 * runtime permission: without the grant the gaze pose never arrives, and Look
 * to point is dead. So a VR session with the setting on asks for it, once per
 * run of the app, and never waits for the answer, but only on a headset that
 * declares eye tracking: Look to point is on by default, and a headset
 * without it should never prompt for something it cannot do. Only the names
 * this platform knows as runtime permissions are asked for or counted, so a
 * headset that has none has nothing to ask and another store's name is never
 * taken for a refusal. An activity can only have one request up at a time, so
 * the hand tracking permission goes up in the same one.
 */
public final class EyeTrackingPermission {

    public static final String META = "com.oculus.permission.EYE_TRACKING";
    public static final String PICO = "com.picovr.permission.EYE_TRACKING";
    // Android XR's name for gaze as an input, the one XR_EXT_eye_gaze_interaction wants
    public static final String ANDROID_XR = "android.permission.EYE_TRACKING_FINE";
    static final String[] NAMES = { META, PICO, ANDROID_XR };

    // The system feature each platform declares for eye tracking, the same
    // three the manifest lists
    static final String[] FEATURES = {
            "oculus.software.eye_tracking", "pvr.software.eyetracking",
            "android.hardware.xr.input.eye_tracking"
    };

    /** Whether the system declares a feature: the PackageManager, or a test's own set. */
    interface Features {
        boolean has(String name);
    }

    /** The request code the answer comes back under. */
    public static final int REQUEST_CODE = 0x4559;

    public static final int ANSWER_NONE = 0;
    public static final int ANSWER_GRANTED = 1;
    public static final int ANSWER_DENIED = 2;

    // Once per run, however many streams are started in it. The platform keeps
    // its own count of refusals and stops showing the dialog after enough.
    private static boolean askedThisRun;

    private EyeTrackingPermission() {
    }

    /**
     * Whether to put the request up: a VR session with Look to point on, on a
     * platform where the permission is a runtime one, not granted yet and not
     * asked for already this run.
     */
    public static boolean shouldAsk(boolean vr, boolean gazeOn, boolean runtime, boolean granted,
                                    boolean asked) {
        return vr && gazeOn && runtime && !granted && !asked;
    }

    /** The same for the eyes, which only a headset that declares eye tracking is asked for. */
    static boolean shouldAskForEyes(Features features, boolean vr, boolean gazeOn,
                                    boolean runtime, boolean granted, boolean asked) {
        return tracksEyes(features) && shouldAsk(vr, gazeOn, runtime, granted, asked);
    }

    /** Whether the headset declares eye tracking under any platform's name for it. */
    static boolean tracksEyes(Features features) {
        for (String name : FEATURES) {
            if (features.has(name)) {
                return true;
            }
        }
        return false;
    }

    private static Features systemFeatures(Context context) {
        PackageManager pm = context.getPackageManager();
        return pm::hasSystemFeature;
    }

    /** Whether the eyes may point: nothing to grant here, or it is granted. */
    public static boolean gazeAllowed(boolean runtime, boolean granted) {
        return !runtime || granted;
    }

    /**
     * What a request came back with, counting only the names in runtime. An
     * empty result is a request that was cut short, which is no answer.
     */
    public static int answer(String[] permissions, int[] results, List<String> runtime) {
        if (permissions == null || results == null) {
            return ANSWER_NONE;
        }
        int answer = ANSWER_NONE;
        int n = Math.min(permissions.length, results.length);
        for (int i = 0; i < n; i++) {
            if (!runtime.contains(permissions[i])) {
                continue;
            }
            if (results[i] == PackageManager.PERMISSION_GRANTED) {
                return ANSWER_GRANTED;
            }
            answer = ANSWER_DENIED;
        }
        return answer;
    }

    /** Whether any of names was in the request an answer is for. */
    static boolean asked(String[] permissions, List<String> names) {
        if (permissions == null) {
            return false;
        }
        for (String p : permissions) {
            if (names.contains(p)) {
                return true;
            }
        }
        return false;
    }

    /** The eye tracking names among the ones the manifest asks for. */
    static List<String> declared(String[] requested) {
        return declared(requested, NAMES);
    }

    /** Those of names the manifest asks for, in the order of names. */
    static List<String> declared(String[] requested, String[] names) {
        List<String> out = new ArrayList<>();
        if (requested == null) {
            return out;
        }
        for (String name : names) {
            for (String r : requested) {
                if (name.equals(r)) {
                    out.add(name);
                    break;
                }
            }
        }
        return out;
    }

    /** The declared names this platform defines as runtime permissions. */
    public static List<String> runtimeNames(Context context) {
        return runtimeNames(context, NAMES);
    }

    /** Those of names the manifest declares and this platform defines as runtime permissions. */
    static List<String> runtimeNames(Context context, String[] names) {
        List<String> out = new ArrayList<>();
        // Nothing is a runtime permission before Android 6
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return out;
        }
        PackageManager pm = context.getPackageManager();
        String[] requested;
        try {
            PackageInfo info = pm.getPackageInfo(context.getPackageName(),
                    PackageManager.GET_PERMISSIONS);
            requested = info.requestedPermissions;
        }
        catch (PackageManager.NameNotFoundException e) {
            return out;
        }
        for (String name : declared(requested, names)) {
            try {
                PermissionInfo p = pm.getPermissionInfo(name, 0);
                int base = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P ? p.getProtection()
                        : p.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE;
                if (base == PermissionInfo.PROTECTION_DANGEROUS) {
                    out.add(name);
                }
            }
            catch (PackageManager.NameNotFoundException e) {
                // Not a permission this platform has
            }
        }
        return out;
    }

    public static boolean anyGranted(Context context, List<String> names) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return false;
        }
        for (String name : names) {
            if (context.checkSelfPermission(name) == PackageManager.PERMISSION_GRANTED) {
                return true;
            }
        }
        return false;
    }

    /** Whether the eyes may point on this headset as things stand. */
    public static boolean gazeAllowed(Context context) {
        List<String> names = runtimeNames(context);
        return gazeAllowed(!names.isEmpty(), anyGranted(context, names));
    }

    /**
     * Asks if it should, for the eyes with Look to point on and the hands
     * with hand tracking on, in one request, and says why not in the log
     * where it does not. The answer arrives in the activity's
     * onRequestPermissionsResult, which hands it to onResult; nothing waits on
     * it. True when the request went up.
     */
    public static boolean askOnce(Activity activity, boolean vr, boolean gazeOn, boolean handsOn) {
        if (!vr) {
            return false;
        }
        List<String> ask = new ArrayList<>();
        if (gazeOn) {
            Features features = systemFeatures(activity);
            List<String> names = runtimeNames(activity);
            boolean granted = anyGranted(activity, names);
            if (shouldAskForEyes(features, true, true, !names.isEmpty(), granted, askedThisRun)) {
                askedThisRun = true;
                ask.addAll(names);
            }
            else if (!tracksEyes(features)) {
                FileLog.event("eye tracking permission not asked: this headset declares no"
                        + " eye tracking");
            }
            else if (names.isEmpty()) {
                FileLog.event("eye tracking permission not asked: not a runtime permission"
                        + " on this headset");
            }
            else if (granted) {
                FileLog.event("eye tracking permission already granted");
            }
        }
        ask.addAll(HandTrackingPermission.toAsk(activity, handsOn));
        if (ask.isEmpty() || Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return false;
        }
        FileLog.event("headset permissions asked (" + TextUtils.join(", ", ask) + ")");
        activity.requestPermissions(ask.toArray(new String[0]), REQUEST_CODE);
        return true;
    }

    /**
     * Logs the answer to a request of ours. Says whether the eyes may point
     * now, or null when the result was for some other request.
     */
    public static Boolean onResult(Context context, int requestCode, String[] permissions,
                                   int[] results) {
        if (requestCode != REQUEST_CODE) {
            return null;
        }
        List<String> names = runtimeNames(context);
        // A request that carried only the hands has nothing to say about the eyes
        if (asked(permissions, names) || permissions == null || permissions.length == 0) {
            int answer = answer(permissions, results, names);
            if (answer == ANSWER_GRANTED) {
                FileLog.event("eye tracking permission granted");
            }
            else if (answer == ANSWER_DENIED) {
                FileLog.event("eye tracking permission denied, Look to point stays off");
            }
            else {
                FileLog.event("headset permission request cut short");
            }
        }
        HandTrackingPermission.onResult(context, permissions, results);
        return gazeAllowed(!names.isEmpty(), anyGranted(context, names));
    }
}
