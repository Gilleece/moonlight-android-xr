package com.limelight.utils;

import android.content.Context;
import android.os.Build;
import android.preference.PreferenceManager;

import com.limelight.BuildConfig;
import com.limelight.FileLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.RandomAccessFile;
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

/**
 * What a report of a problem is and how it leaves the headset, for both
 * places one can be made: the screen in the settings and the sheet on the
 * About tab inside a session. A report is what the user typed, the app and
 * the headset, the settings that matter and both log files. It is posted as
 * JSON to the collector the build was made with. Only when that fails, or
 * the build has none, is it saved as one text file beside the log, where only
 * the newest few are kept, and the user told where. The text is put together
 * from plain values, so what a report says can be checked off a test rather
 * than a headset.
 */
public final class BugReport {

    /** How a report went. */
    public enum Result {
        /** The collector took it, and nothing was kept on the headset */
        SENT,
        /** Not sent, as the collector could not be reached or turned it away, so saved */
        NOT_SENT,
        /** Not sent, as the collector is at its limits rather than at fault, so saved */
        BUSY,
        /** Not sent, as this build was made without a collector, so saved */
        NO_COLLECTOR,
        /** Not sent, and it could not be saved either */
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
        /** Posts the body with those headers, and says what the server answered. */
        int post(String url, Map<String, String> headers, byte[] body) throws IOException;
    }

    /** The address a reply goes to, remembered between reports since it never changes. */
    public static final String EMAIL_PREF = "bug_report_email";
    /** The runtime the last session ran on, as its log line gives it. */
    public static final String RUNTIME_PREF = "xr_last_runtime";
    /** The block a failed VR start logged, until a start succeeds. */
    public static final String START_FAILURE_PREF = "xr_start_failure";

    /**
     * The first line of every report, with the version on the line after, so
     * a mail can be told apart from other apps' reports. The collector refuses
     * a summary that does not start with it, so tools/report-worker has the
     * same text.
     */
    static final String HEADER = "Moonlight XR bug report\n";

