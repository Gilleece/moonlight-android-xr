package com.limelight.utils;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.preference.PreferenceManager;
import android.util.TypedValue;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.FrameLayout;

import com.limelight.R;

/**
 * A warning popup that the user can silence for good. It comes back on every
 * launch until the checkbox is ticked, so a warning that matters is not lost
 * behind a single dismissal.
 */
public class WarningDialog {
    private static final String DISMISSED_PREFIX = "warning_dismissed_";

    public static boolean isDismissed(Context context, String id) {
        return PreferenceManager.getDefaultSharedPreferences(context)
                .getBoolean(DISMISSED_PREFIX + id, false);
    }

    public static void showIfNeeded(final Activity activity, final String id, final String title, final String message) {
        if (isDismissed(activity, id) || activity.isFinishing()) {
            return;
        }

        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                // The activity can go away between the check and the post
                if (activity.isFinishing()) {
                    return;
                }

                final CheckBox neverAgain = new CheckBox(activity);
                neverAgain.setText(R.string.warning_dialog_never_again);

                // The builder gives the message its own scrolling view, so the
                // checkbox goes in as the custom view underneath it
                FrameLayout layout = new FrameLayout(activity);
                layout.addView(neverAgain, new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
                layout.setPadding(dpToPx(activity, 24), dpToPx(activity, 8),
                        dpToPx(activity, 24), dpToPx(activity, 8));

                new AlertDialog.Builder(activity)
                        .setTitle(title)
                        .setMessage(message)
                        .setView(layout)
                        .setCancelable(true)
                        .setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                if (neverAgain.isChecked()) {
                                    PreferenceManager.getDefaultSharedPreferences(activity)
                                            .edit()
                                            .putBoolean(DISMISSED_PREFIX + id, true)
                                            .apply();
                                }
                                dialog.dismiss();
                            }
                        })
                        .show();
            }
        });
    }

    private static int dpToPx(Context context, int dp) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp,
                context.getResources().getDisplayMetrics());
    }
}
