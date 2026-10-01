package com.limelight.binding.video;

import org.junit.Test;

import java.util.Calendar;
import java.util.Locale;
import java.util.TimeZone;

import static org.junit.Assert.assertEquals;

/** The clock and battery line on the stats and over the settings panel. */
public class XrClockTest {

    private static Calendar at(int hour, int minute) {
        Calendar time = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        time.clear();
        time.set(2026, Calendar.OCTOBER, 1, hour, minute);
        return time;
    }

    @Test
    public void theTimeIsWrittenTheWayTheHeadsetIsSet() {
        assertEquals("14:32", XrClock.timeText(at(14, 32), true, Locale.UK));
        assertEquals("9:05", XrClock.timeText(at(9, 5), true, Locale.UK));
        assertEquals("0:00", XrClock.timeText(at(0, 0), true, Locale.UK));
        assertEquals("2:32 PM", XrClock.timeText(at(14, 32), false, Locale.US));
        assertEquals("12:00 AM", XrClock.timeText(at(0, 0), false, Locale.US));
    }

    @Test
    public void theBatteryFollowsTheTime() {
        assertEquals("14:32  Battery 63%", XrClock.line("14:32", 63, "Battery %1$d%%"));
        assertEquals("14:32  Battery 100%", XrClock.line("14:32", 100, "Battery %1$d%%"));
        assertEquals("14:32  Battery 0%", XrClock.line("14:32", 0, "Battery %1$d%%"));
        // Nothing read, or nothing to read: the time on its own
        assertEquals("14:32", XrClock.line("14:32", -1, "Battery %1$d%%"));
    }

    @Test
    public void theBatteryIntentIsReadAsAPercent() {
        assertEquals(63, XrClock.batteryPercent(63, 100));
        assertEquals(25, XrClock.batteryPercent(50, 200));
        assertEquals(67, XrClock.batteryPercent(2, 3));
        assertEquals(100, XrClock.batteryPercent(100, 100));
        assertEquals(100, XrClock.batteryPercent(120, 100));
        assertEquals(0, XrClock.batteryPercent(0, 100));
        assertEquals(-1, XrClock.batteryPercent(-1, 100));
        assertEquals(-1, XrClock.batteryPercent(50, 0));
        assertEquals(-1, XrClock.batteryPercent(50, -1));
    }
}
