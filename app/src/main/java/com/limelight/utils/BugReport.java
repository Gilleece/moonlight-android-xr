package com.limelight.utils;

import android.content.Context;
import android.os.Build;
import android.preference.PreferenceManager;

import com.limelight.BuildConfig;
import com.limelight.FileLog;
import com.limelight.ReportContentProvider;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.GZIPOutputStream;

/**
 * What a report of a problem is and how it leaves the headset, for both
 * places one can be made: the screen in the settings and the sheet on the
 * About tab inside a session. A report is one text file: what the user typed,
 * the app and the headset, the settings that matter and both log files. It is
 * always saved on the device first, beside the log where only the newest few
 * are kept, then posted gzipped to the collector when the build knows of one,
 * so a failed post still leaves something to send by hand. The text is put
 * together from plain values, so what a report says can be checked off a test
 * rather than a headset.
 */
public final class BugReport {

    /** How a report went. */
    public enum Result {
        /** Saved, and the collector took it */
        SENT,
        /** Saved, but the collector could not be reached or turned it away */
        NOT_SENT,
        /** Saved, and the collector turned it away for its limits rather than a fault */
        BUSY,
        /** Saved, and this build has no collector to send it to */
        SAVED,
        /** Could not be written at all */
        NOT_WRITTEN
    }

    /** How a report went, where it is and what stopped it. */
    public static final class Outcome {
        public final Result result;
        /** Where the report was saved, or null where it was not */
        public final String path;
        /** What went wrong, for the results that went wrong, or null */
        public final String detail;

        Outcome(Result result, String path, String detail) {
            this.result = result;
            this.path = path;
            this.detail = detail;
        }
    }

    /** What a report says about the app, the headset and the settings. */
    public static final class Details {
        public String version = "";
        public String packageName = "";
        /** The commit the build came from, empty for a build without one */
        public String commit = "";
        public boolean root;
        public boolean debug;
        public String manufacturer = "";
        public String brand = "";
        public String model = "";
        public String device = "";
        public String androidRelease = "";
        public int sdk;
        public String buildDisplay = "";
        public boolean headset;
        /** The OpenXR runtime the last session ran on, or null before there was one */
        public String runtime;
        /** Where the last VR start stopped, if it did and none has started since, or null */
        public String startFailure;
        /** The settings lines, each ending in a newline */
        public String settings = "";
        /** What the session in progress has up, or null outside one */
        public String session;
    }

    /** Hands a report to the collector. A test swaps in its own. */
    public interface Transport {
        /** Posts the file as the body with those headers, and says what the server answered. */
        int post(String url, Map<String, String> headers, File body) throws IOException;
    }

    /** The address a reply goes to, remembered between reports since it never changes. */
    public static final String EMAIL_PREF = "bug_report_email";
    /** The runtime the last session ran on, as its log line gives it. */
    public static final String RUNTIME_PREF = "xr_last_runtime";
    /** The block a failed VR start logged, until a start succeeds. */
    public static final String START_FAILURE_PREF = "xr_start_failure";

    /**
     * The first line of every report. The collector refuses a body that does
     * not unpack to this, so tools/report-worker has the same text.
     */
    static final String HEADER = "Moonlight XR bug report\n";

    private static final String NAME_PREFIX = "moonlight-xr-report-";
    private static final String NAME_SUFFIX = ".txt";
    /** How many reports are kept on the device, the newest. */
    static final int KEEP_REPORTS = 5;
    private static final int CONNECT_TIMEOUT_MS = 15000;
    private static final int READ_TIMEOUT_MS = 30000;
    // Where shared storage is mounted, which the file manager does not show
    private static final String STORAGE_ROOT = "/storage/emulated/0/";

    private BugReport() {
    }

    /** Whether this build knows where to send reports. */
    public static boolean collectorConfigured() {
        return !BuildConfig.REPORT_URL.isEmpty();
    }

    /** Whether a note has anything in it but spaces. */
    public static boolean hasNote(String note) {
        return note != null && !note.trim().isEmpty();
    }

    /**
     * A light check that an address could be answered: one @ with something
     * either side, a dot in what follows it with something either side of
     * that, and no spaces. Not a validation, only a catch for a slip.
     */
    public static boolean looksLikeEmail(String address) {
        String text = address == null ? "" : address.trim();
        int at = text.indexOf('@');
        if (at <= 0 || at != text.lastIndexOf('@') || text.matches(".*\\s.*")) {
            return false;
        }
        String host = text.substring(at + 1);
        return host.indexOf('.') > 0 && host.lastIndexOf('.') < host.length() - 1;
    }

    /** The address is optional, so blank is fine and anything else has to look like one. */
    public static boolean addressOk(String address) {
        return address == null || address.trim().isEmpty() || looksLikeEmail(address);
    }