    // What the collector takes: the note, the part before the logs, the logs
    // and the whole body, the logs in bytes of UTF-8 and the rest in chars
    static final int NOTE_MAX = 4000;
    static final int SUMMARY_MAX = 8000;
    static final int LOG_MAX_BYTES = 6 * 1024 * 1024;
    static final int BODY_MAX_BYTES = 8 * 1024 * 1024;

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
        return !collectorUrl().isEmpty();
    }

    /** Where this build sends reports, or empty where it has nowhere. */
    public static String collectorUrl() {
        return usableUrl(BuildConfig.REPORT_URL);
    }

    /**
     * A collector address as it can be used, or empty: a report carries the
     * log, so it only ever travels over https, and any other address counts
     * as no collector at all. The build refuses one too.
     */
    static String usableUrl(String url) {
        String trimmed = url == null ? "" : url.trim();
        return trimmed.toLowerCase(Locale.ROOT).startsWith("https://") ? trimmed : "";
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
     * The part of the report before the logs: the first line and the version,
     * the note, who it is from, why VR last failed to start if it did, then
     * the app and the headset and the settings. This is the summary the
     * collector is sent, and the top of a saved report.
     */
    public static String compose(String message, String email, Details d) {
        String note = message == null ? "" : message.trim();
        StringBuilder text = new StringBuilder();
        text.append(HEADER);
        text.append("Version: ").append(d.version).append('\n');
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
     * The headers the collector reads: the token it checks, as a bearer token,
     * and that the body is JSON. The token goes here and nowhere in the body.
     */
    public static Map<String, String> headers(String token) {
        Map<String, String> headers = new LinkedHashMap<>();
        headers.put("Content-Type", "application/json");
        if (token != null && !token.isEmpty()) {
            headers.put("Authorization", "Bearer " + token);
        }
        return headers;
    }

    /** The name a report made at that time goes by, saved or sent. */
    static String reportName(Date when) {
        return NAME_PREFIX + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(when)
                + NAME_SUFFIX;
    }

    /**
     * Writes the report into dir under that name: the text, then each log
     * that exists under a line naming it, and lets go of all but the newest
     * KEEP_REPORTS there. Returns the file.
     */
    public static File write(File dir, String name, String header, File... logs)
            throws IOException {
        if (dir == null || (!dir.isDirectory() && !dir.mkdirs())) {
            throw new IOException("no writable storage for the report");
        }
        File report = new File(dir, name);
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

    // The line a log starts under, in a saved report and in what is sent
    private static String logTitle(File log) {
        return "\n----- " + log.getName() + " -----\n";
    }

    private static void appendLog(Writer out, File log) throws IOException {
        if (log == null || !log.isFile()) {
            return;
        }
        out.write(logTitle(log));
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
     * The logs as they are sent, oldest first, each under the line naming it,
     * in at most max bytes of UTF-8. Two full logs come to 10 MB, more than
     * the collector takes, so past max the oldest part is left out: whole
     * files first, then the start of the one the cut falls in, from the line
     * after the cut so no line arrives half there. A first line then says how
     * much was left out. Each log is read only as far as it had come when this
     * started, as the current one is still being written.
     */
    static String logText(File[] logs, int max) throws IOException {
        List<File> present = new ArrayList<>();
        List<Long> sizes = new ArrayList<>();
        long total = 0;
        for (File log : logs) {
            if (log != null && log.isFile()) {
                present.add(log);
                sizes.add(log.length());
                total += utf8(logTitle(log)).length + log.length();
            }
        }

        // Room kept for the line saying what was cut
        final int cutLineRoom = 160;
        long drop = total <= max ? 0 : total - (max - cutLineRoom);
        long leftOut = 0;
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < present.size(); i++) {
            File log = present.get(i);
            long size = sizes.get(i);
            String title = logTitle(log);
            long titleBytes = utf8(title).length;
            if (drop >= titleBytes + size) {
                // All of this one goes
                drop -= titleBytes + size;
                leftOut += size;
                continue;
            }
            long from = Math.max(0, drop - titleBytes);
            drop = 0;
            byte[] bytes = readRange(log, from, size);
            int start = 0;
            if (from > 0) {
                start = lineStart(bytes);
                leftOut += from + start;
            }
            text.append(title).append(new String(bytes, start, bytes.length - start,
                    StandardCharsets.UTF_8));
        }
        if (leftOut > 0) {
            String limit = max % (1024 * 1024) == 0 ? (max / (1024 * 1024)) + " MB" : max + " bytes";
            text.insert(0, "----- cut to the newest " + limit + " of the logs, the oldest "
                    + leftOut + " bytes left out -----\n");
        }
        return text.toString();
    }

    // Where the first whole line in a slice starts: after its first newline,
    // or failing one, at the first byte that starts a character
    private static int lineStart(byte[] bytes) {
        for (int i = 0; i < bytes.length; i++) {
            if (bytes[i] == '\n') {
                return i + 1;
            }
        }
        int i = 0;
        while (i < bytes.length && (bytes[i] & 0xC0) == 0x80) {
            i++;
        }
        return i;
    }

    private static byte[] readRange(File file, long from, long to) throws IOException {
        RandomAccessFile in = new RandomAccessFile(file, "r");
        try {
            long end = Math.min(to, in.length());
            int length = (int) Math.max(0, end - from);
            byte[] bytes = new byte[length];
            in.seek(from);
            in.readFully(bytes);
            return bytes;
        } finally {
            in.close();
        }
    }

    private static byte[] utf8(String text) {
        return text.getBytes(StandardCharsets.UTF_8);
    }

    /**
     * Whether the collector's answer is one of its limits rather than a
     * fault: too many reports for now (429) or a report bigger than it takes
     * (413). Either way the saved copy is the one to send later.
     */
    static boolean busy(int code) {
        return code == 429 || code == 413;
    }

    /** At most max chars of text, never splitting a character in two. */
    static String clip(String text, int max) {
        if (text.length() <= max) {
            return text;
        }
        int end = Character.isHighSurrogate(text.charAt(max - 1)) ? max - 1 : max;
        return text.substring(0, end);
    }

    /**
     * The body the collector takes: the note, the address to reply to or
     * null, the summary, the logs and the name the report goes by, as one
     * JSON object in UTF-8. Should the logs' escaping still carry it past
     * BODY_MAX_BYTES, more of their oldest part is left out until it fits.
     */
    static byte[] body(String note, String replyTo, String summary, String log, String fileName) {
        String reply = replyTo == null || replyTo.trim().isEmpty() ? null : replyTo.trim();
        String kept = log;
        for (;;) {
            StringBuilder json = new StringBuilder(kept.length() + 16384);
            json.append("{\"note\":");
            quote(json, clip(note == null ? "" : note.trim(), NOTE_MAX));
            json.append(",\"replyTo\":");
            if (reply == null) {
                json.append("null");
            }
            else {
                quote(json, reply);
            }
            json.append(",\"summary\":");
            quote(json, clip(summary, SUMMARY_MAX));
            json.append(",\"log\":");
            quote(json, kept);
            json.append(",\"fileName\":");
            quote(json, fileName);
            json.append('}');
            byte[] bytes = utf8(json.toString());
            if (bytes.length <= BODY_MAX_BYTES || kept.isEmpty()) {
                return bytes;
            }
            // Each char left out takes a byte or more with it
            int cut = Math.min(kept.length(), bytes.length - BODY_MAX_BYTES + 65536);
            int next = kept.indexOf('\n', cut);
            kept = "----- cut further to fit the collector -----\n"
                    + (next < 0 ? "" : kept.substring(next + 1));
        }
    }

    // A JSON string, escaping only what JSON asks for, plus the two line
    // separators JavaScript would choke on
    private static void quote(StringBuilder out, String text) {
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (c < 0x20 || c == '\u2028' || c == '\u2029') {
                        out.append(String.format(Locale.ROOT, "\\u%04x", (int) c));
                    }
                    else {
                        out.append(c);
                    }
                    break;
            }
        }
        out.append('"');
    }

    /**
     * Everything after the summary is put together: posted to the collector
     * where there is one, and saved into dir only where that fails or there
     * is none. Apart from Android, so a test can run it.
     */
    static Outcome sendOrSave(File dir, String name, String summary, String note, String email,
                              File[] logs, String url, String token, Transport transport) {
        String target = usableUrl(url);
        if (target.isEmpty()) {
            return save(Result.NO_COLLECTOR, null, dir, name, summary, logs);
        }
        Result failed;
        String detail;
        try {
            byte[] body = body(note, email, summary, logText(logs, LOG_MAX_BYTES), name);
            int code = transport.post(target, headers(token), body);
            if (code / 100 == 2) {
                return new Outcome(Result.SENT, null, null);
            }
            failed = busy(code) ? Result.BUSY : Result.NOT_SENT;
            detail = "server answered " + code;
        } catch (IOException e) {
            failed = Result.NOT_SENT;
            detail = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
        }
        return save(failed, detail, dir, name, summary, logs);
    }

    // The fallback: the whole report saved beside the log, logs uncut
    private static Outcome save(Result result, String detail, File dir, String name,
                                String summary, File[] logs) {
        try {
            File report = write(dir, name, summary, logs);
            return new Outcome(result, report.getAbsolutePath(), detail);
        } catch (IOException e) {
            String why = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
            return new Outcome(Result.NOT_WRITTEN, null, detail != null ? detail + "; " + why : why);
        }
    }

    /**
     * The one folder reports are saved in: beside the log, which is where the
     * headset's file manager finds them, or the app's own folder with the log
     * switched off.
     */
    public static File reportDir(Context context) {
        File besideLog = logDir();
        return besideLog != null ? besideLog : context.getExternalFilesDir("reports");
    }

    /**
     * The whole of a report, from the settings or inside a session: sent to
     * the collector, or saved beside the log where it cannot be. Blocks for
     * as long as the post takes, so never on the main thread or the frame
     * loop. session says what the session has up, for a line of its own, and
     * is null outside one.
     */
    public static Outcome send(Context context, String message, String email, String session) {
        // Whatever the log's writer thread still holds goes to disk first, so
        // the report carries the lines written seconds ago
        FileLog.flush();
        String summary = compose(message, email, gather(context, session));
        return sendOrSave(reportDir(context), reportName(new Date()), summary, message, email,
                logFiles(), collectorUrl(), BuildConfig.REPORT_TOKEN, HTTP);
    }

    /** A path as the headset's file manager shows it, from Download on, or as it is. */
    public static String shortPath(String path) {
        if (path == null) {
            return null;
        }
        return path.startsWith(STORAGE_ROOT) ? path.substring(STORAGE_ROOT.length()) : path;
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
                + "fileLog " + prefs.fileLogLevel + " checkUpdates " + prefs.checkUpdates + "\n";
    }

    /** Posts over HTTP with the timeouts a few megabytes over a headset's wifi wants. */
    public static final Transport HTTP = new Transport() {
        @Override
        public int post(String url, Map<String, String> headers, byte[] body) throws IOException {
            HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
            try {
                conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
                conn.setReadTimeout(READ_TIMEOUT_MS);
                conn.setRequestMethod("POST");
                conn.setDoOutput(true);
                conn.setFixedLengthStreamingMode(body.length);
                for (Map.Entry<String, String> header : headers.entrySet()) {
                    conn.setRequestProperty(header.getKey(), header.getValue());
                }
                OutputStream out = conn.getOutputStream();
                try {
                    out.write(body);
                } finally {
                    out.close();
                }
                return conn.getResponseCode();
            } finally {
                conn.disconnect();
            }
        }
    };
}
