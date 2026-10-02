package com.limelight.binding.video;

import org.junit.Test;

import static com.limelight.binding.video.XrDoffGrace.END;
import static com.limelight.binding.video.XrDoffGrace.GRACE_MS;
import static com.limelight.binding.video.XrDoffGrace.HOLD;
import static com.limelight.binding.video.XrDoffGrace.MARK_MS;
import static com.limelight.binding.video.XrDoffGrace.NOTHING;
import static com.limelight.binding.video.XrDoffGrace.RESUME;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * The hold a removed headset gets: when it starts, what brings the stream
 * back, the minute it runs at most counted from the first stop, its marks,
 * and the setting off leaving the stop as it always was.
 */
public class XrDoffGraceTest {

    // A session focused at t = 1 s, as every hold here starts from
    private static XrDoffGrace focusedAt1s(boolean enabled) {
        XrDoffGrace g = new XrDoffGrace(enabled);
        assertEquals(NOTHING, g.focused(1_000));
        return g;
    }

    @Test
    public void aMinuteWithMarksEveryTenSeconds() {
        assertEquals(60_000, GRACE_MS);
        assertEquals(10_000, MARK_MS);
    }

    @Test
    public void aDoffAndBackCarriesOn() {
        XrDoffGrace g = focusedAt1s(true);
        // The runtime backgrounds the activity, then stops the session
        assertEquals(HOLD, g.activityStopped(5_000));
        assertTrue(g.holding());
        assertEquals(NOTHING, g.sessionAway(5_200));
        // Back on: the activity first, which is not enough on its own
        assertEquals(NOTHING, g.activityStarted(25_000));
        assertTrue(g.holding());
        // Then the session focused again
        assertEquals(RESUME, g.focused(25_900));
        assertFalse(g.holding());
        assertEquals(20_900, g.lastHeldMs());
        // Nothing runs out afterwards
        assertEquals(NOTHING, g.tick(70_000));
    }

    @Test
    public void theSessionMayComeBackBeforeTheActivity() {
        XrDoffGrace g = focusedAt1s(true);
        assertEquals(HOLD, g.sessionAway(2_000));
        assertEquals(NOTHING, g.activityStopped(2_100));
        assertEquals(NOTHING, g.focused(10_000));
        assertTrue(g.holding());
        assertEquals(RESUME, g.activityStarted(10_050));
    }

    @Test
    public void aSessionThatOnlyGoesIdleIsHeldToo() {
        // A runtime that never stops the activity for a removed headset
        XrDoffGrace g = focusedAt1s(true);
        assertEquals(HOLD, g.sessionAway(3_000));
        assertEquals(RESUME, g.focused(13_000));
        assertEquals(10_000, g.lastHeldMs());
    }

    @Test
    public void anActivityStopWithTheSessionStillFocusedResumesOnStart() {
        XrDoffGrace g = focusedAt1s(true);
        assertEquals(HOLD, g.activityStopped(3_000));
        assertEquals(RESUME, g.activityStarted(4_000));
    }

    @Test
    public void theMinuteEndsTheStream() {
        XrDoffGrace g = focusedAt1s(true);
        long t0 = 10_000;
        assertEquals(HOLD, g.activityStopped(t0));
        assertEquals(NOTHING, g.tick(t0 + GRACE_MS - 1));
        assertTrue(g.holding());
        assertEquals(END, g.tick(t0 + GRACE_MS));
        assertFalse(g.holding());
        assertEquals(GRACE_MS, g.lastHeldMs());
        // Once
        assertEquals(NOTHING, g.tick(t0 + GRACE_MS + 1_000));
        // A late return finds nothing to resume
        assertEquals(NOTHING, g.activityStarted(t0 + GRACE_MS + 2_000));
        assertEquals(NOTHING, g.focused(t0 + GRACE_MS + 3_000));
    }

    @Test
    public void aReturnPastTheMinuteEndsTheStream() {
        // A headset asleep runs no timer, so the minute can be found gone
        // only as it comes back
        XrDoffGrace g = focusedAt1s(true);
        long t0 = 10_000;
        assertEquals(HOLD, g.activityStopped(t0));
        assertEquals(NOTHING, g.sessionAway(t0 + 100));
        assertEquals(NOTHING, g.activityStarted(t0 + 300_000));
        assertTrue(g.holding());
        assertEquals(END, g.focused(t0 + 301_000));
        assertFalse(g.holding());
        assertEquals(301_000, g.lastHeldMs());
        // Just inside it carries on
        assertEquals(HOLD, g.activityStopped(400_000));
        assertEquals(RESUME, g.activityStarted(400_000 + GRACE_MS - 1));
    }

