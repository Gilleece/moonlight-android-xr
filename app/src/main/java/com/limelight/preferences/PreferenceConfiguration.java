package com.limelight.preferences;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.preference.PreferenceManager;
import android.view.Display;

import com.limelight.FileLog;
import com.limelight.nvstream.jni.MoonBridge;

public class PreferenceConfiguration {
    public enum FormatOption {
        AUTO,
        FORCE_AV1,
        FORCE_HEVC,
        FORCE_H264,
    };

    public enum AnalogStickForScrolling {
        NONE,
        RIGHT,
        LEFT
    }

    private static final String LEGACY_RES_FPS_PREF_STRING = "list_resolution_fps";
    private static final String LEGACY_ENABLE_51_SURROUND_PREF_STRING = "checkbox_51_surround";

    static final String RESOLUTION_PREF_STRING = "list_resolution";
    static final String FPS_PREF_STRING = "list_fps";
    static final String BITRATE_PREF_STRING = "seekbar_bitrate_kbps";
    private static final String BITRATE_PREF_OLD_STRING = "seekbar_bitrate";
    private static final String STRETCH_PREF_STRING = "checkbox_stretch_video";
    private static final String SOPS_PREF_STRING = "checkbox_enable_sops";
    private static final String DISABLE_TOASTS_PREF_STRING = "checkbox_disable_warnings";
    private static final String HOST_AUDIO_PREF_STRING = "checkbox_host_audio";
    private static final String DEADZONE_PREF_STRING = "seekbar_deadzone";
    private static final String OSC_OPACITY_PREF_STRING = "seekbar_osc_opacity";
    private static final String LANGUAGE_PREF_STRING = "list_languages";
    private static final String SMALL_ICONS_PREF_STRING = "checkbox_small_icon_mode";
    private static final String MULTI_CONTROLLER_PREF_STRING = "checkbox_multi_controller";
    static final String AUDIO_CONFIG_PREF_STRING = "list_audio_config";
    private static final String USB_DRIVER_PREF_SRING = "checkbox_usb_driver";
    private static final String VIDEO_FORMAT_PREF_STRING = "video_format";
    private static final String ONSCREEN_CONTROLLER_PREF_STRING = "checkbox_show_onscreen_controls";
    private static final String ONLY_L3_R3_PREF_STRING = "checkbox_only_show_L3R3";
    private static final String SHOW_GUIDE_BUTTON_PREF_STRING = "checkbox_show_guide_button";
    private static final String LEGACY_DISABLE_FRAME_DROP_PREF_STRING = "checkbox_disable_frame_drop";
    private static final String ENABLE_HDR_PREF_STRING = "checkbox_enable_hdr";
    private static final String ENABLE_PIP_PREF_STRING = "checkbox_enable_pip";
    public static final String ENABLE_PERF_OVERLAY_STRING = "checkbox_enable_perf_overlay";
    private static final String ENABLE_GL_RENDER_PATH_PREF_STRING = "checkbox_enable_gl_render_path";
    private static final String ENABLE_VR_MODE_PREF_STRING = "checkbox_enable_vr_mode";
    private static final String VR_HEAD_LOCKED_PREF_STRING = "checkbox_vr_head_locked";
    private static final String VR_DISTANCE_PREF_STRING = "seekbar_vr_distance";
    private static final String VR_SCREEN_SIZE_PREF_STRING = "seekbar_vr_screen_size";
    private static final String VR_CURVATURE_PREF_STRING = "seekbar_vr_curvature";
    public static final String VR_DEPTH_SOURCE_PREF_STRING = "list_vr_depth_source";
    public static final String VR_ENV_RES_PREF_STRING = "list_vr_env_res";
    public static final String VR_SHARPENING_PREF_STRING = "list_vr_sharpening";
    private static final String VR_EYE_SWAP_PREF_STRING = "checkbox_vr_eye_swap";
    public static final String VR_PASSTHROUGH_PREF_STRING = "checkbox_vr_passthrough";
    private static final String VR_POINTER_PREF_STRING = "checkbox_vr_pointer";
    private static final String VR_GAZE_PREF_STRING = "checkbox_vr_gaze";
    private static final String VR_HAND_TRACKING_PREF_STRING = "checkbox_vr_hand_tracking";
    // Not a setting, this is where a screen moved with the controllers is kept
    public static final String VR_SCREEN_POSE_PREF_STRING = "vr_screen_pose";
    // Nor is this: the cell the environment grid was last left on. Legacy, and
    // only read now to move an old install onto the ids below.
    public static final String VR_ENVIRONMENT_PREF_STRING = "vr_environment";
    // Where that choice lives now. An id names an environment and is forever, a
    // cell is only where it currently sits in the grid, so rearranging the grid
    // cannot scramble what anyone had picked.
    public static final String VR_ENVIRONMENT_ID_PREF_STRING = "vr_environment_id";
    public static final int VR_ENV_PASSTHROUGH = 0;
    public static final int VR_ENV_VOID = 1;
    public static final int VR_ENV_MINIMAL_ROOM = 2;
    public static final int VR_ENV_PSX_CINEMA = 3;
    // The 360 photos start here, numbered by their order in the assets folder,
    // well clear of the rooms so both lists can grow
    public static final int VR_ENV_FIRST_PHOTO = 100;
    // Nor is this: a marker that the Gen 1 profile decision has been made
    public static final String GEN1_PROFILE_PREF_STRING = "perf_profile_gen1";
    public static final String VR_SEPARATION_PREF_STRING = "seekbar_vr_separation";
    private static final String VR_DEPTH_DEBUG_PREF_STRING = "checkbox_vr_depth_debug";
    private static final String VR_INFERENCE_CADENCE_PREF_STRING = "seekbar_vr_inference_cadence";
    public static final String VR_CONVERGENCE_PREF_STRING = "seekbar_vr_convergence";
    private static final String VR_DEPTH_SCALE_PREF_STRING = "seekbar_vr_depth_scale";
    public static final String VR_AMBILIGHT_PREF_STRING = "checkbox_vr_ambilight";
    public static final String VR_AMBILIGHT_LEVEL_PREF_STRING = "seekbar_vr_ambilight_level";
    public static final String VR_ROOM_LIGHT_PREF_STRING = "checkbox_vr_room_light";
    public static final String FILE_LOG_PREF_STRING = "list_vr_file_log";
    private static final String BIND_ALL_USB_STRING = "checkbox_usb_bind_all";
    private static final String MOUSE_EMULATION_STRING = "checkbox_mouse_emulation";
    private static final String ANALOG_SCROLLING_PREF_STRING = "analog_scrolling";
    private static final String MOUSE_NAV_BUTTONS_STRING = "checkbox_mouse_nav_buttons";
    static final String UNLOCK_FPS_STRING = "checkbox_unlock_fps";
    private static final String VIBRATE_OSC_PREF_STRING = "checkbox_vibrate_osc";
    private static final String VIBRATE_FALLBACK_PREF_STRING = "checkbox_vibrate_fallback";
    private static final String VIBRATE_FALLBACK_STRENGTH_PREF_STRING = "seekbar_vibrate_fallback_strength";
    private static final String FLIP_FACE_BUTTONS_PREF_STRING = "checkbox_flip_face_buttons";
    private static final String TOUCHSCREEN_TRACKPAD_PREF_STRING = "checkbox_touchscreen_trackpad";
    private static final String LATENCY_TOAST_PREF_STRING = "checkbox_enable_post_stream_toast";
    private static final String FRAME_PACING_PREF_STRING = "frame_pacing";
    private static final String ABSOLUTE_MOUSE_MODE_PREF_STRING = "checkbox_absolute_mouse_mode";
    private static final String ENABLE_AUDIO_FX_PREF_STRING = "checkbox_enable_audiofx";
    private static final String REDUCE_REFRESH_RATE_PREF_STRING = "checkbox_reduce_refresh_rate";
    private static final String FULL_RANGE_PREF_STRING = "checkbox_full_range";
    private static final String GAMEPAD_TOUCHPAD_AS_MOUSE_PREF_STRING = "checkbox_gamepad_touchpad_as_mouse";
    private static final String GAMEPAD_MOTION_SENSORS_PREF_STRING = "checkbox_gamepad_motion_sensors";
    private static final String GAMEPAD_MOTION_FALLBACK_PREF_STRING = "checkbox_gamepad_motion_fallback";

