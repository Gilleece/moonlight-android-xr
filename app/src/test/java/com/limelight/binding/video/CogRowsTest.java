package com.limelight.binding.video;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/**
 * Where the panel's rows are drawn, which has to be where the native side hit
 * tests and rings them. The display tab carries the head aim, controllers,
 * pointer sleep, ray, controller model and click rows and packs its thirteen
 * rows closer than the other tabs, and its choices are marked from one strip
 * that has to reach every row's cells. The screen tab carries head aim's two tracks under its
 * six and packs its eight rows closer too.
 */
public class CogRowsTest {

    @Test
    public void theDisplayTabHasThirteenRowsThatFit() {
        assertEquals(3, COG_OPTION_HEAD_LOCK);
        assertEquals(4, COG_OPTION_HEAD_AIM);
        assertEquals(5, COG_OPTION_GAMEPAD);
        assertEquals(6, COG_OPTION_POINTER_SLEEP);
        assertEquals(7, COG_OPTION_RAY);
        assertEquals(8, COG_OPTION_CONTROLLERS);
        assertEquals(9, COG_OPTION_CLICK_SOUND);
        assertEquals(12, COG_OPTION_COUNT);
        assertEquals(2, COG_GAMEPAD_CELLS);
        assertEquals(COG_OPTION_COUNT, COG_DISPLAY_SLIDER_ROW);
        assertEquals(14, SETTING_POINTER_SLEEP);
        assertEquals(15, SETTING_CLICK_SOUND);
        assertEquals(21, SETTING_CONTROLLER_MODEL);
        assertEquals(22, SETTING_HEAD_AIM_SENSITIVITY);
        assertEquals(23, SETTING_HEAD_AIM_DEADZONE);
        assertEquals(0.197f, XrPanels.cogRowV(COG_TAB_DISPLAY, 0), 1e-6f);
        assertEquals(0.917f, XrPanels.cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW), 1e-5f);
        // The first row's band starts under the tab bar
        assertTrue(XrPanels.cogRowV(COG_TAB_DISPLAY, 0) - COG_DISPLAY_ROW_HALF
                >= COG_TAB_BAR_B - 1e-6f);
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
    public void theScreenTabHasEightRowsOverItsReset() {
        assertEquals(8, COG_SCREEN_ROW_COUNT);
        assertEquals(COG_SLIDER_COUNT, COG_SLIDER_AIM_SENSITIVITY);
        assertEquals(COG_SLIDER_COUNT + 1, COG_SLIDER_AIM_DEADZONE);
        assertEquals(0.22f, XrPanels.cogRowV(COG_TAB_SCREEN, 0), 1e-6f);
        for (int row = 1; row < COG_SCREEN_ROW_COUNT; row++) {
            float gap = XrPanels.cogRowV(COG_TAB_SCREEN, row)
                    - XrPanels.cogRowV(COG_TAB_SCREEN, row - 1);
            assertTrue(gap >= 2.0f * COG_SCREEN_ROW_HALF - 1e-6f);
            assertTrue(gap > 2.0f * COG_SCREEN_CELL_HALF);
        }
        assertTrue(XrPanels.cogRowV(COG_TAB_SCREEN, COG_SCREEN_ROW_COUNT - 1)
                + COG_SCREEN_ROW_HALF < COG_RESET_T);
        // Two more sheets for a screen not locked to the head, after the room's
        assertEquals(COG_ART_ROOM_ABOUT + 1, COG_ART_SCREEN_WORLD);
        assertEquals(COG_ART_SCREEN_WORLD + 1, COG_ART_DISPLAY_WORLD);
        assertEquals(COG_ART_DISPLAY_WORLD + 1, COG_ART_COUNT);
    }

    @Test
    public void headAimsValuesReadWithTheirUnits() {
        assertEquals("8 px/\u00b0", XrPanels.headAimReadout(COG_SLIDER_AIM_SENSITIVITY, 8));
        assertEquals("30 px/\u00b0", XrPanels.headAimReadout(COG_SLIDER_AIM_SENSITIVITY, 30));
        assertEquals("2\u00b0/s", XrPanels.headAimReadout(COG_SLIDER_AIM_DEADZONE, 2));
        assertEquals("0\u00b0/s", XrPanels.headAimReadout(COG_SLIDER_AIM_DEADZONE, 0));
        assertEquals(2, READOUT_SCREEN);
    }

    @Test
    public void theOtherTabsKeepTheirRows() {
        for (int row = 0; row < COG_ROW3D_COUNT; row++) {
            assertEquals(COG_ROW_V0 + row * COG_ROW_STEP, XrPanels.cogRowV(COG_TAB_3D, row),
                    1e-6f);
        }
        for (int row = 0; row < PICTURE_VALUES; row++) {
            assertEquals(COG_ROW_V0 + row * COG_ROW_STEP, XrPanels.cogRowV(COG_TAB_PICTURE, row),
                    1e-6f);
        }
    }
}
