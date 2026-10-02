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

import com.limelight.utils.BugReport;
import com.limelight.utils.UiHelper;

import java.io.File;
import java.io.IOException;

/**
 * Puts together everything a bug report needs and sends it, so a user does
 * not have to find the log file, work out what device they have, or remember
 * which settings they were on. What goes in the report and how it is posted
 * is BugReport, which the sheet inside the session uses too.
 *
 * Headsets rarely have an email app, so the report is always saved next to
 * the log first and the email is a second step that may not be possible. In
 * that case the user is told where the file is and where to send it.
 */
public class BugReportActivity extends Activity {
    public static final String REPORT_ADDRESS = "hello@seangilleece.com";

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
                .getString(BugReport.EMAIL_PREF, ""));

        TextView intro = findViewById(R.id.reportIntro);
        String logPath = FileLog.getLogPath();
        intro.setText(getString(R.string.bug_report_intro, REPORT_ADDRESS,
                logPath != null ? logPath : getString(R.string.bug_report_log_off)));

        // With a collector to send to the button says so, since that path
        // needs no email app and works from a headset
        Button send = findViewById(R.id.reportSend);
        if (BugReport.collectorConfigured()) {
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
                .putString(BugReport.EMAIL_PREF, email).apply();

        File report;
        try {
            report = BugReport.save(this, message, email);
        } catch (IOException e) {
            Toast.makeText(this, getString(R.string.bug_report_failed, e.getMessage()),
                    Toast.LENGTH_LONG).show();
            return;
        }

        // Saved beside the log, where the headset's file manager can see it,
        // for the case where it cannot leave the device and travels by hand
        String where = report.getAbsolutePath();

        // A build that knows where reports go sends them straight there, which
        // is the only way off a headset with no email app
        if (byEmail && BugReport.collectorConfigured()) {
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
                final String failure = BugReport.post(report, BuildConfig.REPORT_URL,
                        BugReport.headersFor(email, message), BugReport.HTTP);
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

    // Whether anything on this device can take a mail. Most headsets have
    // nothing, and a chooser with no entries is worse than saying so.
    private boolean haveMailApp() {
        Intent probe = new Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:" + REPORT_ADDRESS));
        return !getPackageManager().queryIntentActivities(probe, PackageManager.MATCH_DEFAULT_ONLY)
                .isEmpty();
    }
}
