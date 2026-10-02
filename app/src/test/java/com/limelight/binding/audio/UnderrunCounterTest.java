package com.limelight.binding.audio;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** The track's underrun count, read once a second on the audio thread, against a fake track. */
public class UnderrunCounterTest {

    // Counts how often it is read, and runs dry when told to
    private static final class FakeTrack implements UnderrunCounter.Track {
        int underruns;
        int reads;

        @Override
        public int underrunCount() {
            reads++;
            return underruns;
        }
    }

    @Test
    public void nothingIsKnownBeforeTheFirstBlock() {
        UnderrunCounter counter = new UnderrunCounter(new FakeTrack());
        assertEquals(-1, counter.count());
    }

    @Test
    public void theTrackIsReadOnceASecondWhateverTheBlockRate() {
        FakeTrack track = new FakeTrack();
        UnderrunCounter counter = new UnderrunCounter(track);
        // 5 ms blocks for 3 s: the first block reads, then one a second
        int read = 0;
        for (long ms = 10000; ms < 13000; ms += 5) {
            if (counter.poll(ms)) {
                read++;
            }
        }
        assertEquals(3, read);
        assertEquals(3, track.reads);
        assertEquals(0, counter.count());
    }

    @Test
    public void aNewUnderrunShowsAtTheNextRead() {
        FakeTrack track = new FakeTrack();
        UnderrunCounter counter = new UnderrunCounter(track);
        assertTrue(counter.poll(1000));
        track.underruns = 2;
        // Not yet a second on
        assertFalse(counter.poll(1999));
        assertEquals(0, counter.count());
        assertTrue(counter.poll(2000));
        assertEquals(2, counter.count());
        // The track's count is a running total, and so is this
        track.underruns = 5;
        assertTrue(counter.poll(3000));
        assertEquals(5, counter.count());
    }

    @Test
    public void theTotalAtTheEndIsReadThereAndThen() {
        FakeTrack track = new FakeTrack();
        UnderrunCounter counter = new UnderrunCounter(track);
        counter.poll(1000);
        track.underruns = 7;
        assertEquals(7, counter.readNow());
        assertEquals(7, counter.count());
        assertEquals(2, track.reads);
    }
}
