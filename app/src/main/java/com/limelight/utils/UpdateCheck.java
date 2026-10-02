package com.limelight.utils;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

import com.limelight.BuildConfig;
import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * "Check for updates": at most once a day, as the PC list opens, one
 * anonymous request to GitHub for this fork's latest release. It carries
 * nothing but the app's version, and nothing is ever downloaded or installed.
 * A newer release puts a notice on the PC list, whose View opens the release
 * page in the browser and whose Dismiss keeps it away until a newer one.
 *
 * Releases are tagged v0.3, v0.4 and so on, and the app's versionName ends in
 * the same number, as in 12.1-xr0.3. Anything that does not read that way is
 * never taken for a newer release.
 */
public final class UpdateCheck {

    static final String LATEST_RELEASE_URL =
            "https://api.github.com/repos/Gilleece/moonlight-android-xr/releases/latest";
    static final String RELEASES_PAGE = "https://github.com/Gilleece/moonlight-android-xr/releases";

    // The check's own state, not settings
    static final String LAST_CHECK_PREF = "update_last_check";
    static final String LATEST_TAG_PREF = "update_latest_tag";
    static final String LATEST_URL_PREF = "update_latest_url";
    static final String DISMISSED_TAG_PREF = "update_dismissed_tag";

    static final long INTERVAL_MS = 24L * 60 * 60 * 1000;
    static final int TIMEOUT_MS = 5000;
    // A release's answer is a few kilobytes with its notes
    private static final int MAX_ANSWER_BYTES = 1024 * 1024;

    // v0.4 or 0.4, any number of parts, nothing after. Six digits a part
    // keeps every part inside an int.
    private static final Pattern TAG = Pattern.compile("[vV]?(\\d{1,6}(?:\\.\\d{1,6})*)");
    private static final Pattern APP =
            Pattern.compile("(?:^|[^A-Za-z])xr(\\d{1,6}(?:\\.\\d{1,6})*)$");

    /** What the notice says and where its View goes. */
    public static final class Notice {
        public final String tag;
        public final String url;

        Notice(String tag, String url) {
            this.tag = tag;
            this.url = url;
        }
    }

    // A refused request, kept apart so the log can say its status
    static final class StatusException extends IOException {
        final int status;

        StatusException(int status) {
            super("HTTP " + status);
            this.status = status;
        }
    }

    private UpdateCheck() {
    }

    /** A release tag's numbers, from v0.4 or 0.4, or null for anything else. */
    static int[] parseTag(String tag) {
        if (tag == null) {
            return null;
        }
        Matcher m = TAG.matcher(tag.trim());
        return m.matches() ? numbers(m.group(1)) : null;
    }

    /** The numbers of the xrN.N that ends a versionName like 12.1-xr0.3, or null. */
    static int[] appVersion(String versionName) {
        if (versionName == null) {
            return null;
        }
        Matcher m = APP.matcher(versionName.trim());
        return m.find() ? numbers(m.group(1)) : null;
    }

    private static int[] numbers(String dotted) {
        String[] parts = dotted.split("\\.");
        int[] out = new int[parts.length];
        for (int i = 0; i < parts.length; i++) {
            out[i] = Integer.parseInt(parts[i]);
        }
        return out;
    }

