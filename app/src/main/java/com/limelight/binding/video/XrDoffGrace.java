package com.limelight.binding.video;

/**
 * Keeps the stream through a headset taken off for a moment. Once the session
 * has been focused, the activity stopping (the runtime backgrounds it when the
 * headset comes off) or the session going to stopping or idle starts a hold
 * rather than ending the stream. The hold lasts until the activity is started
 * again and the session is focused again, or until a minute has passed since
 * it began, when the stream ends. Another stop during a hold changes nothing:
 * the minute still runs from the first. The times are wall clock, sleep
 * included, so a headset that slept through the minute ends the stream when
 * it wakes rather than carrying on.
 *
 * With the setting off a stop ends the stream at once, as it always did, and
 * a session that goes idle is left as it always was.
 *
 * Plain state over the times handed in, so it runs on a desktop. One thread.
 */
public final class XrDoffGrace {

    /** How long a hold lasts before the stream ends. */
    public static final long GRACE_MS = 60_000;
    /** How often a hold says how long it has run. */
    public static final long MARK_MS = 10_000;

    /** What a step asks of the stream. */
    public static final int NOTHING = 0;
    // A hold has just begun: keep the stream, mute it and send nothing
    public static final int HOLD = 1;
    // The headset is back: carry on with picture, sound and input
    public static final int RESUME = 2;
    // End the stream: a stop with the setting off, or a hold run out
    public static final int END = 3;

    private final boolean enabled;
    private boolean focusedOnce;
    // Why a hold goes on: the activity stopped, the session away
    private boolean stopped;
    private boolean away;
    private boolean holding;
    private long heldSinceMs;
    // The last whole mark said, and how long the last hold ran
    private long markSaid;
    private long lastHeldMs;

    public XrDoffGrace(boolean enabled) {
        this.enabled = enabled;
    }

    public boolean enabled() {
        return enabled;
    }

    /** A hold is running. */
    public boolean holding() {
        return holding;
    }

    /** How long the running hold has run, 0 with none. */
    public long heldMs(long nowMs) {
        return holding ? nowMs - heldSinceMs : 0;
    }

    /** How long the last hold to end ran, by a resume or by running out. */
    public long lastHeldMs() {
        return lastHeldMs;
    }

    /**
     * The session is focused, the first time or back again. Ends a hold once
     * the activity is started as well: RESUME, or END past the minute.
     */
    public int focused(long nowMs) {
        focusedOnce = true;
        away = false;
        return resumeIfBack(nowMs);
    }

    /**
     * The session went to stopping or idle. Before the first focus that is the
     * launch's business, not this.
     */
    public int sessionAway(long nowMs) {
        if (!focusedOnce || !enabled) {
            return NOTHING;
        }
        away = true;
        return startHold(nowMs);
    }

    /**
     * The activity stopped, or its window surface went, after the first focus.
     * END with the setting off, so the caller stops as it always has.
     */
    public int activityStopped(long nowMs) {
        if (!enabled) {
            return END;
        }
        focusedOnce = true;
        stopped = true;
        return startHold(nowMs);
    }

    /** The activity is started again, or its window surface is back. As focused. */
    public int activityStarted(long nowMs) {
        stopped = false;
        return resumeIfBack(nowMs);
    }

    /** END once the hold has run its minute, which also ends the hold. */
    public int tick(long nowMs) {
        if (!holding || nowMs - heldSinceMs < GRACE_MS) {
            return NOTHING;
        }
        finishHold(nowMs);
        return END;
    }

    /**
     * The next whole mark of a hold, 10 s, 20 s and so on, the first time it
     * is asked for at or past it; 0 when no new mark is due.
     */
    public long markDue(long nowMs) {
        if (!holding) {
            return 0;
        }
        long mark = (nowMs - heldSinceMs) / MARK_MS * MARK_MS;
        if (mark <= markSaid) {
            return 0;
        }
        markSaid = mark;
        return mark;
    }

    /** How long until the next mark or the end of the hold, -1 with none. */
    public long nextTickInMs(long nowMs) {
        if (!holding) {
            return -1;
        }
        long held = nowMs - heldSinceMs;
        long next = (held / MARK_MS + 1) * MARK_MS;
        return Math.max(0, Math.min(next, GRACE_MS) - held);
    }

    /** Forgets a hold without saying anything, for a stream ending another way. */
    public void cancel(long nowMs) {
        if (holding) {
            finishHold(nowMs);
        }
        stopped = false;
        away = false;
    }

    private int startHold(long nowMs) {
        if (holding) {
            return NOTHING;
        }
        holding = true;
        heldSinceMs = nowMs;
        markSaid = 0;
        return HOLD;
    }

    private int resumeIfBack(long nowMs) {
        if (!holding || stopped || away) {
            return NOTHING;
        }
        finishHold(nowMs);
        return lastHeldMs >= GRACE_MS ? END : RESUME;
    }

    private void finishHold(long nowMs) {
        lastHeldMs = nowMs - heldSinceMs;
        holding = false;
    }
}