    // 1440p everywhere. 4K looks better on a headset panel but costs decode
    // latency and host bitrate, and it is one list entry away for anyone who
    // wants it.
    static final String DEFAULT_RESOLUTION = "2560x1440";
    static final String DEFAULT_FPS = "90";

    // What a Gen 1 headset starts on. The depth model is most of the cost of a
    // 3D frame, so the cadence is the big lever here, and 72 is what these
    // panels run at natively anyway.
    private static final String GEN1_RESOLUTION = "2560x1440";
    private static final String GEN1_FPS = "72";
    private static final int GEN1_INFERENCE_CADENCE = 6;
    // These headsets have neither the memory nor the fill rate for a full size
    // room, so they start on the smallest tier
    private static final String GEN1_ENV_RES = "low";

    private static final boolean DEFAULT_STRETCH = false;
    private static final boolean DEFAULT_SOPS = true;
    private static final boolean DEFAULT_DISABLE_TOASTS = false;
    private static final boolean DEFAULT_HOST_AUDIO = false;
    private static final int DEFAULT_DEADZONE = 7;
    private static final int DEFAULT_OPACITY = 90;
    public static final String DEFAULT_LANGUAGE = "default";
    private static final boolean DEFAULT_MULTI_CONTROLLER = true;
    private static final boolean DEFAULT_USB_DRIVER = true;
    private static final String DEFAULT_VIDEO_FORMAT = "auto";