    /** Part by part, a missing part counting as 0, so 0.4 and 0.4.0 are the same. */
    static int compare(int[] a, int[] b) {
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? a[i] : 0;
            int y = i < b.length ? b[i] : 0;
            if (x != y) {
                return x < y ? -1 : 1;
            }
        }
        return 0;
    }

    /** Whether a release tag is newer than this build. Never when either does not read. */
    public static boolean isNewer(String tag, String versionName) {
        int[] release = parseTag(tag);
        int[] app = appVersion(versionName);
        return release != null && app != null && compare(release, app) > 0;
    }

    /**
     * Whether the notice shows for the newest release known: newer than this
     * build, and newer than any release whose notice was dismissed.
     */
    static boolean shouldShow(String latestTag, String versionName, String dismissedTag) {
        if (!isNewer(latestTag, versionName)) {
            return false;
        }
        int[] dismissed = parseTag(dismissedTag);
        return dismissed == null || compare(parseTag(latestTag), dismissed) > 0;
    }

    /**
     * Whether a day has gone since the last check. A clock set back before it
     * counts as due too, or the check would stop until the clock caught up.
     */
    static boolean isDue(long now, long lastCheck) {
        return lastCheck <= 0 || now < lastCheck || now - lastCheck >= INTERVAL_MS;
    }

    /** The page View opens: the release's own, if it is this repository's, else the list. */
    static String viewUrl(String htmlUrl) {
        return htmlUrl != null && htmlUrl.startsWith(RELEASES_PAGE + "/") ? htmlUrl : RELEASES_PAGE;
    }

    /** GitHub refuses a request with no User-Agent, so it gets the app and version only. */
    static String userAgent(String versionName) {
        return "Moonlight XR " + versionName;
    }

    /** A failed check as the log gets it: the HTTP status, or the kind of failure. */
    static String describeFailure(Exception e) {
        if (e instanceof StatusException) {
            return "HTTP " + ((StatusException) e).status;
        }
        return e.getClass().getSimpleName();
    }

    /** The notice to show, or null when the setting is off or nothing newer is known. */
    public static Notice notice(SharedPreferences prefs, String versionName) {
        if (!PreferenceConfiguration.checkUpdatesOn(prefs)) {
            return null;
        }
        String tag = prefs.getString(LATEST_TAG_PREF, null);
        if (!shouldShow(tag, versionName, prefs.getString(DISMISSED_TAG_PREF, null))) {
            return null;
        }
        return new Notice(tag.trim(), viewUrl(prefs.getString(LATEST_URL_PREF, null)));
    }

    /** Keeps the notice for this release away. A newer one brings it back. */
    public static void dismiss(SharedPreferences prefs, String tag) {
        prefs.edit().putString(DISMISSED_TAG_PREF, tag).apply();
    }

    /**
     * Whether a check goes now. One that does is recorded at once, so a failed
     * check waits a day as well and a second open cannot start another.
     */
    static boolean claim(SharedPreferences prefs, long now) {
        if (!PreferenceConfiguration.checkUpdatesOn(prefs)
                || !isDue(now, prefs.getLong(LAST_CHECK_PREF, 0))) {
            return false;
        }
        prefs.edit().putLong(LAST_CHECK_PREF, now).apply();
        return true;
    }

    /**
     * Keeps what a check found and says what came of it, for the log. A
     * prerelease, a draft or a tag that does not read leaves what was known.
     */
    static String record(SharedPreferences prefs, String versionName, String tag,
                         String htmlUrl, boolean prerelease, boolean draft) {
        if (prerelease || draft) {
            return "the latest is a " + (draft ? "draft" : "prerelease") + ", ignored";
        }
        if (parseTag(tag) == null) {
            return "the latest has a tag that does not read as a version, ignored";
        }
        String latest = tag.trim();
        prefs.edit()
                .putString(LATEST_TAG_PREF, latest)
                .putString(LATEST_URL_PREF, viewUrl(htmlUrl))
                .apply();
        String outcome;
        if (!isNewer(latest, versionName)) {
            outcome = "no notice";
        }
        else if (shouldShow(latest, versionName, prefs.getString(DISMISSED_TAG_PREF, null))) {
            outcome = "notice shown";
        }
        else {
            outcome = "dismissed earlier, no notice";
        }
        return "latest " + latest + ", this build " + versionName + ", " + outcome;
    }

    /**
     * Starts a check on its own thread when the setting is on and a day has
     * gone since the last. A check that gets an answer stores it and then runs
     * onDone on that thread. A failure is one log line and nothing else.
     */
    public static void checkIfDue(Context context, final Runnable onDone) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        if (!PreferenceConfiguration.checkUpdatesOn(prefs)) {
            return;
        }
        long now = System.currentTimeMillis();
        long last = prefs.getLong(LAST_CHECK_PREF, 0);
        if (!claim(prefs, now)) {
            LimeLog.info("Update check not due, the last was "
                    + ((now - last) / 60000) + " min ago");
            return;
        }

        Thread thread = new Thread() {
            @Override
            public void run() {
                String versionName = BuildConfig.VERSION_NAME;
                try {
                    JSONObject latest = new JSONObject(get(versionName));
                    FileLog.event("Update check: " + record(prefs, versionName,
                            latest.optString("tag_name", null),
                            latest.optString("html_url", null),
                            latest.optBoolean("prerelease", false),
                            latest.optBoolean("draft", false)));
                } catch (Exception e) {
                    FileLog.event("Update check failed: " + describeFailure(e));
                    return;
                }
                onDone.run();
            }
        };
        thread.setName("Update check");
        thread.start();
    }

    private static String get(String versionName) throws IOException {
        HttpURLConnection conn = (HttpURLConnection) new URL(LATEST_RELEASE_URL).openConnection();
        try {
            conn.setConnectTimeout(TIMEOUT_MS);
            conn.setReadTimeout(TIMEOUT_MS);
            conn.setUseCaches(false);
            conn.setRequestProperty("Accept", "application/vnd.github+json");
            // Set by hand, since the default names the device and its build
            conn.setRequestProperty("User-Agent", userAgent(versionName));
            int status = conn.getResponseCode();
            if (status != HttpURLConnection.HTTP_OK) {
                throw new StatusException(status);
            }
            InputStream in = conn.getInputStream();
            try {
                ByteArrayOutputStream body = new ByteArrayOutputStream();
                byte[] chunk = new byte[8192];
                int read;
                while ((read = in.read(chunk)) > 0) {
                    body.write(chunk, 0, read);
                    if (body.size() > MAX_ANSWER_BYTES) {
                        throw new IOException("answer too large");
                    }
                }
                return new String(body.toByteArray(), StandardCharsets.UTF_8);
            } finally {
                in.close();
            }
        } finally {
            conn.disconnect();
        }
    }
}
