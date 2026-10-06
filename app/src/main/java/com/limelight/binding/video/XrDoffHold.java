package com.limelight.binding.video;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

import com.limelight.FileLog;

import java.util.Locale;

/**
 * A removed headset's hold run on the main thread: XrDoffGrace's decisions,
 * a line in the log every 10 s while it lasts, and what the stream does about
 * each handed to its owner. Timed by the clock that counts sleep, since a
 * headset that sleeps stops the one the handler waits on. Main thread only.
 */
public final class XrDoffHold {

    /** What the stream does as a hold starts, ends and runs out. Main thread. */
    public interface Owner {
        // Held: mute the sound and send nothing
        void onDoffHold();

        // The headset is back on: sound and input again
        void onDoffResume();

        // The minute is up: end the stream as the exit button ends it
        void onDoffExpired();
    }

    private final XrDoffGrace grace;
    private final Owner owner;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!grace.holding()) {
                return;
            }
            long now = SystemClock.elapsedRealtime();
            long mark = grace.markDue(now);
            if (grace.tick(now) == XrDoffGrace.END) {
                expired();
                return;
            }
            if (mark > 0) {
                FileLog.event("stream held " + (mark / 1000) + " s for the headset");
            }
            handler.postDelayed(this, grace.nextTickInMs(now));
        }
    };

    public XrDoffHold(boolean enabled, Owner owner) {
        this.grace = new XrDoffGrace(enabled);
        this.owner = owner;
    }

    public boolean holding() {
        return grace.holding();
    }

    /**
     * The activity stopped, or its window surface went, after the session's
     * first focus. Returns whether the stream is held rather than to be ended
     * as it always was, which with the setting off it never is.
     */
    public boolean activityStopped(String why) {
        if (grace.activityStopped(SystemClock.elapsedRealtime()) == XrDoffGrace.HOLD) {
            started(why);
        }
        return grace.holding();
    }

    /** The activity is started again, or its window surface is back. */
    public void activityStarted() {
        back(grace.activityStarted(SystemClock.elapsedRealtime()));
    }

    /** The session went to stopping or idle after its first focus. */
    public void sessionAway() {
        if (grace.sessionAway(SystemClock.elapsedRealtime()) == XrDoffGrace.HOLD) {
            started("the session stopped");
        }
    }

    /** The session is focused again. */
    public void sessionBack() {
        back(grace.focused(SystemClock.elapsedRealtime()));
    }

    /** The stream is ending some other way, so a hold is let go quietly. */
    public void cancel() {
        handler.removeCallbacks(tick);
        grace.cancel(SystemClock.elapsedRealtime());
    }

    private void started(String why) {
        FileLog.event("headset off (" + why + "), holding the stream for up to "
                + (XrDoffGrace.GRACE_MS / 1000) + " s with the sound muted");
        owner.onDoffHold();
        handler.removeCallbacks(tick);
        handler.postDelayed(tick, grace.nextTickInMs(SystemClock.elapsedRealtime()));
    }

    // Back on, in time or, after a headset asleep, too late
    private void back(int action) {
        if (action == XrDoffGrace.END) {
            expired();
        }
        else if (action == XrDoffGrace.RESUME) {
            handler.removeCallbacks(tick);
            FileLog.event(String.format(Locale.ROOT, "headset back after %.1f s, carrying on",
                    grace.lastHeldMs() / 1000.0));
            owner.onDoffResume();
        }
    }

    private void expired() {
        handler.removeCallbacks(tick);
        FileLog.event(String.format(Locale.ROOT, "stream held %.0f s for the headset and it did "
                + "not come back, ending the stream", grace.lastHeldMs() / 1000.0));
        owner.onDoffExpired();
    }
}
