package com.limelight.binding.video;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/**
 * Where the panel's rows are drawn, which has to be where the native side hit
 * tests and rings them. The display tab carries the pointer sleep, ray and
 * click rows and packs its ten rows closer than the other tabs, and its
 * choices are marked from one strip that has to reach every row's cells.
 */
public class CogRowsTest {

    @Test
    public void theDisplayTabHasTenRowsThatFit() {
        assertEquals(4, COG_OPTION_POINTER_SLEEP);
        assertEquals(5, COG_OPTION_RAY);
        assertEquals(6, COG_OPTION_CLICK_SOUND);
        assertEquals(9, COG_OPTION_COUNT);
        assertEquals(COG_OPTION_COUNT, COG_DISPLAY_SLIDER_ROW);
        assertEquals(14, SETTING_POINTER_SLEEP);
        assertEquals(15, SETTING_CLICK_SOUND);
        assertEquals(0.21f, XrPanels.cogRowV(COG_TAB_DISPLAY, 0), 1e-6f);
        assertEquals(0.921f, XrPanels.cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW), 1e-5f);
        for (int row = 1; row <= COG_DISPLAY_SLIDER_ROW; row++) {
            float gap = XrPanels.cogRowV(COG_TAB_DISPLAY, row)
                    - XrPanels.cogRowV(COG_TAB_DISPLAY, row - 1);
            assertTrue(gap >= 2.0f * COG_DISPLAY_ROW_HALF - 1e-6f);
            assertTrue(gap > 2.0f * COG_DISPLAY_CELL_HALF);
        }
        // The glow level's thumb, grown under the ray, still clears the bottom
        float thumbHalf = 0.085f * 1.25f * 0.5f;
        assertTrue(XrPanels.cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW) + thumbHalf < 1.0f);
    }

    @Test
    public void theMarksReachEveryRowsCells() {
        assertEquals(COG_OPTION_COUNT, MARK_VALUES);
        float top = COG_MARKS_T;
        float bottom = COG_MARKS_T + COG_MARKS_TEX_H / (float)COG_TEX_H;
        float left = COG_MARKS_L;
        float right = COG_MARKS_L + COG_MARKS_TEX_W / (float)COG_TEX_W;
        assertTrue(left < COG_TRACK_L);
        assertTrue(right > COG_TRACK_R);
        for (int row = 0; row < COG_OPTION_COUNT; row++) {
            float v = XrPanels.cogRowV(COG_TAB_DISPLAY, row);
            assertTrue(v - COG_DISPLAY_CELL_HALF > top);
            assertTrue(v + COG_DISPLAY_CELL_HALF < bottom);
        }
        // And stops short of the glow level's track under them
        assertTrue(bottom < XrPanels.cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW)
                - COG_DISPLAY_CELL_HALF);
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
