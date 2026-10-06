package com.limelight.binding.video;

import com.limelight.R;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;

/**
 * The words that say which shortcut leaves gamepad mode: the toast's second
 * line and the hint under the Display tab's controllers row, one for each
 * shortcut and the menu button and grip for anything else.
 */
public class GamepadShortcutWordsTest {

    @Test
    public void theToastSaysTheWayBack() {
        assertEquals(R.string.vr_toast_gamepad_back_menu_grip,
                XrPanels.gamepadBackText(PAD_SHORTCUT_MENU_GRIP));
        assertEquals(R.string.vr_toast_gamepad_back_sticks,
                XrPanels.gamepadBackText(PAD_SHORTCUT_STICKS));
        assertEquals(R.string.vr_toast_gamepad_back_triggers_grips,
                XrPanels.gamepadBackText(PAD_SHORTCUT_TRIGGERS_GRIPS));
        assertEquals(R.string.vr_toast_gamepad_back_menu_grip, XrPanels.gamepadBackText(-1));
        assertEquals(R.string.vr_toast_gamepad_back_menu_grip, XrPanels.gamepadBackText(3));
    }

    @Test
    public void theDisplayTabNamesIt() {
        assertEquals(R.string.vr_panel_shortcut_sticks,
                XrPanels.controllersHint(PAD_SHORTCUT_STICKS));
        assertEquals(R.string.vr_panel_shortcut_triggers_grips,
                XrPanels.controllersHint(PAD_SHORTCUT_TRIGGERS_GRIPS));
        assertEquals(R.string.vr_panel_shortcut_menu_grip,
                XrPanels.controllersHint(PAD_SHORTCUT_MENU_GRIP));
        assertEquals(XrPanels.controllersHint(PAD_SHORTCUT_MENU_GRIP), XrPanels.controllersHint(9));
        assertNotEquals(XrPanels.controllersHint(PAD_SHORTCUT_STICKS),
                XrPanels.controllersHint(PAD_SHORTCUT_TRIGGERS_GRIPS));
    }
}
