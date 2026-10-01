package com.limelight.preferences;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

/** The runtime's display rates as stored for the frame rate list, and read back. */
public class XrDisplayRatesTest {

    @Test
    public void ratesAreStoredWholeLowestFirstAndOnce() {
        assertEquals("60,72,80,90,120",
                XrDisplayRates.format(new float[] { 60.0f, 72.0f, 80.0f, 90.0f, 120.0f }));
        assertEquals("72,90,120", XrDisplayRates.format(new float[] { 119.88f, 89.91f, 72.0f }));
        assertEquals("72,90", XrDisplayRates.format(new float[] { 90.0f, 72.0f, 90.2f }));
    }

    @Test
    public void nothingOfferedStoresNothing() {
        assertEquals("", XrDisplayRates.format(null));
        assertEquals("", XrDisplayRates.format(new float[0]));
        assertEquals("", XrDisplayRates.format(new float[] { 0.0f, -90.0f }));
        assertEquals("72", XrDisplayRates.format(new float[] { 0.0f, 72.0f }));
    }

    @Test
    public void theHighestIsReadBack() {
        assertEquals(120, XrDisplayRates.highest("60,72,80,90,120"));
        assertEquals(90, XrDisplayRates.highest("72,90"));
        assertEquals(120, XrDisplayRates.highest(XrDisplayRates.format(
                new float[] { 119.88f, 72.0f })));
    }

    @Test
    public void nothingStoredIsZero() {
        assertEquals(0, XrDisplayRates.highest(null));
        assertEquals(0, XrDisplayRates.highest(""));
        assertEquals(0, XrDisplayRates.highest("fast"));
    }

    @Test
    public void aBadEntrySpoilsOnlyItself() {
        assertEquals(90, XrDisplayRates.highest("72,x,90"));
        assertEquals(120, XrDisplayRates.highest(" 72 , 120 "));
        assertEquals(72, XrDisplayRates.highest("72,"));
    }
}
