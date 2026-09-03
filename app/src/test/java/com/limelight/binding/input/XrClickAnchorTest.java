package com.limelight.binding.input;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

public class XrClickAnchorTest {
    private static final int WIDTH = 2560;

    @Test
    public void movesGoOutUntilSomethingIsClicked() {
        XrClickAnchor anchor = new XrClickAnchor(WIDTH);
        assertTrue(anchor.onMove(100, 100, 1000));
        assertTrue(anchor.onMove(103, 101, 1010));
        assertFalse(anchor.isActive());
    }

    @Test
    public void driftAroundAFreshClickIsDropped() {
        XrClickAnchor anchor = new XrClickAnchor(WIDTH);
        assertEquals("hold from 100,100", anchor.onLeftDown(100, 100, 1000));
        anchor.onLeftUp();
        assertTrue(anchor.isActive());

        // Well inside the 35 px the button-up radius allows at this width
        assertFalse(anchor.onMove(110, 108, 1050));
        assertFalse(anchor.onMove(90, 95, 1100));
        assertNull(anchor.holdEnd());
        assertTrue(anchor.isActive());
    }

    @Test
    public void escapingTheAnchorEndsTheHoldAndSendsTheMove() {
        XrClickAnchor anchor = new XrClickAnchor(WIDTH);
        anchor.onLeftDown(100, 100, 1000);
        anchor.onLeftUp();

        assertTrue(anchor.onMove(200, 100, 1100));
        assertEquals("escape 100px", anchor.holdEnd());
        assertFalse(anchor.isActive());
        // And from then on every move goes out again
        assertTrue(anchor.onMove(201, 100, 1110));
        assertNull(anchor.holdEnd());
    }

    @Test
    public void theHoldTimesOut() {
        XrClickAnchor anchor = new XrClickAnchor(WIDTH);
        anchor.onLeftDown(100, 100, 1000);
        anchor.onLeftUp();

        assertFalse(anchor.onMove(101, 100, 1000 + XrClickAnchor.HOLD_MS - 1));
        assertTrue(anchor.onMove(101, 100, 1000 + XrClickAnchor.HOLD_MS));
        assertEquals("timeout", anchor.holdEnd());
    }

    @Test
    public void theRadiusIsTightWhileTheButtonIsHeld() {
        XrClickAnchor anchor = new XrClickAnchor(WIDTH);
        anchor.onLeftDown(100, 100, 1000);
        assertEquals(WIDTH / XrClickAnchor.HOLD_DIV_DOWN, anchor.radius());

        // 20 px is drift with the button up but a drag with it down
        assertTrue(anchor.onMove(120, 100, 1050));
        assertEquals("escape 20px", anchor.holdEnd());

        anchor.onLeftDown(120, 100, 1100);
        anchor.onLeftUp();
        assertEquals(WIDTH / XrClickAnchor.HOLD_DIV_UP, anchor.radius());
        assertFalse(anchor.onMove(140, 100, 1150));
    }

    @Test
    public void aSecondTapKeepsTheFirstAnchorAndRestartsTheWindow() {
        XrClickAnchor anchor = new XrClickAnchor(WIDTH);
        anchor.onLeftDown(100, 100, 1000);
        anchor.onLeftUp();
        assertFalse(anchor.onMove(105, 104, 1200));

        assertEquals("held on anchor 100,100", anchor.onLeftDown(105, 104, 1300));
        anchor.onLeftUp();
        // Measured from the second tap, so this is still inside the window
        assertFalse(anchor.onMove(106, 104, 1300 + XrClickAnchor.HOLD_MS - 1));
        assertTrue(anchor.onMove(106, 104, 1300 + XrClickAnchor.HOLD_MS));
    }

    @Test
    public void nothingToAnchorBeforeThePointerHasSpoken() {
        XrClickAnchor anchor = new XrClickAnchor(WIDTH);
        assertEquals("no hold (no pointer yet)", anchor.onLeftDown(-1, -1, 1000));
        assertFalse(anchor.isActive());
        assertTrue(anchor.onMove(5, 5, 1010));
    }

    @Test
    public void theRadiusNeverCollapsesOnANarrowFrame() {
        XrClickAnchor anchor = new XrClickAnchor(10);
        anchor.onLeftDown(1, 1, 0);
        assertEquals(1, anchor.radius());
    }
}
