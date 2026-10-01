package com.limelight.binding.video;

import com.limelight.R;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Why a VR session did not start, as the native start reported it: the step
 * it stopped at, the call there and what the runtime answered, the runtime
 * if an instance was made, and the extensions it offered and those asked
 * for. Plain values, so the block the log and a report carry and the one
 * line the flat panel shows can both be checked off a test.
 */
public final class XrStartFailure {

    // The order nativeTakeStartFailure fills its array in
    static final int FIELD_STEP = 0;
    static final int FIELD_CALL = 1;
    static final int FIELD_RESULT = 2;
    static final int FIELD_RESULT_NAME = 3;
    static final int FIELD_DETAIL = 4;
    static final int FIELD_RUNTIME = 5;
    static final int FIELD_OFFERED = 6;
    static final int FIELD_REQUESTED = 7;
    static final int FIELDS = 8;

    // What the instance cannot be made without
    static final String[] REQUIRED = { "XR_KHR_opengl_es_enable", "XR_KHR_android_create_instance" };

    /** Where it stopped: loader, runtime, extensions, instance, system, graphics, egl, session, space, swapchain, gl, timeout or unknown. */
    public final String step;
    /** The call that failed there, empty where it was no one call. */
    public final String call;
    /** The XrResult, 0 where the step failed without one. */
    public final int result;
    /** The XrResult by name, empty with no result. */
    public final String resultName;
    public final String detail;
    /** The runtime's name and version, or null when no instance was made. */
    public final String runtime;
    /** What the runtime offered, empty when it offered nothing, null when not known. */
    public final String[] offered;
    /** What the instance asked for, empty when it never got that far, null when not known. */
    public final String[] requested;
    // How long a start that never answered was waited for
    private final int waitedSeconds;

    XrStartFailure(String step, String call, int result, String resultName, String detail,
                   String runtime, String[] offered, String[] requested) {
        this(step, call, result, resultName, detail, runtime, offered, requested, 0);
    }

    private XrStartFailure(String step, String call, int result, String resultName,
                           String detail, String runtime, String[] offered, String[] requested,
                           int waitedSeconds) {
        this.step = step == null || step.isEmpty() ? "unknown" : step;
        this.call = call == null ? "" : call;
        this.result = result;
        this.resultName = resultName == null ? "" : resultName;
        this.detail = detail == null ? "" : detail;
        this.runtime = runtime == null || runtime.isEmpty() ? null : runtime;
        this.offered = offered;
        this.requested = requested;
        this.waitedSeconds = waitedSeconds;
    }

    /** What the native start handed over, or an unknown stop if it handed over nothing usable. */
    static XrStartFailure fromNative(String[] fields) {
        if (fields == null || fields.length < FIELDS) {
            return unknown();
        }
        int result = 0;
        try {
            result = Integer.parseInt(fields[FIELD_RESULT]);
        }
        catch (NumberFormatException e) {
            // Left at none
        }
        return new XrStartFailure(fields[FIELD_STEP], fields[FIELD_CALL], result,
                fields[FIELD_RESULT_NAME], fields[FIELD_DETAIL], fields[FIELD_RUNTIME],
                names(fields[FIELD_OFFERED]), names(fields[FIELD_REQUESTED]));
    }

    /** A start that had not answered when the wait for it ran out. */
    static XrStartFailure timedOut(int seconds) {
        return new XrStartFailure("timeout", "", 0, "", "no answer within " + seconds + " s",
                null, null, null, seconds);
    }

    /** A start that failed without saying where. */
    static XrStartFailure unknown() {
        return new XrStartFailure("unknown", "", 0, "", "", null, null, null);
    }

    private static String[] names(String list) {
        if (list == null || list.trim().isEmpty()) {
            return new String[0];
        }
        return list.trim().split("\\s+");
    }

    /** The required extensions the runtime did not offer, empty when it is not known. */
    List<String> missing() {
        List<String> out = new ArrayList<>();
        if (offered == null) {
            return out;
        }
        List<String> have = Arrays.asList(offered);
        for (String name : REQUIRED) {
            if (!have.contains(name)) {
                out.add(name);
            }
        }
        return out;
    }

    // The answer where there is one, else what was said about it, else the call
    private String answer() {
        if (!resultName.isEmpty()) {
            return resultName;
        }
        if (result != 0) {
            return Integer.toString(result);
        }
        if (!detail.isEmpty()) {
            return detail;
        }
        return call;
    }

    /** The string the flat panel's one line reason comes from. */
    public int reasonRes() {
        switch (step) {
            case "loader":
            case "runtime":
                return R.string.vr_fail_no_runtime;
            case "extensions":
                return R.string.vr_fail_extensions;
            case "instance":
                return R.string.vr_fail_instance;
            case "system":
                return R.string.vr_fail_system;
            case "graphics":
                return R.string.vr_fail_graphics;
            case "egl":
                return R.string.vr_fail_egl;
            case "session":
                return R.string.vr_fail_session;
            case "space":
                return R.string.vr_fail_space;
            case "swapchain":
                return R.string.vr_fail_swapchain;
            case "gl":
                return R.string.vr_fail_gl;
            case "timeout":
                return R.string.vr_fail_timeout;
            default:
                return R.string.vr_fail_unknown;
        }
    }

    /** What goes in that string's placeholder, or null for a string without one. */
    public String reasonArg() {
        switch (step) {
            case "extensions": {
                List<String> missing = missing();
                return missing.isEmpty() ? detail : join(", ", missing);
            }
            case "instance":
            case "system":
            case "graphics":
            case "session":
            case "space":
            case "swapchain":
                return answer();
            case "egl":
                return call.isEmpty() ? detail : call + ", " + detail;
            case "timeout":
                return Integer.toString(waitedSeconds);
            default:
                return null;
        }
    }

    /**
     * Everything known about the stop, a heading and indented lines, for the
     * log and for a report. when says when it was, for a copy kept apart
     * from the log, or null.
     */
    public String block(String when) {
        StringBuilder b = new StringBuilder("VR session did not start");
        if (when != null) {
            b.append(", ").append(when);
        }
        b.append('\n');
        b.append("  stopped at: ").append(step);
        if (!call.isEmpty()) {
            b.append(" (").append(call).append(')');
        }
        b.append('\n');
        if (result != 0 || !resultName.isEmpty()) {
            b.append("  result: ").append(resultName.isEmpty() ? "?" : resultName)
                    .append(" (").append(result).append(")\n");
        }
        if (!detail.isEmpty()) {
            b.append("  detail: ").append(detail).append('\n');
        }
        b.append("  runtime: ").append(runtime != null ? runtime : "none, no instance was made")
                .append('\n');
        if (requested != null && requested.length > 0) {
            b.append("  asked for ").append(requested.length).append(": ")
                    .append(join(" ", Arrays.asList(requested))).append('\n');
        }
        if (offered == null) {
            b.append("  offered: not known\n");
        }
        else if (offered.length == 0) {
            b.append("  offered: nothing\n");
        }
        else {
            b.append("  offered ").append(offered.length).append(": ")
                    .append(join(" ", Arrays.asList(offered))).append('\n');
        }
        return b.toString();
    }

    private static String join(String with, List<String> parts) {
        StringBuilder b = new StringBuilder();
        for (String part : parts) {
            if (b.length() > 0) {
                b.append(with);
            }
            b.append(part);
        }
        return b.toString();
    }
}
