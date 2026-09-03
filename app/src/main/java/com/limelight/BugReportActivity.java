package com.limelight;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.utils.UiHelper;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.zip.GZIPOutputStream;

/**
 * Puts together everything a bug report needs and sends it, so a user does
 * not have to find the log file, work out what device they have, or remember
 * which settings they were on. The report is one text file: what the user
 * typed, the app and device, the settings that matter, and both log files.
 *
 * Headsets rarely have an email app, so the report is always saved next to
 * the log first and the email is a second step that may not be possible. In
 * that case the user is told where the file is and where to send it.
 */
public class BugReportActivity extends Activity {
    public static final String REPORT_ADDRESS = "hello@seangilleece.com";

    // Remembered between reports, since it is the one field that never changes
    private static final String EMAIL_PREF = "bug_report_email";

    private EditText emailView;
    private EditText messageView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        UiHelper.setLocale(this);
        setContentView(R.layout.activity_bug_report);
        UiHelper.notifyNewRootView(this);

        emailView = findViewById(R.id.reportEmail);
        messageView = findViewById(R.id.reportMessage);
        emailView.setText(PreferenceManager.getDefaultSharedPreferences(this)
                .getString(EMAIL_PREF, ""));

        TextView intro = findViewById(R.id.reportIntro);
        String logPath = FileLog.getLogPath();
        intro.setText(getString(R.string.bug_report_intro, REPORT_ADDRESS,
                logPath != null ? logPath : getString(R.string.bug_report_log_off)));

        // With a collector to send to the button says so, since that path
        // needs no email app and works from a headset
        Button send = findViewById(R.id.reportSend);
        if (!BuildConfig.REPORT_URL.isEmpty()) {
            send.setText(R.string.bug_report_send_direct);
        }