    /** Whether a report with this note and address can go: a note, and an address that could be answered or none. */
    public static boolean canSend(String note, String address) {
        return hasNote(note) && addressOk(address);
    }

    /**
     * The part of the report before the logs: the note, who it is from, why
     * VR last failed to start if it did, then the app and the headset and the
     * settings.
     */
    public static String compose(String message, String email, Details d) {
        String note = message == null ? "" : message.trim();
        StringBuilder text = new StringBuilder();
        text.append(HEADER);
        text.append("From: ").append(email == null ? "" : email.trim()).append("\n\n");
        text.append(note.isEmpty() ? "(no message)" : note).append('\n');

        // First, since on a headset where VR never starts it is the whole story
        if (d.startFailure != null && !d.startFailure.trim().isEmpty()) {
            text.append("\n----- last VR start -----\n");
            text.append(d.startFailure.trim()).append('\n');
        }

        text.append("\n----- app and device -----\n");
        text.append("moonlight ").append(d.version).append(' ').append(d.packageName)
                .append(d.root ? " root" : "").append(d.debug ? " debug" : "");
        if (d.commit != null && !d.commit.isEmpty()) {
            text.append(" commit ").append(d.commit);
        }
        text.append('\n');
        text.append("device ").append(d.manufacturer).append(' ').append(d.brand).append(' ')
                .append(d.model).append(" (").append(d.device).append(")\n");
        text.append("android ").append(d.androidRelease).append(" sdk ").append(d.sdk)
                .append(" build ").append(d.buildDisplay).append('\n');
        text.append("headset ").append(d.headset).append('\n');
        if (d.runtime != null && !d.runtime.isEmpty()) {
            text.append("runtime ").append(d.runtime).append('\n');
        }

        text.append("\n----- settings -----\n");
        text.append(d.settings);
        if (d.session != null && !d.session.isEmpty()) {
            text.append("session ").append(d.session).append('\n');
        }
        return text.toString();
    }

    /**
     * The headers the collector reads: the token it checks, and the device,
     * version, address and note again in clear, so the mail can say what the
     * report is about without anyone unpacking it. The token goes here and
     * nowhere in the body.
     */
    public static Map<String, String> headers(String token, String device, String version,
                                              String email, String message) {
        Map<String, String> headers = new LinkedHashMap<>();
        headers.put("Content-Type", "application/gzip");
        if (token != null && !token.isEmpty()) {
            headers.put("X-Report-Token", token);
        }
        headers.put("X-Report-Device", headerSafe(device, 120));
        headers.put("X-Report-Version", headerSafe(version, 120));
        headers.put("X-Report-Email", headerSafe(email, 120));
        headers.put("X-Report-Summary", headerSafe(message, 2000));
        return headers;
    }

    // Headers carry printable ASCII on one line and nothing else
    static String headerSafe(String value, int max) {
        String ascii = (value == null ? "" : value).replaceAll("[\\r\\n]+", " / ")
                .replaceAll("[^\\x20-\\x7E]", "?");
        return ascii.length() > max ? ascii.substring(0, max) : ascii;
    }

