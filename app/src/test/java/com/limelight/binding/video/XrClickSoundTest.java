package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/** The tick the panels make, and how much silence the frame loop keeps queued ahead of it. */
public class XrClickSoundTest {

    @Test
    public void theTickIsShortQuietAndEndsOnSilence() {
        short[] tick = XrClickSound.tickSamples(48000);
        // 18 ms at 48 kHz
        assertEquals(864, tick.length);
        assertEquals(0, tick[0]);
        int peak = 0;
        for (short s : tick) {
            peak = Math.max(peak, Math.abs(s));
        }
        // Well short of full scale, and not so quiet it is lost
        assertTrue(peak < Short.MAX_VALUE * 0.3);
        assertTrue(peak > Short.MAX_VALUE * 0.1);
        // The last samples are all but silent, so it ends without a click of
        // its own
        for (int i = tick.length - 8; i < tick.length; i++) {
            assertTrue(Math.abs(tick[i]) < 50);
        }
        // The same at another rate, scaled
        assertEquals(793, XrClickSound.tickSamples(44100).length);
    }

    @Test
    public void theQueueIsToppedUpToItsTarget() {
        // Nothing written yet: all of it
        assertEquals(1440, XrClickSound.framesToQueue(0, 0, 1440));
        // Full: nothing
        assertEquals(0, XrClickSound.framesToQueue(1440, 0, 1440));
        // Part played
        assertEquals(500, XrClickSound.framesToQueue(10000, 9060, 1440));
        // A tick queued over the target leaves nothing to add
        assertEquals(0, XrClickSound.framesToQueue(12000, 9000, 1440));
    }

    @Test
    public void aTickAlwaysFitsBehindTheSilence() {
        // A roomy buffer keeps the full 30 ms
        assertEquals(1440, XrClickSound.queueFor(4608, 864));
        // A tight one keeps less, so the tick and a quarter again still fit
        assertEquals(2016 - 864 - 216, XrClickSound.queueFor(2016, 864));
        assertTrue(XrClickSound.queueFor(2016, 864) + 864 <= 2016);
        // And never next to none
        assertEquals(144, XrClickSound.queueFor(900, 864));
    }

    @Test
    public void aSlowFrameLoopQueuesMore() {
        // At a steady 90 Hz a frame and a mixer cycle come to a little over
        // the usual 30 ms, and a faster loop keeps the usual
        assertEquals(1493, XrClickSound.queueTarget(1440, 533 + 960, 4608, 864));
        assertEquals(1440, XrClickSound.queueTarget(1440, 400 + 960, 4608, 864));
        // A 40 ms frame while the session loads queues enough to last it
        assertEquals(1920 + 960, XrClickSound.queueTarget(1440, 1920 + 960, 4608, 864));
        // But never so much a tick will not fit behind it
        assertEquals(4608 - 864 - 216, XrClickSound.queueTarget(1440, 9000, 4608, 864));
    }

    @Test
    public void theTrackHeadWrapsWithoutHarm() {
        // The track counts frames played in an int that wraps after a day of
        // 48 kHz, and reads negative past 2^31
        long written = (1L << 32) + 1000;
        int head = 0;
        assertEquals(440, XrClickSound.framesToQueue(written, head, 1440));
        long nearTop = (1L << 31) + 2000;
        int headPast = (int)((1L << 31) + 1000);
        assertTrue(headPast < 0);
        assertEquals(440, XrClickSound.framesToQueue(nearTop, headPast, 1440));
    }
}
