package com.limelight.binding.input;

/**
 * Holds the host cursor still around a click from the VR pointer, so a double
 * click lands.
 *
 * A host tests position as well as timing before it calls two clicks a double,
 * and a hand held pointer drifts a few pixels between the taps. The moves
 * carrying that drift are what break the test. Correcting it with a position
 * packet on the second tap does not work either, since the host batches mouse
 * moves but not button events, so the correction can land after the click it
 * was meant to lead. Nothing is sent instead: the first tap anchors the cursor
 * and drift near that anchor is dropped, which leaves the host on the pixel it
 * already has.
 *
 * Pure bookkeeping over pixel positions and clock readings, so it can be run
 * on a desktop without a headset or a host.
 */
public final class XrClickAnchor {
    // How long after a tap the cursor is held, measured from the most recent
    // tap so a chain of them keeps the first one's pixel
    static final long HOLD_MS = 500;
    // Tight while the button is held, so a drag or a text selection still
    // moves the cursor, and wider once it is up, to cover the 5 to 27 px of
    // drift measured between the taps of a natural pair. Both as a fraction of
    // the frame width.
    static final int HOLD_DIV_DOWN = 200;
    static final int HOLD_DIV_UP = 72;

    private final int frameWidth;
    private boolean active;
    private boolean leftDown;
    private long lastDownTime;
    private int anchorX;
    private int anchorY;
    private String holdEnd;

    public XrClickAnchor(int frameWidth) {
        this.frameWidth = frameWidth;
    }

    /**
     * Whether a pointer move to x,y should reach the host. Drift near a fresh
     * anchor is dropped, and a move that escapes the anchor or arrives after
     * the window ends the hold and is sent.
     */
    public boolean onMove(int x, int y, long now) {
        holdEnd = null;
        if (!active) {
            return true;
        }

        long since = now - lastDownTime;
        int radius = radius();
        int dx = x - anchorX;
        int dy = y - anchorY;
        if (since < HOLD_MS && dx * dx + dy * dy <= radius * radius) {
            return false;
        }

        active = false;
        holdEnd = since >= HOLD_MS ? "timeout"
                : "escape " + (int)Math.sqrt(dx * dx + dy * dy) + "px";
        return true;
    }

    /**
     * The left button went down with the pointer last sent to pointerX,
     * pointerY, or -1,-1 if nothing has been sent yet. Anchors the cursor
     * there, or keeps the anchor a chain of taps already has. Returns a word
     * on what happened, for the log.
     */
    public String onLeftDown(int pointerX, int pointerY, long now) {
        String hold;
        if (active) {
            // Nothing has gone out since the first tap, so the host is still
            // on the anchor and a chain needs no position work
            hold = "held on anchor " + anchorX + "," + anchorY;
        }
        else if (pointerX < 0) {
            hold = "no hold (no pointer yet)";
        }
        else {
            anchorX = pointerX;
            anchorY = pointerY;
            active = true;
            hold = "hold from " + pointerX + "," + pointerY;
        }

        // The window runs from the most recent tap, so a chain of them keeps
        // the cursor on the first one's pixel
        lastDownTime = now;
        leftDown = true;
        return hold;
    }

    /** The left button came up. The hold itself carries on, only its radius opens up. */
    public void onLeftUp() {
        leftDown = false;
    }

    /** Why the last hold ended, if the last move ended one, else null. */
    public String holdEnd() {
        return holdEnd;
    }

    public boolean isActive() {
        return active;
    }

    // Half a percent of the frame width with the button down, about 13 px at
    // 2560 wide, and one part in 72 with it up, about 35 px
    int radius() {
        int div = leftDown ? HOLD_DIV_DOWN : HOLD_DIV_UP;
        return Math.max(1, frameWidth / div);
    }
}