    /**
     * Writes the report into dir: the text, then each log that exists under
     * a line naming it, and lets go of all but the newest KEEP_REPORTS there.
     * Returns the file.
     */
    public static File write(File dir, String header, File... logs) throws IOException {
        if (dir == null || (!dir.isDirectory() && !dir.mkdirs())) {
            throw new IOException("no writable storage for the report");
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File report = new File(dir, NAME_PREFIX + stamp + NAME_SUFFIX);
        Writer out = new OutputStreamWriter(new FileOutputStream(report), StandardCharsets.UTF_8);
        try {
            out.write(header);
            for (File log : logs) {
                appendLog(out, log);
            }
        } finally {
            out.close();
        }
        prune(dir, KEEP_REPORTS);
        return report;
    }

    /** Whether a file name is one of the reports, as against the logs that share the folder. */
    public static boolean isReportName(String name) {
        return name != null && name.startsWith(NAME_PREFIX) && name.endsWith(NAME_SUFFIX);
    }

    /**
     * Deletes all but the newest keep reports in dir and leaves everything
     * else there alone. The names carry the time they were made, so the
     * newest sort last. Says how many went.
     */
    static int prune(File dir, int keep) {
        File[] files = dir != null ? dir.listFiles() : null;
        if (files == null) {
            return 0;
        }
        List<String> names = new ArrayList<>();
        for (File file : files) {
            if (file.isFile() && isReportName(file.getName())) {
                names.add(file.getName());
            }
        }
        Collections.sort(names);
        int deleted = 0;
        for (int i = 0; i < names.size() - keep; i++) {
            if (new File(dir, names.get(i)).delete()) {
                deleted++;
            }
        }
        return deleted;
    }

    private static void appendLog(Writer out, File log) throws IOException {
        if (log == null || !log.isFile()) {
            return;
        }
        out.write("\n----- " + log.getName() + " -----\n");
        BufferedReader in = new BufferedReader(new InputStreamReader(
                new FileInputStream(log), StandardCharsets.UTF_8));
        try {
            char[] chunk = new char[16384];
            int read;
            while ((read = in.read(chunk)) > 0) {
                out.write(chunk, 0, read);
            }
        } finally {
            in.close();
        }
    }

    /**
     * Whether the collector's answer is one of its limits rather than a
     * fault: too many reports today (429) or a report bigger than it takes
     * (413). Either way the saved copy is the one to send later.
     */
    static boolean busy(int code) {
        return code == 429 || code == 413;
    }

    /**
     * Sends a saved report where a collector is set, and says how it went.
     * With no url the report stays where it was saved, which is where says.
     * It goes gzipped: a log is mostly repetition and shrinks about ten to
     * one, which is kinder to a headset's uplink and keeps the attachment the
     * collector mails well inside what it can handle.
     */
    public static Outcome deliver(File report, String where, String url,
                                  Map<String, String> headers, Transport transport) {
        if (url == null || url.isEmpty()) {
            return new Outcome(Result.SAVED, where, null);
        }
        File packed = new File(report.getParentFile(), report.getName() + ".gz");
        try {
            gzip(report, packed);
            int code = transport.post(url, headers, packed);
            if (code / 100 == 2) {
                return new Outcome(Result.SENT, where, null);
            }
            return new Outcome(busy(code) ? Result.BUSY : Result.NOT_SENT, where,
                    "server answered " + code);
        } catch (IOException e) {
            return new Outcome(Result.NOT_SENT, where,
                    e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName());
        } finally {
            packed.delete();
        }
    }

    /**
     * Everything after the text is put together: written into dir with the
     * logs after it and handed to deliver. Apart from Android, so a test can
     * run it.
     */
    static Outcome fileReport(File dir, String header, File[] logs, String url,
                              Map<String, String> headers, Transport transport) {
        File report;
        try {
            report = write(dir, header, logs);
        } catch (IOException e) {
            return new Outcome(Result.NOT_WRITTEN, null,
                    e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName());
        }
        return deliver(report, report.getAbsolutePath(), url, headers, transport);
    }

    /**
     * The one folder reports are saved in: beside the log, which is where the
     * headset's file manager finds them, or the app's own folder with the log
     * switched off.
     */
    public static File reportDir(Context context) {
        File besideLog = logDir();
        return besideLog != null ? besideLog : ReportContentProvider.reportsDir(context);
    }

    /**
     * The whole of a report from inside a session: saved beside the log and
     * sent where the build has a collector. Blocks for as long as the post
     * takes, so never on the frame loop. session says what the session has
     * up, for a line of its own.
     */
    public static Outcome file(Context context, String message, String email, String session) {
        // Whatever the log's writer thread still holds goes to disk first, so
        // the report carries the lines written seconds ago
        FileLog.flush();
        String header = compose(message, email, gather(context, session));
        Map<String, String> headers = headers(BuildConfig.REPORT_TOKEN, deviceName(),
                BuildConfig.VERSION_NAME, email, message);
        return fileReport(reportDir(context), header, logFiles(), BuildConfig.REPORT_URL,
                headers, HTTP);
    }

    /**
     * The report saved beside the log, for the settings screen, which then
     * decides how it goes on.
     */
    public static File save(Context context, String message, String email) throws IOException {
        FileLog.flush();
        return write(reportDir(context), compose(message, email, gather(context, null)),
                logFiles());
    }

    /** The headers for a report from this headset and this build. */
    public static Map<String, String> headersFor(String email, String message) {
        return headers(BuildConfig.REPORT_TOKEN, deviceName(), BuildConfig.VERSION_NAME, email,
                message);
    }

    /** A path as the headset's file manager shows it, from Download on, or as it is. */
    public static String shortPath(String path) {
        if (path == null) {
            return null;
        }
        return path.startsWith(STORAGE_ROOT) ? path.substring(STORAGE_ROOT.length()) : path;
    }

    private static String deviceName() {
        return Build.MANUFACTURER + " " + Build.MODEL;
    }

    private static File[] logFiles() {
        String current = FileLog.getLogPath();
        return new File[] { FileLog.getPreviousLogFile(),
                current != null ? new File(current) : null };
    }

    private static File logDir() {
        String current = FileLog.getLogPath();
        return current != null ? new File(current).getParentFile() : null;
    }

    /** What the report says about this app, this headset and its settings. */
    public static Details gather(Context context, String session) {
        Details d = new Details();
        d.version = BuildConfig.VERSION_NAME;
        d.packageName = context.getPackageName();
        d.commit = BuildConfig.GIT_HASH;
        d.root = BuildConfig.ROOT_BUILD;
        d.debug = BuildConfig.DEBUG;
        d.manufacturer = Build.MANUFACTURER;
        d.brand = Build.BRAND;
        d.model = Build.MODEL;
        d.device = Build.DEVICE;
        d.androidRelease = Build.VERSION.RELEASE;
        d.sdk = Build.VERSION.SDK_INT;
        d.buildDisplay = Build.DISPLAY;
        d.headset = PreferenceConfiguration.isHeadset(context);
        d.runtime = PreferenceManager.getDefaultSharedPreferences(context)
                .getString(RUNTIME_PREF, null);
        d.startFailure = PreferenceManager.getDefaultSharedPreferences(context)
                .getString(START_FAILURE_PREF, null);
        d.settings = settingsLines(PreferenceConfiguration.readPreferences(context));
        d.session = session;
        return d;
    }

    // The settings a picture problem usually turns on, straight from the same
    // reader the stream uses so the report says what the stream would see
    private static String settingsLines(PreferenceConfiguration prefs) {
        return "res " + prefs.width + "x" + prefs.height + " fps " + prefs.fps
                + " bitrate " + prefs.bitrate + " format " + prefs.videoFormat
                + " pacing " + prefs.framePacing
                + " audio " + prefs.audioConfiguration.channelCount
                + " virtualSurround " + prefs.vrVirtualSurround + "\n"
                + "vr " + prefs.enableVrMode + " depthMode " + prefs.vrDepthMode
                + " stereo3d " + PreferenceConfiguration.stereoAtStartLabel(prefs.vrDepthMode)
                + " depthModel " + prefs.vrDepthModel
                + " depthRate " + prefs.vrDepthRate + " separation " + prefs.vrStereoSeparation
                + " convergence " + prefs.vrConvergence
                + " defaultPair " + PreferenceConfiguration.defaultPairLabel(prefs.vrDepthModel)
                + " preset " + PreferenceConfiguration.presetLabel(prefs.vrStereoSeparation,
                        prefs.vrDepthModel)
                + " envRes " + prefs.vrEnvResTier
                + " sharpening " + prefs.vrSharpening + " supersampling " + prefs.vrSupersampling
                + " passthrough " + prefs.vrPassthrough
                + " environment " + prefs.vrEnvironmentId
                + " " + (prefs.vrRoomLevels == null ? "room none"
                        : prefs.vrRoomLevels.describe(" "))
                + " hands " + prefs.vrHandTracking + " gaze " + prefs.vrGaze
                + " " + PreferenceConfiguration.inputLabel(prefs.vrPointerSleep,
                        prefs.vrShowRay, prefs.vrControllerModel, " ")
                + " " + PreferenceConfiguration.headAimLabel(prefs.vrHeadAim,
                        prefs.vrHeadAimSensitivity, prefs.vrHeadAimDeadZone, " ")
                + " " + PreferenceConfiguration.gamepadToggleLabel(prefs.vrGamepadToggle, " ")
                + " clickSound " + prefs.vrClickSound + " doffGrace " + prefs.vrDoffGrace
                + " " + PreferenceConfiguration.pictureLabel(prefs.vrPicture, " ")
                + " headLocked " + prefs.vrHeadLocked + " ambilight " + prefs.vrAmbilight + "\n"
                + "fileLog " + prefs.fileLogLevel + "\n";
    }

    /** Posts over HTTP with the timeouts a few megabytes over a headset's wifi wants. */
    public static final Transport HTTP = new Transport() {
        @Override
        public int post(String url, Map<String, String> headers, File body) throws IOException {
            HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
            try {
                conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
                conn.setReadTimeout(READ_TIMEOUT_MS);
                conn.setRequestMethod("POST");
                conn.setDoOutput(true);
                conn.setFixedLengthStreamingMode(body.length());
                for (Map.Entry<String, String> header : headers.entrySet()) {
                    conn.setRequestProperty(header.getKey(), header.getValue());
                }
                InputStream in = new FileInputStream(body);
                try {
                    OutputStream out = conn.getOutputStream();
                    byte[] chunk = new byte[16384];
                    int read;
                    while ((read = in.read(chunk)) > 0) {
                        out.write(chunk, 0, read);
                    }
                    out.close();
                } finally {
                    in.close();
                }
                return conn.getResponseCode();
            } finally {
                conn.disconnect();
            }
        }
    };

    private static void gzip(File from, File to) throws IOException {
        FileInputStream in = new FileInputStream(from);
        try {
            GZIPOutputStream out = new GZIPOutputStream(new FileOutputStream(to));
            try {
                byte[] chunk = new byte[16384];
                int read;
                while ((read = in.read(chunk)) > 0) {
                    out.write(chunk, 0, read);
                }
            } finally {
                out.close();
            }
        } finally {
            in.close();
        }
    }
}
