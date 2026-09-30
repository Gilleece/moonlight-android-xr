package com.limelight.binding.video;

/**
 * Tracks each pair of depth staging through a map's trip. The frame loop
 * captures into a free pair, the stage thread reads the capture back into
 * the pair's model input, the depth thread runs the model on it, and the
 * stage thread uploads the map, or drops the pair when the model made
 * nothing, and frees it.
 *
 * Two rules keep the overlap in order. Only one capture ever waits for the
 * model, so a frame never queues behind another older one. And a pair is
 * only uploaded or dropped once every pair captured before it has been, so
 * maps go up in the order their frames arrived, and whatever a scene cut
 * check asked of a map lands on that map and no other.
 *
 * Not thread safe: the renderer holds its depth lock around every call.
 */
final class DepthPairs {
    static final int FREE = 0;
    static final int CAPTURED = 1;
    static final int FINISHING = 2;
    static final int STAGED = 3;
    static final int RUNNING = 4;
    static final int RAN = 5;
    static final int UPLOADING = 6;

    private final int[] state;
    // When each pair was captured, as a count, and whether the model made a
    // map from it
    private final long[] order;
    private final boolean[] made;
    private long captures;

    DepthPairs(int pairs) {
        state = new int[pairs];
        order = new long[pairs];
        made = new boolean[pairs];
    }

    int state(int pair) {
        return state[pair];
    }

    /**
     * The pair the next capture goes into, or -1 when a capture is already
     * waiting for the model or no pair is free. One frame ahead of the model
     * is all the overlap needs, and a second would only be staler by the
     * time the model got to it. The pair stays free until captured() is
     * called, which is safe since only the frame loop hands free pairs out.
     */
    int forCapture() {
        int free = -1;
        for (int i = 0; i < state.length; i++) {
            int s = state[i];
            if (s == CAPTURED || s == FINISHING || s == STAGED) {
                return -1;
            }
            if (s == FREE && free < 0) {
                free = i;
            }
        }
        return free;
    }

    /** A readback was queued into the pair forCapture() gave out. */
    void captured(int pair) {
        order[pair] = ++captures;
        made[pair] = false;
        state[pair] = CAPTURED;
    }

    /** The capture for the stage thread to read back next, now in hand, or -1. */
    int takeToFinish() {
        int pair = first(CAPTURED);
        if (pair >= 0) {
            state[pair] = FINISHING;
        }
        return pair;
    }

    /**
     * A readback done: staged for the model, or, when there was nothing to
     * run the model on, straight to waiting its turn to be dropped.
     */
    void finished(int pair, boolean staged) {
        state[pair] = staged ? STAGED : RAN;
    }

    /** The pair for the model to run next, now running, or -1 while it runs another. */
    int takeToRun() {
        if (first(RUNNING) >= 0) {
            return -1;
        }
        int pair = first(STAGED);
        if (pair >= 0) {
            state[pair] = RUNNING;
        }
        return pair;
    }

    /** The model is done with the pair, and says whether it made a map. */
    void ran(int pair, boolean madeMap) {
        made[pair] = madeMap;
        state[pair] = RAN;
    }

    /**
     * The pair to upload or drop next, now in hand, or -1: the oldest pair
     * in flight, once the model is done with it.
     */
    int takeToUpload() {
        int oldest = -1;
        for (int i = 0; i < state.length; i++) {
            if (state[i] != FREE && (oldest < 0 || order[i] < order[oldest])) {
                oldest = i;
            }
        }
        if (oldest < 0 || state[oldest] != RAN) {
            return -1;
        }
        state[oldest] = UPLOADING;
        return oldest;
    }

    /** Whether the model made a map of the pair, so it is uploaded rather than dropped. */
    boolean made(int pair) {
        return made[pair];
    }

    /** The pair's map is up or dropped, and the pair is free for the next capture. */
    void freed(int pair) {
        state[pair] = FREE;
    }

    private int first(int wanted) {
        for (int i = 0; i < state.length; i++) {
            if (state[i] == wanted) {
                return i;
            }
        }
        return -1;
    }
}
