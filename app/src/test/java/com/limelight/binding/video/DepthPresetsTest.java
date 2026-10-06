package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;

/** The 3D tab's presets: three separations around each model's own, on the depth track. */
public class DepthPresetsTest {

    private static final int ZIPDEPTH = MidasDepthSource.ZIPDEPTH.defaultSeparation;
    private static final int MIDAS = MidasDepthSource.MIDAS.defaultSeparation;

    @Test
    public void balancedIsTheModelsOwn() {
        assertEquals(6, DepthPresets.value(DepthPresets.BALANCED, ZIPDEPTH));
        assertEquals(5, DepthPresets.value(DepthPresets.BALANCED, MIDAS));
    }

    @Test
    public void comfortAndStrongAreThreeStepsEitherSide() {
        assertArrayEquals(new int[] { 3, 6, 9 }, DepthPresets.values(ZIPDEPTH));
        assertArrayEquals(new int[] { 2, 5, 8 }, DepthPresets.values(MIDAS));
    }

    // The cells are drawn in the order the native side numbers them
    @Test
    public void theCellsAreInTrackOrder() {
        assertEquals(XrShared.COG_PRESET_COMFORT, DepthPresets.COMFORT);
        assertEquals(XrShared.COG_PRESET_BALANCED, DepthPresets.BALANCED);
        assertEquals(XrShared.COG_PRESET_STRONG, DepthPresets.STRONG);
        assertEquals(XrShared.COG_PRESET_CELLS, DepthPresets.values(ZIPDEPTH).length);
        assertEquals("Comfort", DepthPresets.name(DepthPresets.COMFORT));
        assertEquals("Balanced", DepthPresets.name(DepthPresets.BALANCED));
        assertEquals("Strong", DepthPresets.name(DepthPresets.STRONG));
        assertEquals("none", DepthPresets.name(DepthPresets.NONE));
    }

    @Test
    public void neverOffTheTrack() {
        assertEquals(0, DepthPresets.value(DepthPresets.COMFORT, 1));
        assertEquals(XrShared.COG_SEP_STEPS, DepthPresets.value(DepthPresets.STRONG, 14));
        for (int def = 0; def <= XrShared.COG_SEP_STEPS; def++) {
            for (int value : DepthPresets.values(def)) {
                assertEquals(Math.max(0, Math.min(XrShared.COG_SEP_STEPS, value)), value);
            }
        }
    }

    @Test
    public void theAccentIsOnTheCellTheValueIs() {
        assertEquals(DepthPresets.COMFORT, DepthPresets.presetFor(3, ZIPDEPTH));
        assertEquals(DepthPresets.BALANCED, DepthPresets.presetFor(6, ZIPDEPTH));
        assertEquals(DepthPresets.STRONG, DepthPresets.presetFor(9, ZIPDEPTH));
        assertEquals(DepthPresets.COMFORT, DepthPresets.presetFor(2, MIDAS));
        assertEquals(DepthPresets.BALANCED, DepthPresets.presetFor(5, MIDAS));
        assertEquals(DepthPresets.STRONG, DepthPresets.presetFor(8, MIDAS));
    }

    @Test
    public void anywhereElseOnTheTrackIsNone() {
        for (int value : new int[] { 0, 1, 2, 4, 5, 7, 8, 10, 15 }) {
            assertEquals("separation " + value, DepthPresets.NONE,
                    DepthPresets.presetFor(value, ZIPDEPTH));
        }
        // What the debug property can ask for, past the track's end
        assertEquals(DepthPresets.NONE, DepthPresets.presetFor(40, ZIPDEPTH));
    }

    // A default near an end clamps a preset onto it, and it still reads Balanced
    @Test
    public void aClampedPresetOnTheDefaultReadsBalanced() {
        assertEquals(DepthPresets.BALANCED, DepthPresets.presetFor(0, 0));
        assertEquals(DepthPresets.BALANCED,
                DepthPresets.presetFor(XrShared.COG_SEP_STEPS, XrShared.COG_SEP_STEPS));
        // And a clamped Comfort that is not the default still reads as Comfort
        assertEquals(DepthPresets.COMFORT, DepthPresets.presetFor(0, 2));
    }
}
