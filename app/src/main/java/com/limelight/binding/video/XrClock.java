package com.limelight.binding.video;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.text.format.DateFormat;

import com.limelight.R;

import java.text.SimpleDateFormat;
import java.util.Calendar;
import java.util.Locale;

/**
 * The time and the battery, for the first line of the stats and the top of
 * the settings panel, so a long session has a clock somewhere. The battery,
 * and whether the clock is written twelve or twenty four hour, cost a call
 * into the system, so they are read once a minute off whichever thread asks;
 * the line itself is put together from what was last read.
 */
final class XrClock {
    private static final long READ_EVERY_MS = 60000;

    private final Context context;
    private final String batteryFormat;
    private int batteryPercent = -1;
    private boolean twentyFour = true;
    private long readMs;
    private boolean reading;

    XrClock(Context context) {
        this.context = context.getApplicationContext();
        this.batteryFormat = context.getString(R.string.vr_battery);
        // Once here, off the frame loop, so the first line already has both
        read(System.currentTimeMillis());
    }

    /**
     * The line as it stands now: the time, then the battery once it has been
     * read. Cheap on any thread; a minute after the last read it starts the
     * next one off on a thread of its own and carries on with the last.
     */
    synchronized String line(long nowMillis) {
        if (nowMillis - readMs >= READ_EVERY_MS && !reading) {
            reading = true;
            Thread reader = new Thread() {
                @Override
                public void run() {
                    read(System.currentTimeMillis());
                }
            };
            reader.setName("Video - XR Clock");
            reader.start();
        }
        Calendar now = Calendar.getInstance();
        now.setTimeInMillis(nowMillis);
        return line(timeText(now, twentyFour, Locale.getDefault()), batteryPercent, batteryFormat);
    }

    // The sticky battery intent and the clock setting, both calls into the
    // system, then the results under the lock
    private void read(long nowMillis) {
        int percent = -1;
        try {
            Intent battery = context.registerReceiver(null,
                    new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery != null) {
                percent = batteryPercent(battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1),
                        battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1));
            }
        } catch (RuntimeException ignored) {
            // No battery to read, which only costs the line its second half
        }
        boolean hours24 = DateFormat.is24HourFormat(context);
        synchronized (this) {
            batteryPercent = percent;
            twentyFour = hours24;
            readMs = nowMillis;
            reading = false;
        }
    }

    /** The time of day as the headset is set to write it, hours and minutes. */
    static String timeText(Calendar now, boolean twentyFour, Locale locale) {
        SimpleDateFormat format = new SimpleDateFormat(twentyFour ? "H:mm" : "h:mm a", locale);
        format.setTimeZone(now.getTimeZone());
        return format.format(now.getTime());
    }

    /** The time, then the battery when there is one to say, two spaces apart. */
    static String line(String time, int batteryPercent, String batteryFormat) {
        if (batteryPercent < 0) {
            return time;
        }
        return time + "  " + String.format(Locale.getDefault(), batteryFormat, batteryPercent);
    }

    /** A battery intent's level as a whole percent, or -1 where it does not say. */
    static int batteryPercent(int level, int scale) {
        if (level < 0 || scale <= 0) {
            return -1;
        }
        return Math.min(100, Math.round(level * 100.0f / scale));
    }
}