        findViewById(R.id.reportBack).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
        send.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendReport(true);
            }
        });
        Button save = findViewById(R.id.reportSave);
        save.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendReport(false);
            }
        });
    }

    private void sendReport(boolean byEmail) {
        String email = emailView.getText().toString().trim();
        String message = messageView.getText().toString().trim();
        PreferenceManager.getDefaultSharedPreferences(this).edit()
                .putString(EMAIL_PREF, email).apply();

        File report;
        try {
            report = writeReport(email, message);
        } catch (IOException e) {
            Toast.makeText(this, getString(R.string.bug_report_failed, e.getMessage()),
                    Toast.LENGTH_LONG).show();
            return;
        }

        // A copy where the headset's file manager can see it, for the case
        // where the report cannot leave the device and has to travel by hand
        File visible = copyBesideLog(report);
        String where = visible != null ? visible.getAbsolutePath() : report.getAbsolutePath();

        // A build that knows where reports go sends them straight there, which
        // is the only way off a headset with no email app
        if (byEmail && !BuildConfig.REPORT_URL.isEmpty()) {
            upload(report, email, message, where);
            return;
        }

        if (!byEmail || !haveMailApp()) {
            String text = byEmail
                    ? getString(R.string.bug_report_no_mail, where, REPORT_ADDRESS)
                    : getString(R.string.bug_report_saved, where);
            new AlertDialog.Builder(this)
                    .setTitle(R.string.title_bug_report)
                    .setMessage(text)
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
            return;
        }

        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_EMAIL, new String[] { REPORT_ADDRESS });
        send.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.bug_report_subject,
                Build.MANUFACTURER + " " + Build.MODEL, BuildConfig.VERSION_NAME));
        send.putExtra(Intent.EXTRA_TEXT, message + "\n\n" + getString(R.string.bug_report_from, email));
        send.putExtra(Intent.EXTRA_STREAM, ReportContentProvider.uriFor(report));
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivity(Intent.createChooser(send, getString(R.string.title_bug_report)));
        } catch (ActivityNotFoundException e) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.title_bug_report)
                    .setMessage(getString(R.string.bug_report_no_mail, where, REPORT_ADDRESS))
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
        }
    }

    // Posts the report to the collector off the main thread and says how it
    // went. A failure leaves the saved copy where it is and says where.
    private void upload(final File report, final String email, final String message,
                        final String where) {
        final Button send = findViewById(R.id.reportSend);
        send.setEnabled(false);
        Toast.makeText(this, R.string.bug_report_sending, Toast.LENGTH_SHORT).show();

        new Thread() {
            @Override
            public void run() {
                final String failure = postReport(report, email, message);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        send.setEnabled(true);
                        if (isFinishing()) {
                            return;
                        }
                        if (failure == null) {
                            Toast.makeText(BugReportActivity.this, R.string.bug_report_sent,
                                    Toast.LENGTH_LONG).show();
                            finish();
                            return;
                        }
                        new AlertDialog.Builder(BugReportActivity.this)
                                .setTitle(R.string.title_bug_report)
                                .setMessage(getString(R.string.bug_report_upload_failed, failure,
                                        where, REPORT_ADDRESS))
                                .setPositiveButton(android.R.string.ok, null)
                                .show();
                    }
                });
            }
        }.start();
    }

    // Null when the collector took the report, otherwise what went wrong. The
    // report goes up gzipped: a log is mostly repetition and shrinks about ten
    // to one, which is kinder to a headset's uplink and keeps the attachment
    // the collector mails on well inside what it can handle.
    private static String postReport(File report, String email, String message) {
        File packed = new File(report.getParentFile(), report.getName() + ".gz");
        HttpURLConnection conn = null;
        try {
            gzip(report, packed);

            conn = (HttpURLConnection) new URL(BuildConfig.REPORT_URL).openConnection();
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(30000);
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setFixedLengthStreamingMode(packed.length());
            conn.setRequestProperty("Content-Type", "application/gzip");
            if (!BuildConfig.REPORT_TOKEN.isEmpty()) {
                conn.setRequestProperty("X-Report-Token", BuildConfig.REPORT_TOKEN);
            }
            conn.setRequestProperty("X-Report-Device", headerSafe(Build.MANUFACTURER + " " + Build.MODEL));
            conn.setRequestProperty("X-Report-Version", headerSafe(BuildConfig.VERSION_NAME));
            conn.setRequestProperty("X-Report-Email", headerSafe(email));
            // The message again in clear, so the mail can carry it without
            // anyone unpacking the attachment to see what the report is about
            conn.setRequestProperty("X-Report-Summary", headerSafe(message, 2000));

            FileInputStream in = new FileInputStream(packed);
            try {
                byte[] chunk = new byte[16384];
                int read;
                while ((read = in.read(chunk)) > 0) {
                    conn.getOutputStream().write(chunk, 0, read);
                }
            } finally {
                in.close();
            }

            int code = conn.getResponseCode();
            if (code / 100 != 2) {
                return "server answered " + code;
            }
            return null;
        } catch (IOException e) {
            return e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
            packed.delete();
        }
    }

    private static void gzip(File from, File to) throws IOException {
        FileInputStream in = new FileInputStream(from);
        GZIPOutputStream out = new GZIPOutputStream(new FileOutputStream(to));
        try {
            byte[] chunk = new byte[16384];
            int read;
            while ((read = in.read(chunk)) > 0) {
                out.write(chunk, 0, read);
            }
        } finally {
            in.close();
            out.close();
        }
    }

    // Headers carry printable ASCII on one line and nothing else
    private static String headerSafe(String value) {
        return headerSafe(value, 120);
    }

    private static String headerSafe(String value, int max) {
        String ascii = value.replaceAll("[\\r\\n]+", " / ").replaceAll("[^\\x20-\\x7E]", "?");
        return ascii.length() > max ? ascii.substring(0, max) : ascii;
    }

    // Whether anything on this device can take a mail. Most headsets have
    // nothing, and a chooser with no entries is worse than saying so.
    private boolean haveMailApp() {
        Intent probe = new Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:" + REPORT_ADDRESS));
        return !getPackageManager().queryIntentActivities(probe, PackageManager.MATCH_DEFAULT_ONLY)
                .isEmpty();
    }

    private File writeReport(String email, String message) throws IOException {
        File dir = ReportContentProvider.reportsDir(this);
        if (dir == null || (!dir.isDirectory() && !dir.mkdirs())) {
            throw new IOException("no writable storage for the report");
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File report = new File(dir, "moonlight-xr-report-" + stamp + ".txt");

        // Whatever the writer thread still holds goes to disk first, so the
        // report carries the lines written seconds ago
        FileLog.flush();

        Writer out = new OutputStreamWriter(new FileOutputStream(report), StandardCharsets.UTF_8);
        try {
            out.write("Moonlight XR bug report\n");
            out.write("From: " + email + "\n\n");
            out.write(message.isEmpty() ? "(no message)\n" : message + "\n");
            out.write("\n----- app and device -----\n");
            out.write(describeDevice());
            out.write("\n----- settings -----\n");
            out.write(describeSettings());
            appendLog(out, FileLog.getPreviousLogFile());
            appendLog(out, FileLog.getLogPath() != null ? new File(FileLog.getLogPath()) : null);
        } finally {
            out.close();
        }
        return report;
    }

    private String describeDevice() {
        return "moonlight " + BuildConfig.VERSION_NAME + " " + getPackageName()
                + (BuildConfig.ROOT_BUILD ? " root" : "") + (BuildConfig.DEBUG ? " debug" : "") + "\n"
                + "device " + Build.MANUFACTURER + " " + Build.BRAND + " " + Build.MODEL
                + " (" + Build.DEVICE + ")\n"
                + "android " + Build.VERSION.RELEASE + " sdk " + Build.VERSION.SDK_INT
                + " build " + Build.DISPLAY + "\n"
                + "headset " + PreferenceConfiguration.isHeadset(this) + "\n";
    }

    // The settings a picture problem usually turns on, straight from the same
    // reader the stream uses so the report says what the stream would see
    private String describeSettings() {
        PreferenceConfiguration prefs = PreferenceConfiguration.readPreferences(this);
        return "res " + prefs.width + "x" + prefs.height + " fps " + prefs.fps
                + " bitrate " + prefs.bitrate + " format " + prefs.videoFormat
                + " pacing " + prefs.framePacing + "\n"
                + "vr " + prefs.enableVrMode + " depthMode " + prefs.vrDepthMode
                + " cadence " + prefs.vrInferenceCadence + " separation " + prefs.vrStereoSeparation
                + " convergence " + prefs.vrConvergence + " envRes " + prefs.vrEnvResTier
                + " sharpening " + prefs.vrSharpening + " passthrough " + prefs.vrPassthrough
                + " hands " + prefs.vrHandTracking + " gaze " + prefs.vrGaze
                + " headLocked " + prefs.vrHeadLocked + " ambilight " + prefs.vrAmbilight + "\n"
                + "fileLog " + prefs.fileLogLevel + "\n";
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

    // Best effort: the log's own folder is the one the headset's file manager
    // shows, and if it is not there the report is still in the app's own
    private File copyBesideLog(File report) {
        String logPath = FileLog.getLogPath();
        if (logPath == null) {
            return null;
        }
        File target = new File(new File(logPath).getParentFile(), report.getName());
        try {
            FileInputStream in = new FileInputStream(report);
            FileOutputStream out = new FileOutputStream(target);
            try {
                byte[] chunk = new byte[16384];
                int read;
                while ((read = in.read(chunk)) > 0) {
                    out.write(chunk, 0, read);
                }
            } finally {
                in.close();
                out.close();
            }
            return target;
        } catch (IOException e) {
            return null;
        }
    }
}
