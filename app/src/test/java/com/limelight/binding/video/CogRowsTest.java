package com.limelight.binding.video;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/**
 * Where the panel's rows are drawn, which has to be where the native side hit
 * tests and rings them. The display tab carries the pointer sleep row and
 * packs its eight rows closer than the other tabs.
 */
public class CogRowsTest {

    @Test
    public void theDisplayTabHasEightRowsThatFit() {
        assertEquals(4, COG_OPTION_POINTER_SLEEP);
        assertEquals(7, COG_OPTION_COUNT);
        assertEquals(COG_OPTION_COUNT, COG_DISPLAY_SLIDER_ROW);
        assertEquals(14, SETTING_POINTER_SLEEP);
        assertEquals(0.235f, XrPanels.cogRowV(COG_TAB_DISPLAY, 0), 1e-6f);
        assertEquals(0.907f, XrPanels.cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW), 1e-5f);
        for (int row = 1; row <= COG_DISPLAY_SLIDER_ROW; row++) {
            float gap = XrPanels.cogRowV(COG_TAB_DISPLAY, row)
                    - XrPanels.cogRowV(COG_TAB_DISPLAY, row - 1);
            assertTrue(gap >= 2.0f * COG_DISPLAY_ROW_HALF - 1e-6f);
            assertTrue(gap > 2.0f * COG_DISPLAY_CELL_HALF);
        }
    }

    @Test
    public void theOtherTabsKeepTheirRows() {
        for (int row = 0; row < COG_SLIDER_COUNT; row++) {
            assertEquals(COG_ROW_V0 + row * COG_ROW_STEP, XrPanels.cogRowV(COG_TAB_SCREEN, row),
                    1e-6f);
            assertEquals(COG_ROW_V0 + row * COG_ROW_STEP, XrPanels.cogRowV(COG_TAB_3D, row),
                    1e-6f);
        }
    }
}
