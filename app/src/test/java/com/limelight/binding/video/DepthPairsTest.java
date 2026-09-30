package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** The pairs of depth staging on their way through the depth threads. */
public class DepthPairsTest {

    /** A pair captured, read back and handed to the model, which is left running it. */
    private static int toModel(DepthPairs pairs) {
        int pair = pairs.forCapture();
        pairs.captured(pair);
        assertEquals(pair, pairs.takeToFinish());
        pairs.finished(pair, true);
        assertEquals(pair, pairs.takeToRun());
        return pair;
    }

    /** A second pair captured and read back while the model runs the first. */
    private static int staged(DepthPairs pairs) {
        int pair = pairs.forCapture();
        pairs.captured(pair);
        assertEquals(pair, pairs.takeToFinish());
        pairs.finished(pair, true);
        return pair;
    }

    @Test
    public void anIdlePipelineOnlyTakesACapture() {
        DepthPairs pairs = new DepthPairs(2);
        assertEquals(0, pairs.forCapture());
        assertEquals(-1, pairs.takeToFinish());
        assertEquals(-1, pairs.takeToRun());
        assertEquals(-1, pairs.takeToUpload());
    }

    /** The overlap itself: the next frame is read back while the model runs. */
    @Test
    public void theNextCaptureIsReadBackWhileTheModelRuns() {
        DepthPairs pairs = new DepthPairs(2);
        int running = toModel(pairs);
        int next = pairs.forCapture();
        assertTrue(next >= 0 && next != running);
        pairs.captured(next);
        assertEquals(next, pairs.takeToFinish());
        pairs.finished(next, true);
        // Staged, and the model is still busy with the first
        assertEquals(-1, pairs.takeToRun());
        assertEquals(DepthPairs.STAGED, pairs.state(next));
    }

    /** Only one frame ever waits for the model, whether captured, reading back or staged. */
    @Test
    public void onlyOneCaptureWaitsForTheModel() {
        DepthPairs pairs = new DepthPairs(3);
        toModel(pairs);
        int waiting = pairs.forCapture();
        pairs.captured(waiting);
        assertEquals(-1, pairs.forCapture());
        pairs.takeToFinish();
        assertEquals(-1, pairs.forCapture());
        pairs.finished(waiting, true);
        assertEquals(-1, pairs.forCapture());
    }

    /** The model takes the staged pair as soon as it is done with the other. */
    @Test
    public void theModelTakesTheWaitingPairStraightAfter() {
        DepthPairs pairs = new DepthPairs(2);
        int first = toModel(pairs);
        int second = staged(pairs);

        pairs.ran(first, true);
        assertEquals(second, pairs.takeToRun());
        // And the first map goes up while the second runs
        assertEquals(first, pairs.takeToUpload());
        assertTrue(pairs.made(first));
        pairs.freed(first);
        assertEquals(first, pairs.forCapture());
    }

    /** One pair waiting to go up and the other in the model: no capture. */
    @Test
    public void noCaptureWithBothPairsBusy() {
        DepthPairs pairs = new DepthPairs(2);
        int first = toModel(pairs);
        staged(pairs);
        pairs.ran(first, true);
        pairs.takeToRun();
        assertEquals(-1, pairs.forCapture());
    }

    /**
     * A newer capture that could not be read back waits for the older map to
     * go up before it is dropped, so whatever its cut check found lands after
     * that map, not before it.
     */
    @Test
    public void aDropWaitsBehindAnOlderMap() {
        DepthPairs pairs = new DepthPairs(2);
        int older = toModel(pairs);
        int newer = pairs.forCapture();
        pairs.captured(newer);
        pairs.takeToFinish();
        pairs.finished(newer, false);
        assertEquals(DepthPairs.RAN, pairs.state(newer));
        assertEquals(-1, pairs.takeToUpload());

        pairs.ran(older, true);
        assertEquals(older, pairs.takeToUpload());
        assertEquals(-1, pairs.takeToUpload());
        pairs.freed(older);
        assertEquals(newer, pairs.takeToUpload());
        assertFalse(pairs.made(newer));
    }

    /** A newer map the model finishes first still waits for the older one. */
    @Test
    public void aNewerMapWaitsForTheOlderOne() {
        DepthPairs pairs = new DepthPairs(2);
        int older = toModel(pairs);
        int newer = staged(pairs);
        // Not an order the one model thread can produce, but the upload order
        // must not rest on that
        pairs.ran(newer, true);
        assertEquals(-1, pairs.takeToUpload());
        pairs.ran(older, true);
        assertEquals(older, pairs.takeToUpload());
        pairs.freed(older);
        assertEquals(newer, pairs.takeToUpload());
    }

    /** A pair used again goes to the back of the order, behind the other. */
    @Test
    public void mapsGoUpInCaptureOrderAsPairsComeRound() {
        DepthPairs pairs = new DepthPairs(2);
        int a = toModel(pairs);
        int b = staged(pairs);
        pairs.ran(a, true);
        pairs.takeToRun();
        pairs.takeToUpload();
        pairs.freed(a);

        // a again, captured after b
        assertEquals(a, pairs.forCapture());
        pairs.captured(a);
        pairs.takeToFinish();
        pairs.finished(a, true);
        pairs.ran(b, true);
        assertEquals(a, pairs.takeToRun());
        pairs.ran(a, true);
        assertEquals(b, pairs.takeToUpload());
        pairs.freed(b);
        assertEquals(a, pairs.takeToUpload());
    }

    /** A run the model made nothing of is dropped rather than uploaded. */
    @Test
    public void aFailedRunIsDropped() {
        DepthPairs pairs = new DepthPairs(2);
        int pair = toModel(pairs);
        pairs.ran(pair, false);
        assertEquals(pair, pairs.takeToUpload());
        assertFalse(pairs.made(pair));
        pairs.freed(pair);
        assertEquals(DepthPairs.FREE, pairs.state(pair));
    }

    /** A pair captured again after a failed run carries nothing of it over. */
    @Test
    public void aPairUsedAgainStartsClean() {
        DepthPairs pairs = new DepthPairs(2);
        int pair = toModel(pairs);
        pairs.ran(pair, true);
        pairs.takeToUpload();
        pairs.freed(pair);
        assertEquals(pair, pairs.forCapture());
        pairs.captured(pair);
        assertFalse(pairs.made(pair));
        assertEquals(DepthPairs.CAPTURED, pairs.state(pair));
    }
}
