package com.limelight;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

import com.limelight.utils.BugReport;
import com.limelight.utils.UiHelper;

/**
 * Puts together everything a bug report needs and sends it, so a user does
 * not have to find the log file, work out what device they have, or remember
 * which settings they were on. What goes in the report and how it is posted
 * is BugReport, which the sheet inside the session uses too.
 *
 * There is one action, Send. The report goes to the collector the build was
 * made with; only where that fails, or the build has none, is it saved beside
 * the log, and the user is told why and where.
 */
public class BugReportActivity extends Activity {
    public static final String REPORT_ADDRESS = "hello@seangilleece.com";

    private EditText emailView;
    private EditText messageView;
    private Button sendButton;
    private Toast sendingToast;

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

        findViewById(R.id.reportBack).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
        sendButton = findViewById(R.id.reportSend);
        sendButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendReport();
            }
        });
    }

    // The same checks the sheet in the session makes: a note, and an address
    // that could be answered or none
    private void sendReport() {
        final String email = emailView.getText().toString().trim();
        final String message = messageView.getText().toString().trim();
        if (!BugReport.hasNote(message)) {
            messageView.setError(getString(R.string.bug_report_message_hint));
            messageView.requestFocus();
            return;
        }
        if (!BugReport.addressOk(email)) {
            emailView.setError(getString(R.string.vr_report_email_bad));
            emailView.requestFocus();
            return;
        }
        PreferenceManager.getDefaultSharedPreferences(this).edit()
                .putString(BugReport.EMAIL_PREF, email).apply();

        sendButton.setEnabled(false);
        if (BugReport.collectorConfigured()) {
            sendingToast = Toast.makeText(this, R.string.bug_report_sending, Toast.LENGTH_SHORT);
            sendingToast.show();
        }

        // The post can take a while over a headset's wifi, so it never runs
        // on the main thread
        Thread send = new Thread() {
            @Override
            public void run() {
                final BugReport.Outcome outcome = BugReport.send(BugReportActivity.this, message,
                        email, null);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        sendButton.setEnabled(true);
                        if (!isFinishing()) {
                            showOutcome(outcome);
                        }
                    }
                });
            }
        };
        send.setName("Bug report send");
        send.start();
    }

    // Said in a dialog either way: a toast queued as the screen closes is
    // dropped on a headset, so a sent report closes the screen once read
    private void showOutcome(BugReport.Outcome outcome) {
        if (sendingToast != null) {
            sendingToast.cancel();
            sendingToast = null;
        }
        if (outcome.result == BugReport.Result.SENT) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.title_bug_report)
                    .setMessage(R.string.bug_report_sent)
                    .setPositiveButton(android.R.string.ok, null)
                    .setOnDismissListener(new DialogInterface.OnDismissListener() {
                        @Override
                        public void onDismiss(DialogInterface dialog) {
                            finish();
                        }
                    })
                    .show();
            return;
        }

        String text;
        switch (outcome.result) {
            case NO_COLLECTOR:
                text = getString(R.string.bug_report_no_collector_at) + "\n\n" + outcome.path;
                break;
            case BUSY:
                text = getString(R.string.bug_report_busy_at) + "\n\n" + outcome.path;
                break;
            case NOT_SENT:
                text = getString(R.string.bug_report_not_sent_at) + "\n\n" + outcome.path
                        + (outcome.detail != null ? "\n\n(" + outcome.detail + ")" : "");
                break;
            default:
                text = getString(R.string.bug_report_failed, outcome.detail);
                break;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.title_bug_report)
                .setMessage(text)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }
}