    private static final boolean ONSCREEN_CONTROLLER_DEFAULT = false;
    private static final boolean ONLY_L3_R3_DEFAULT = false;
    private static final boolean SHOW_GUIDE_BUTTON_DEFAULT = true;
    private static final boolean DEFAULT_ENABLE_HDR = false;
    private static final boolean DEFAULT_ENABLE_PIP = false;
    private static final boolean DEFAULT_ENABLE_PERF_OVERLAY = false;
    private static final boolean DEFAULT_ENABLE_GL_RENDER_PATH = false;
    private static final boolean DEFAULT_ENABLE_VR_MODE = true;
    private static final boolean DEFAULT_VR_HEAD_LOCKED = false;
    // Tenths of a metre. 2 m at 3 m wide was reported too close and too
    // large, 3 m at 3 m wide is what a sustained session settled on.
    private static final int DEFAULT_VR_DISTANCE = 30;
    private static final int DEFAULT_VR_SCREEN_SIZE = 30;
    private static final int DEFAULT_VR_CURVATURE = 0;
    private static final String DEFAULT_VR_DEPTH_SOURCE = "model";
    // Standard everywhere, which caps the room at a size every headset here can
    // hold. Gen 1 headsets are seeded onto low instead, see seedGen1PerfProfile.
    private static final String DEFAULT_VR_ENV_RES = "standard";
    private static final String DEFAULT_VR_SHARPENING = "quality";
    private static final boolean DEFAULT_VR_EYE_SWAP = false;
    private static final boolean DEFAULT_VR_PASSTHROUGH = false;
    private static final boolean DEFAULT_VR_GAZE = true;
    private static final boolean DEFAULT_VR_HAND_TRACKING = true;
    private static final boolean DEFAULT_VR_POINTER = true;
    // Tenths of a percent of frame width. 5 measured comfortable on device and
    // 7 already strained, once the depth map started using its full range.
    public static final int DEFAULT_VR_SEPARATION = 5;
    private static final boolean DEFAULT_VR_DEPTH_DEBUG = false;
    private static final int DEFAULT_VR_INFERENCE_CADENCE = 3;
    // Neither of these is in the 2d settings. Measured on device, neither is
    // perceptible at a comfortable separation, so they would be sliders that
    // do nothing. Convergence is on the in headset panel instead, where it sits
    // beside the depth slider that gives it something to do.
    public static final int DEFAULT_VR_CONVERGENCE = 50;
    private static final int DEFAULT_VR_DEPTH_SCALE = 100;
    // On at half strength, which is where the panel's tick sits. Anyone who has
    // already turned it off keeps their saved value
    public static final boolean DEFAULT_VR_AMBILIGHT = true;
    public static final int DEFAULT_VR_AMBILIGHT_LEVEL = 50;
    // Inside a room the light off the picture is most of what makes the place
    // look lit at all
    public static final boolean DEFAULT_VR_ROOM_LIGHT = true;
    // Warnings and errors by default. The file is small, and a report that
    // arrives without one is a round trip nobody wants.
    public static final String DEFAULT_FILE_LOG = "basic";
    private static final boolean DEFAULT_BIND_ALL_USB = false;
    private static final boolean DEFAULT_MOUSE_EMULATION = true;
    private static final String DEFAULT_ANALOG_STICK_FOR_SCROLLING = "right";
    private static final boolean DEFAULT_MOUSE_NAV_BUTTONS = false;
    private static final boolean DEFAULT_UNLOCK_FPS = false;
    private static final boolean DEFAULT_VIBRATE_OSC = true;
    private static final boolean DEFAULT_VIBRATE_FALLBACK = false;
    private static final int DEFAULT_VIBRATE_FALLBACK_STRENGTH = 100;
    private static final boolean DEFAULT_FLIP_FACE_BUTTONS = false;
    private static final boolean DEFAULT_TOUCHSCREEN_TRACKPAD = true;
    private static final String DEFAULT_AUDIO_CONFIG = "2"; // Stereo
    private static final boolean DEFAULT_LATENCY_TOAST = false;
    private static final String DEFAULT_FRAME_PACING = "latency";
    private static final boolean DEFAULT_ABSOLUTE_MOUSE_MODE = false;
    private static final boolean DEFAULT_ENABLE_AUDIO_FX = false;
    private static final boolean DEFAULT_REDUCE_REFRESH_RATE = false;
    private static final boolean DEFAULT_FULL_RANGE = false;
    private static final boolean DEFAULT_GAMEPAD_TOUCHPAD_AS_MOUSE = false;
    private static final boolean DEFAULT_GAMEPAD_MOTION_SENSORS = true;
    private static final boolean DEFAULT_GAMEPAD_MOTION_FALLBACK = false;

    // EnvResTier, matching the values in xr_renderer.c
    public static final int VR_ENV_RES_LOW = 0;
    public static final int VR_ENV_RES_STANDARD = 1;
    public static final int VR_ENV_RES_HIGH = 2;
    public static final int VR_ENV_RES_ULTRA = 3;

    public static final int FRAME_PACING_MIN_LATENCY = 0;
    public static final int FRAME_PACING_BALANCED = 1;
    public static final int FRAME_PACING_CAP_FPS = 2;
    public static final int FRAME_PACING_MAX_SMOOTHNESS = 3;

    public static final String RES_360P = "640x360";
    public static final String RES_480P = "854x480";
    public static final String RES_720P = "1280x720";
    public static final String RES_1080P = "1920x1080";
    public static final String RES_1440P = "2560x1440";
    public static final String RES_4K = "3840x2160";
    public static final String RES_NATIVE = "Native";

    public int width, height, fps;
    public int bitrate;
    public FormatOption videoFormat;
    public int deadzonePercentage;
    public int oscOpacity;
    public boolean stretchVideo, enableSops, playHostAudio, disableWarnings;
    public String language;
    public boolean smallIconMode, multiController, usbDriver, flipFaceButtons;
    public boolean onscreenController;
    public boolean onlyL3R3;
    public boolean showGuideButton;
    public boolean enableHdr;
    public boolean enablePip;
    public boolean enablePerfOverlay;
    public boolean enableGlRenderPath;
    public boolean enableVrMode;
    public boolean vrHeadLocked;
    // Tenths of a meter
    public int vrDistance;
    public int vrScreenSize;
    // 0 to 100
    public int vrCurvature;
    // 0 off, 1 flat, 2 ramp, 3 blob, 4 eye test, 5 shift test, 6 depth model
    public int vrDepthMode;
    // How large the 3d room renders per eye, one of the tiers below. The
    // renderer takes the same numbers.
    public int vrEnvResTier;
    // 0 off, 1 normal, 2 quality
    public int vrSharpening;
    public boolean vrEyeSwap;
    // Tenths of a percent of frame width
    public int vrStereoSeparation;
    public boolean vrDepthDebug;
    public boolean vrPassthrough;
    public boolean vrPointer;
    public boolean vrGaze;
    public boolean vrHandTracking;
    // Run the depth model on every Nth video frame
    public int vrInferenceCadence;
    public int vrConvergence;
    public int vrDepthScale;
    // Colours from the frame bleeding into the space around the screen, and
    // how strong that is as a percentage
    public boolean vrAmbilight;
    public int vrAmbilightLevel;
    // The same colours washed over the walls of a 3d environment
    public boolean vrRoomLight;
    // off, basic or verbose
    public String fileLogLevel;
    public boolean enableLatencyToast;
    public boolean bindAllUsb;
    public boolean mouseEmulation;
    public AnalogStickForScrolling analogStickForScrolling;
    public boolean mouseNavButtons;
    public boolean unlockFps;
    public boolean vibrateOsc;
    public boolean vibrateFallbackToDevice;
    public int vibrateFallbackToDeviceStrength;
    public boolean touchscreenTrackpad;
    public MoonBridge.AudioConfiguration audioConfiguration;
    public int framePacing;
    public boolean absoluteMouseMode;
    public boolean enableAudioFx;
    public boolean reduceRefreshRate;
    public boolean fullRange;
    public boolean gamepadMotionSensors;
    public boolean gamepadTouchpadAsMouse;
    public boolean gamepadMotionSensorsFallbackToDevice;