    @Test
    public void aSecondDoffDuringTheHoldExtendsNothing() {
        XrDoffGrace g = focusedAt1s(true);
        long t0 = 10_000;
        assertEquals(HOLD, g.activityStopped(t0));
        assertEquals(NOTHING, g.sessionAway(t0 + 300));
        // Half back on, the activity started but the session not yet focused,
        // then off again
        assertEquals(NOTHING, g.activityStarted(t0 + 40_000));
        assertEquals(NOTHING, g.activityStopped(t0 + 41_000));
        assertEquals(NOTHING, g.sessionAway(t0 + 41_200));
        assertEquals(41_000, g.heldMs(t0 + 41_000));
        // Still the first doff's minute
        assertEquals(NOTHING, g.tick(t0 + GRACE_MS - 1));
        assertEquals(END, g.tick(t0 + GRACE_MS));
    }

    @Test
    public void aDoffAfterAReturnStartsAFreshMinute() {
        XrDoffGrace g = focusedAt1s(true);
        assertEquals(HOLD, g.activityStopped(10_000));
        assertEquals(RESUME, g.activityStarted(50_000));
        assertEquals(HOLD, g.activityStopped(55_000));
        assertEquals(NOTHING, g.tick(10_000 + GRACE_MS));
        assertEquals(NOTHING, g.tick(55_000 + GRACE_MS - 1));
        assertEquals(END, g.tick(55_000 + GRACE_MS));
    }

    @Test
    public void theMarksComeOncePerTenSeconds() {
        XrDoffGrace g = focusedAt1s(true);
        long t0 = 100_000;
        assertEquals(0, g.markDue(t0));
        assertEquals(-1, g.nextTickInMs(t0));
        g.activityStopped(t0);
        assertEquals(MARK_MS, g.nextTickInMs(t0));
        assertEquals(0, g.markDue(t0 + 9_999));
        assertEquals(10_000, g.markDue(t0 + 10_000));
        assertEquals(0, g.markDue(t0 + 10_500));
        assertEquals(9_500, g.nextTickInMs(t0 + 10_500));
        // A late tick says the mark it is past, once
        assertEquals(30_000, g.markDue(t0 + 31_000));
        assertEquals(0, g.markDue(t0 + 39_000));
        assertEquals(40_000, g.markDue(t0 + 40_000));
        assertEquals(50_000, g.markDue(t0 + 50_000));
        // The last tick lands on the minute, not past it
        assertEquals(10_000, g.nextTickInMs(t0 + 50_000));
        assertEquals(1_000, g.nextTickInMs(t0 + 59_000));
        assertEquals(0, g.nextTickInMs(t0 + 61_000));
        // A fresh hold counts from its own start
        g.activityStarted(t0 + 55_000);
        g.activityStopped(t0 + 70_000);
        assertEquals(0, g.markDue(t0 + 75_000));
        assertEquals(10_000, g.markDue(t0 + 80_000));
    }

    @Test
    public void nothingIsHeldBeforeTheFirstFocus() {
        // The launch hold looks after that time; a session going idle then is
        // not a removed headset
        XrDoffGrace g = new XrDoffGrace(true);
        assertEquals(NOTHING, g.sessionAway(1_000));
        assertFalse(g.holding());
        assertEquals(NOTHING, g.activityStarted(2_000));
    }

    @Test
    public void withTheSettingOffAStopEndsTheStreamAtOnce() {
        XrDoffGrace g = focusedAt1s(false);
        assertFalse(g.enabled());
        assertEquals(END, g.activityStopped(5_000));
        assertFalse(g.holding());
        // And a session gone idle is left alone, as it always was
        assertEquals(NOTHING, g.sessionAway(5_100));
        assertFalse(g.holding());
        assertEquals(NOTHING, g.tick(5_000 + GRACE_MS));
        assertEquals(NOTHING, g.focused(6_000));
        assertEquals(NOTHING, g.activityStarted(6_100));
    }

    @Test
    public void aCancelForgetsTheHold() {
        XrDoffGrace g = focusedAt1s(true);
        g.activityStopped(10_000);
        g.sessionAway(10_100);
        g.cancel(20_000);
        assertFalse(g.holding());
        assertEquals(NOTHING, g.tick(10_000 + GRACE_MS));
        assertEquals(NOTHING, g.activityStarted(21_000));
        // And the next doff is held afresh
        assertEquals(HOLD, g.activityStopped(30_000));
        assertEquals(RESUME, g.activityStarted(31_000));
    }
}
