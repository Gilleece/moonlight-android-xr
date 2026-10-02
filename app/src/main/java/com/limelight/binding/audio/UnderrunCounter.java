package com.limelight.binding.audio;

/**
 * How many times the audio track has run dry, read off the track at most
 * once a second on the audio thread, so the stats can show it from any
 * thread without touching the track themselves.
 */
final class UnderrunCounter {
    /** The one thing read off the track: its running count of underruns. */
    interface Track {
        int underrunCount();
    }

    static final long READ_INTERVAL_MS = 1000;

    private final Track track;
    private long lastReadMs;
    private boolean everRead;
    private volatile int count = -1;

    UnderrunCounter(Track track) {
        this.track = track;
    }

    /** Audio thread, once a block: reads the track if a second has passed since the last read. */
    boolean poll(long nowMs) {
        if (everRead && nowMs - lastReadMs < READ_INTERVAL_MS) {
            return false;
        }
        everRead = true;
        lastReadMs = nowMs;
        count = track.underrunCount();
        return true;
    }

    /** Reads the track whenever it was last read, for the total as the stream ends. */
    int readNow() {
        count = track.underrunCount();
        return count;
    }

    /** Underruns at the last read, -1 before the first. Any thread. */
    int count() {
        return count;
    }
}