    public static boolean isNativeResolution(int width, int height) {
        // It's not a native resolution if it matches an existing resolution option
        if (width == 640 && height == 360) {
            return false;
        }
        else if (width == 854 && height == 480) {
            return false;
        }
        else if (width == 1280 && height == 720) {
            return false;
        }
        else if (width == 1920 && height == 1080) {
            return false;
        }
        else if (width == 2560 && height == 1440) {
            return false;
        }
        else if (width == 3840 && height == 2160) {
            return false;
        }

        return true;
    }

    // If we have a screen that has semi-square dimensions, we may want to change our behavior
    // to allow any orientation and vertical+horizontal resolutions.
    public static boolean isSquarishScreen(int width, int height) {
        float longDim = Math.max(width, height);
        float shortDim = Math.min(width, height);

        // We just put the arbitrary cutoff for a square-ish screen at 1.3
        return longDim / shortDim < 1.3f;
    }

    public static boolean isSquarishScreen(Display display) {
        int width, height;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            width = display.getMode().getPhysicalWidth();
            height = display.getMode().getPhysicalHeight();
        }
        else {
            width = display.getWidth();
            height = display.getHeight();
        }

        return isSquarishScreen(width, height);
    }

    private static String convertFromLegacyResolutionString(String resString) {
        if (resString.equalsIgnoreCase("360p")) {
            return RES_360P;
        }
        else if (resString.equalsIgnoreCase("480p")) {
            return RES_480P;
        }
        else if (resString.equalsIgnoreCase("720p")) {
            return RES_720P;
        }
        else if (resString.equalsIgnoreCase("1080p")) {
            return RES_1080P;
        }
        else if (resString.equalsIgnoreCase("1440p")) {
            return RES_1440P;
        }
        else if (resString.equalsIgnoreCase("4K")) {
            return RES_4K;
        }
        else {
            // Should be unreachable
            return RES_720P;
        }
    }

    private static int getWidthFromResolutionString(String resString) {
        return Integer.parseInt(resString.split("x")[0]);
    }

    private static int getHeightFromResolutionString(String resString) {
        return Integer.parseInt(resString.split("x")[1]);
    }

    private static String getResolutionString(int width, int height) {
        switch (height) {
            case 360:
                return RES_360P;
            case 480:
                return RES_480P;
            default:
            case 720:
                return RES_720P;
            case 1080:
                return RES_1080P;
            case 1440:
                return RES_1440P;
            case 2160:
                return RES_4K;
        }
    }

    // Quest 2, Quest Pro and Pico 4 are the XR2 Gen 1 headsets and have a lot
    // less GPU headroom than the Gen 2 devices this was tuned on, so a full
    // rate 3D stream is too much for them.
    public static boolean isXr2Gen1Headset() {
        String model = Build.MODEL != null ? Build.MODEL : "";

        // Meta puts the marketing name in the model, but the board name is the
        // more stable of the two, so check both.
        if (model.equalsIgnoreCase("Quest 2") || model.equalsIgnoreCase("Quest Pro") ||
                "hollywood".equalsIgnoreCase(Build.DEVICE) || "seacliff".equalsIgnoreCase(Build.DEVICE)) {
            return true;
        }

        // Pico ships a model number rather than a name. The 4 and the 4
        // Enterprise are A81xx, the later headsets are not.
        boolean isPico = "pico".equalsIgnoreCase(Build.MANUFACTURER) || "pico".equalsIgnoreCase(Build.BRAND);
        return isPico && model.regionMatches(true, 0, "A81", 0, 3);
    }

    // A Gen 1 headset gets a gentler starting point, written before the xml
    // defaults are applied so it only lands on a fresh install. Whether the
    // key exists says the decision was made, its value says the profile was
    // actually applied rather than an existing install being left alone.
    public static void seedGen1PerfProfile(Context context) {
        if (!isXr2Gen1Headset()) {
            return;
        }

        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);

        // The room size tier is newer than the marker, so it is seeded even on
        // an install that already carries one, which would otherwise be moved
        // off the small room it has been drawing all along. The key does not
        // exist until it has been picked or seeded, so a choice already made
        // is still left alone.
        if (!prefs.contains(VR_ENV_RES_PREF_STRING)) {
            prefs.edit().putString(VR_ENV_RES_PREF_STRING, GEN1_ENV_RES).apply();
            FileLog.event("perf profile: XR2 Gen 1 headset, environment res " + GEN1_ENV_RES);
        }

        if (prefs.contains(GEN1_PROFILE_PREF_STRING)) {
            return;
        }

        // The legacy key carries both res and fps, so an install that still
        // has it counts as having chosen them
        boolean seedResFps = !prefs.contains(LEGACY_RES_FPS_PREF_STRING);
        boolean seedResolution = seedResFps && !prefs.contains(RESOLUTION_PREF_STRING);
        boolean seedFps = seedResFps && !prefs.contains(FPS_PREF_STRING);
        boolean seedCadence = !prefs.contains(VR_INFERENCE_CADENCE_PREF_STRING);
        boolean appliedAny = seedResolution || seedFps || seedCadence;

        SharedPreferences.Editor editor = prefs.edit();
        StringBuilder applied = new StringBuilder();
        if (seedResolution) {
            editor.putString(RESOLUTION_PREF_STRING, GEN1_RESOLUTION);
            applied.append(GEN1_RESOLUTION);
        }
        if (seedFps) {
            editor.putString(FPS_PREF_STRING, GEN1_FPS);
            applied.append(applied.length() > 0 ? " " : "").append(GEN1_FPS).append(" fps");
        }
        if (seedCadence) {
            editor.putInt(VR_INFERENCE_CADENCE_PREF_STRING, GEN1_INFERENCE_CADENCE);
            applied.append(applied.length() > 0 ? " " : "").append("cadence ").append(GEN1_INFERENCE_CADENCE);
        }
        editor.putBoolean(GEN1_PROFILE_PREF_STRING, appliedAny);
        editor.apply();

        FileLog.event("perf profile: XR2 Gen 1 headset (" + Build.MANUFACTURER + " " + Build.MODEL
                + "/" + Build.DEVICE + "), "
                + (appliedAny ? "applied " + applied : "existing settings left alone"));
    }

    public static int getDefaultBitrate(String resString, String fpsString) {
        int width = getWidthFromResolutionString(resString);
        int height = getHeightFromResolutionString(resString);
        int fps = Integer.parseInt(fpsString);

        // This logic is shamelessly stolen from Moonlight Qt:
        // https://github.com/moonlight-stream/moonlight-qt/blob/master/app/settings/streamingpreferences.cpp

        // Don't scale bitrate linearly beyond 60 FPS. It's definitely not a linear
        // bitrate increase for frame rate once we get to values that high.
        double frameRateFactor = (fps <= 60 ? fps : (Math.sqrt(fps / 60.f) * 60.f)) / 30.f;

        // TODO: Collect some empirical data to see if these defaults make sense.
        // We're just using the values that the Shield used, as we have for years.
        int[] pixelVals = {
            640 * 360,
            854 * 480,
            1280 * 720,
            1920 * 1080,
            2560 * 1440,
            3840 * 2160,
            -1,
        };
        int[] factorVals = {
            1,
            2,
            5,
            10,
            20,
            40,
            -1
        };

        // Calculate the resolution factor by linear interpolation of the resolution table
        float resolutionFactor;
        int pixels = width * height;
        for (int i = 0; ; i++) {
            if (pixels == pixelVals[i]) {
                // We can bail immediately for exact matches
                resolutionFactor = factorVals[i];
                break;
            }
            else if (pixels < pixelVals[i]) {
                if (i == 0) {
                    // Never go below the lowest resolution entry
                    resolutionFactor = factorVals[i];
                }
                else {
                    // Interpolate between the entry greater than the chosen resolution (i) and the entry less than the chosen resolution (i-1)
                    resolutionFactor = ((float)(pixels - pixelVals[i-1]) / (pixelVals[i] - pixelVals[i-1])) * (factorVals[i] - factorVals[i-1]) + factorVals[i-1];
                }
                break;
            }
            else if (pixelVals[i] == -1) {
                // Never go above the highest resolution entry
                resolutionFactor = factorVals[i-1];
                break;
            }
        }

        return (int)Math.round(resolutionFactor * frameRateFactor) * 1000;
    }

    public static boolean getDefaultSmallMode(Context context) {
        PackageManager manager = context.getPackageManager();
        if (manager != null) {
            // TVs shouldn't use small mode by default
            if (manager.hasSystemFeature(PackageManager.FEATURE_TELEVISION)) {
                return false;
            }

            // API 21 uses LEANBACK instead of TELEVISION
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP_MR1) {
                if (manager.hasSystemFeature(PackageManager.FEATURE_LEANBACK)) {
                    return false;
                }
            }
        }

        // Use small mode on anything smaller than a 7" tablet
        return context.getResources().getConfiguration().smallestScreenWidthDp < 500;
    }

    public static int getDefaultBitrate(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        return getDefaultBitrate(
                prefs.getString(RESOLUTION_PREF_STRING, DEFAULT_RESOLUTION),
                prefs.getString(FPS_PREF_STRING, DEFAULT_FPS));
    }

    private static FormatOption getVideoFormatValue(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);

        String str = prefs.getString(VIDEO_FORMAT_PREF_STRING, DEFAULT_VIDEO_FORMAT);
        if (str.equals("auto")) {
            return FormatOption.AUTO;
        }
        else if (str.equals("forceav1")) {
            return FormatOption.FORCE_AV1;
        }
        else if (str.equals("forceh265")) {
            return FormatOption.FORCE_HEVC;
        }
        else if (str.equals("neverh265")) {
            return FormatOption.FORCE_H264;
        }
        else {
            // Should never get here
            return FormatOption.AUTO;
        }
    }

    private static int getFramePacingValue(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);

        // Migrate legacy never drop frames option to the new location
        if (prefs.contains(LEGACY_DISABLE_FRAME_DROP_PREF_STRING)) {
            boolean legacyNeverDropFrames = prefs.getBoolean(LEGACY_DISABLE_FRAME_DROP_PREF_STRING, false);
            prefs.edit()
                    .remove(LEGACY_DISABLE_FRAME_DROP_PREF_STRING)
                    .putString(FRAME_PACING_PREF_STRING, legacyNeverDropFrames ? "balanced" : "latency")
                    .apply();
        }

        String str = prefs.getString(FRAME_PACING_PREF_STRING, DEFAULT_FRAME_PACING);
        if (str.equals("latency")) {
            return FRAME_PACING_MIN_LATENCY;
        }
        else if (str.equals("balanced")) {
            return FRAME_PACING_BALANCED;
        }
        else if (str.equals("cap-fps")) {
            return FRAME_PACING_CAP_FPS;
        }
        else if (str.equals("smoothness")) {
            return FRAME_PACING_MAX_SMOOTHNESS;
        }
        else {
            // Should never get here
            return FRAME_PACING_MIN_LATENCY;
        }
    }

    private static AnalogStickForScrolling getAnalogStickForScrollingValue(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);

        String str = prefs.getString(ANALOG_SCROLLING_PREF_STRING, DEFAULT_ANALOG_STICK_FOR_SCROLLING);
        if (str.equals("right")) {
            return AnalogStickForScrolling.RIGHT;
        }
        else if (str.equals("left")) {
            return AnalogStickForScrolling.LEFT;
        }
        else {
            return AnalogStickForScrolling.NONE;
        }
    }

    public static void resetStreamingSettings(Context context) {
        // We consider resolution, FPS, bitrate, HDR, and video format as "streaming settings" here
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        prefs.edit()
                .remove(BITRATE_PREF_STRING)
                .remove(BITRATE_PREF_OLD_STRING)
                .remove(LEGACY_RES_FPS_PREF_STRING)
                .remove(RESOLUTION_PREF_STRING)
                .remove(FPS_PREF_STRING)
                .remove(VIDEO_FORMAT_PREF_STRING)
                .remove(ENABLE_HDR_PREF_STRING)
                .remove(UNLOCK_FPS_STRING)
                .remove(FULL_RANGE_PREF_STRING)
                .apply();
    }

    public static void completeLanguagePreferenceMigration(Context context) {
        // Put our language option back to default which tells us that we've already migrated it
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        prefs.edit().putString(LANGUAGE_PREF_STRING, DEFAULT_LANGUAGE).apply();
    }

    public static boolean isShieldAtvFirmwareWithBrokenHdr() {
        // This particular Shield TV firmware crashes when using HDR
        // https://www.nvidia.com/en-us/geforce/forums/notifications/comment/155192/
        return Build.MANUFACTURER.equalsIgnoreCase("NVIDIA") &&
                Build.FINGERPRINT.contains("PPR1.180610.011/4079208_2235.1395");
    }

    public static PreferenceConfiguration readPreferences(Context context) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        PreferenceConfiguration config = new PreferenceConfiguration();

        // Migrate legacy preferences to the new locations
        if (prefs.contains(LEGACY_ENABLE_51_SURROUND_PREF_STRING)) {
            if (prefs.getBoolean(LEGACY_ENABLE_51_SURROUND_PREF_STRING, false)) {
                prefs.edit()
                        .remove(LEGACY_ENABLE_51_SURROUND_PREF_STRING)
                        .putString(AUDIO_CONFIG_PREF_STRING, "51")
                        .apply();
            }
        }

        String str = prefs.getString(LEGACY_RES_FPS_PREF_STRING, null);
        if (str != null) {
            if (str.equals("360p30")) {
                config.width = 640;
                config.height = 360;
                config.fps = 30;
            }
            else if (str.equals("360p60")) {
                config.width = 640;
                config.height = 360;
                config.fps = 60;
            }
            else if (str.equals("720p30")) {
                config.width = 1280;
                config.height = 720;
                config.fps = 30;
            }
            else if (str.equals("720p60")) {
                config.width = 1280;
                config.height = 720;
                config.fps = 60;
            }
            else if (str.equals("1080p30")) {
                config.width = 1920;
                config.height = 1080;
                config.fps = 30;
            }
            else if (str.equals("1080p60")) {
                config.width = 1920;
                config.height = 1080;
                config.fps = 60;
            }
            else if (str.equals("4K30")) {
                config.width = 3840;
                config.height = 2160;
                config.fps = 30;
            }
            else if (str.equals("4K60")) {
                config.width = 3840;
                config.height = 2160;
                config.fps = 60;
            }
            else {
                // Should never get here
                config.width = 1280;
                config.height = 720;
                config.fps = 60;
            }

            prefs.edit()
                    .remove(LEGACY_RES_FPS_PREF_STRING)
                    .putString(RESOLUTION_PREF_STRING, getResolutionString(config.width, config.height))
                    .putString(FPS_PREF_STRING, ""+config.fps)
                    .apply();
        }
        else {
            // Use the new preference location
            String resStr = prefs.getString(RESOLUTION_PREF_STRING, PreferenceConfiguration.DEFAULT_RESOLUTION);

            // Convert legacy resolution strings to the new style
            if (!resStr.contains("x")) {
                resStr = PreferenceConfiguration.convertFromLegacyResolutionString(resStr);
                prefs.edit().putString(RESOLUTION_PREF_STRING, resStr).apply();
            }

            config.width = PreferenceConfiguration.getWidthFromResolutionString(resStr);
            config.height = PreferenceConfiguration.getHeightFromResolutionString(resStr);
            config.fps = Integer.parseInt(prefs.getString(FPS_PREF_STRING, PreferenceConfiguration.DEFAULT_FPS));
        }

        if (!prefs.contains(SMALL_ICONS_PREF_STRING)) {
            // We need to write small icon mode's default to disk for the settings page to display
            // the current state of the option properly
            prefs.edit().putBoolean(SMALL_ICONS_PREF_STRING, getDefaultSmallMode(context)).apply();
        }

        if (!prefs.contains(GAMEPAD_MOTION_SENSORS_PREF_STRING) && Build.VERSION.SDK_INT == Build.VERSION_CODES.S) {
            // Android 12 has a nasty bug that causes crashes when the app touches the InputDevice's
            // associated InputDeviceSensorManager (just calling getSensorManager() is enough).
            // As a workaround, we will override the default value for the gamepad motion sensor
            // option to disabled on Android 12 to reduce the impact of this bug.
            // https://cs.android.com/android/_/android/platform/frameworks/base/+/8970010a5e9f3dc5c069f56b4147552accfcbbeb
            prefs.edit().putBoolean(GAMEPAD_MOTION_SENSORS_PREF_STRING, false).apply();
        }

        // This must happen after the preferences migration to ensure the preferences are populated
        config.bitrate = prefs.getInt(BITRATE_PREF_STRING, prefs.getInt(BITRATE_PREF_OLD_STRING, 0) * 1000);
        if (config.bitrate == 0) {
            config.bitrate = getDefaultBitrate(context);
        }

        String audioConfig = prefs.getString(AUDIO_CONFIG_PREF_STRING, DEFAULT_AUDIO_CONFIG);
        if (audioConfig.equals("71")) {
            config.audioConfiguration = MoonBridge.AUDIO_CONFIGURATION_71_SURROUND;
        }
        else if (audioConfig.equals("51")) {
            config.audioConfiguration = MoonBridge.AUDIO_CONFIGURATION_51_SURROUND;
        }
        else /* if (audioConfig.equals("2")) */ {
            config.audioConfiguration = MoonBridge.AUDIO_CONFIGURATION_STEREO;
        }

        config.videoFormat = getVideoFormatValue(context);
        config.framePacing = getFramePacingValue(context);

        config.analogStickForScrolling = getAnalogStickForScrollingValue(context);

        config.deadzonePercentage = prefs.getInt(DEADZONE_PREF_STRING, DEFAULT_DEADZONE);

        config.oscOpacity = prefs.getInt(OSC_OPACITY_PREF_STRING, DEFAULT_OPACITY);

        config.language = prefs.getString(LANGUAGE_PREF_STRING, DEFAULT_LANGUAGE);

        // Checkbox preferences
        config.disableWarnings = prefs.getBoolean(DISABLE_TOASTS_PREF_STRING, DEFAULT_DISABLE_TOASTS);
        config.enableSops = prefs.getBoolean(SOPS_PREF_STRING, DEFAULT_SOPS);
        config.stretchVideo = prefs.getBoolean(STRETCH_PREF_STRING, DEFAULT_STRETCH);
        config.playHostAudio = prefs.getBoolean(HOST_AUDIO_PREF_STRING, DEFAULT_HOST_AUDIO);
        config.smallIconMode = prefs.getBoolean(SMALL_ICONS_PREF_STRING, getDefaultSmallMode(context));
        config.multiController = prefs.getBoolean(MULTI_CONTROLLER_PREF_STRING, DEFAULT_MULTI_CONTROLLER);
        config.usbDriver = prefs.getBoolean(USB_DRIVER_PREF_SRING, DEFAULT_USB_DRIVER);
        config.onscreenController = prefs.getBoolean(ONSCREEN_CONTROLLER_PREF_STRING, ONSCREEN_CONTROLLER_DEFAULT);
        config.onlyL3R3 = prefs.getBoolean(ONLY_L3_R3_PREF_STRING, ONLY_L3_R3_DEFAULT);
        config.showGuideButton = prefs.getBoolean(SHOW_GUIDE_BUTTON_PREF_STRING, SHOW_GUIDE_BUTTON_DEFAULT);
        config.enableHdr = prefs.getBoolean(ENABLE_HDR_PREF_STRING, DEFAULT_ENABLE_HDR) && !isShieldAtvFirmwareWithBrokenHdr();
        config.enablePip = prefs.getBoolean(ENABLE_PIP_PREF_STRING, DEFAULT_ENABLE_PIP);
        config.enablePerfOverlay = prefs.getBoolean(ENABLE_PERF_OVERLAY_STRING, DEFAULT_ENABLE_PERF_OVERLAY);
        config.enableGlRenderPath = prefs.getBoolean(ENABLE_GL_RENDER_PATH_PREF_STRING, DEFAULT_ENABLE_GL_RENDER_PATH);
        config.enableVrMode = prefs.getBoolean(ENABLE_VR_MODE_PREF_STRING, DEFAULT_ENABLE_VR_MODE);
        config.vrHeadLocked = prefs.getBoolean(VR_HEAD_LOCKED_PREF_STRING, DEFAULT_VR_HEAD_LOCKED);
        config.vrDistance = prefs.getInt(VR_DISTANCE_PREF_STRING, DEFAULT_VR_DISTANCE);
        config.vrScreenSize = prefs.getInt(VR_SCREEN_SIZE_PREF_STRING, DEFAULT_VR_SCREEN_SIZE);
        config.vrCurvature = prefs.getInt(VR_CURVATURE_PREF_STRING, DEFAULT_VR_CURVATURE);
        String depthSource = prefs.getString(VR_DEPTH_SOURCE_PREF_STRING, DEFAULT_VR_DEPTH_SOURCE);
        if (depthSource.equals("flat")) {
            config.vrDepthMode = 1;
        }
        else if (depthSource.equals("ramp")) {
            config.vrDepthMode = 2;
        }
        else if (depthSource.equals("blob")) {
            config.vrDepthMode = 3;
        }
        else if (depthSource.equals("eyetest")) {
            config.vrDepthMode = 4;
        }
        else if (depthSource.equals("shifttest")) {
            config.vrDepthMode = 5;
        }
        else if (depthSource.equals("model")) {
            config.vrDepthMode = 6;
        }
        else {
            config.vrDepthMode = 0;
        }
        String envRes = prefs.getString(VR_ENV_RES_PREF_STRING, DEFAULT_VR_ENV_RES);
        if (envRes.equals("low")) {
            config.vrEnvResTier = VR_ENV_RES_LOW;
        }
        else if (envRes.equals("high")) {
            config.vrEnvResTier = VR_ENV_RES_HIGH;
        }
        else if (envRes.equals("ultra")) {
            config.vrEnvResTier = VR_ENV_RES_ULTRA;
        }
        else {
            config.vrEnvResTier = VR_ENV_RES_STANDARD;
        }
        String sharpening = prefs.getString(VR_SHARPENING_PREF_STRING, DEFAULT_VR_SHARPENING);
        if (sharpening.equals("off")) {
            config.vrSharpening = 0;
        }
        else if (sharpening.equals("normal")) {
            config.vrSharpening = 1;
        }
        else {
            config.vrSharpening = 2;
        }
        config.vrEyeSwap = prefs.getBoolean(VR_EYE_SWAP_PREF_STRING, DEFAULT_VR_EYE_SWAP);
        config.vrPassthrough = prefs.getBoolean(VR_PASSTHROUGH_PREF_STRING, DEFAULT_VR_PASSTHROUGH);
        config.vrPointer = prefs.getBoolean(VR_POINTER_PREF_STRING, DEFAULT_VR_POINTER);
        config.vrGaze = prefs.getBoolean(VR_GAZE_PREF_STRING, DEFAULT_VR_GAZE);
        config.vrHandTracking = prefs.getBoolean(VR_HAND_TRACKING_PREF_STRING, DEFAULT_VR_HAND_TRACKING);
        config.vrStereoSeparation = prefs.getInt(VR_SEPARATION_PREF_STRING, DEFAULT_VR_SEPARATION);
        config.vrDepthDebug = prefs.getBoolean(VR_DEPTH_DEBUG_PREF_STRING, DEFAULT_VR_DEPTH_DEBUG);
        config.vrInferenceCadence = prefs.getInt(VR_INFERENCE_CADENCE_PREF_STRING, DEFAULT_VR_INFERENCE_CADENCE);
        config.vrConvergence = prefs.getInt(VR_CONVERGENCE_PREF_STRING, DEFAULT_VR_CONVERGENCE);
        config.vrDepthScale = prefs.getInt(VR_DEPTH_SCALE_PREF_STRING, DEFAULT_VR_DEPTH_SCALE);
        config.vrAmbilight = prefs.getBoolean(VR_AMBILIGHT_PREF_STRING, DEFAULT_VR_AMBILIGHT);
        config.vrAmbilightLevel = prefs.getInt(VR_AMBILIGHT_LEVEL_PREF_STRING,
                DEFAULT_VR_AMBILIGHT_LEVEL);
        config.vrRoomLight = prefs.getBoolean(VR_ROOM_LIGHT_PREF_STRING, DEFAULT_VR_ROOM_LIGHT);
        config.fileLogLevel = prefs.getString(FILE_LOG_PREF_STRING, DEFAULT_FILE_LOG);
        config.bindAllUsb = prefs.getBoolean(BIND_ALL_USB_STRING, DEFAULT_BIND_ALL_USB);
        config.mouseEmulation = prefs.getBoolean(MOUSE_EMULATION_STRING, DEFAULT_MOUSE_EMULATION);
        config.mouseNavButtons = prefs.getBoolean(MOUSE_NAV_BUTTONS_STRING, DEFAULT_MOUSE_NAV_BUTTONS);
        config.unlockFps = prefs.getBoolean(UNLOCK_FPS_STRING, DEFAULT_UNLOCK_FPS);
        config.vibrateOsc = prefs.getBoolean(VIBRATE_OSC_PREF_STRING, DEFAULT_VIBRATE_OSC);
        config.vibrateFallbackToDevice = prefs.getBoolean(VIBRATE_FALLBACK_PREF_STRING, DEFAULT_VIBRATE_FALLBACK);
        config.vibrateFallbackToDeviceStrength = prefs.getInt(VIBRATE_FALLBACK_STRENGTH_PREF_STRING, DEFAULT_VIBRATE_FALLBACK_STRENGTH);
        config.flipFaceButtons = prefs.getBoolean(FLIP_FACE_BUTTONS_PREF_STRING, DEFAULT_FLIP_FACE_BUTTONS);
        config.touchscreenTrackpad = prefs.getBoolean(TOUCHSCREEN_TRACKPAD_PREF_STRING, DEFAULT_TOUCHSCREEN_TRACKPAD);
        config.enableLatencyToast = prefs.getBoolean(LATENCY_TOAST_PREF_STRING, DEFAULT_LATENCY_TOAST);
        config.absoluteMouseMode = prefs.getBoolean(ABSOLUTE_MOUSE_MODE_PREF_STRING, DEFAULT_ABSOLUTE_MOUSE_MODE);
        config.enableAudioFx = prefs.getBoolean(ENABLE_AUDIO_FX_PREF_STRING, DEFAULT_ENABLE_AUDIO_FX);
        config.reduceRefreshRate = prefs.getBoolean(REDUCE_REFRESH_RATE_PREF_STRING, DEFAULT_REDUCE_REFRESH_RATE);
        config.fullRange = prefs.getBoolean(FULL_RANGE_PREF_STRING, DEFAULT_FULL_RANGE);
        config.gamepadTouchpadAsMouse = prefs.getBoolean(GAMEPAD_TOUCHPAD_AS_MOUSE_PREF_STRING, DEFAULT_GAMEPAD_TOUCHPAD_AS_MOUSE);
        config.gamepadMotionSensors = prefs.getBoolean(GAMEPAD_MOTION_SENSORS_PREF_STRING, DEFAULT_GAMEPAD_MOTION_SENSORS);
        config.gamepadMotionSensorsFallbackToDevice = prefs.getBoolean(GAMEPAD_MOTION_FALLBACK_PREF_STRING, DEFAULT_GAMEPAD_MOTION_FALLBACK);

        return config;
    }
}
