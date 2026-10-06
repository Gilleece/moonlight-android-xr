package com.limelight.binding.video;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * The report sheet's fields as the in world keyboard fills them: where keys
 * land, what the control keys do, and when Send can go.
 */
public class XrReportFormTest {

    private static void type(XrReportForm form, String text) {
        for (int i = 0; i < text.length(); i++) {
            form.type(text.charAt(i));
        }
    }

    @Test
    public void eachOpeningStartsAFreshNoteWithTheAddressRemembered() {
        XrReportForm form = new XrReportForm();
        form.open(null);
        type(form, "old note");
        form.open(" me@example.com ");
        assertEquals("", form.note());
        assertEquals("me@example.com", form.address());
        assertEquals(REPORT_ZONE_NOTE, form.focus());
    }

    @Test
    public void keysGoIntoTheFieldThatHasThem() {
        XrReportForm form = new XrReportForm();
        form.open("");
        type(form, "Black screen");
        form.focus(REPORT_ZONE_EMAIL);
        type(form, "a@b.co");
        assertEquals("Black screen", form.note());
        assertEquals("a@b.co", form.address());
        // A press on something that is not a field leaves the keys where they are
        form.focus(REPORT_ZONE_SEND);
        assertEquals(REPORT_ZONE_EMAIL, form.focus());
    }

    @Test
    public void theControlKeysEditAndMove() {
        XrReportForm form = new XrReportForm();
        form.open("");
        type(form, "abc");
        assertTrue(form.type(8));
        assertEquals("ab", form.note());
        // Tab moves to the address and back
        assertTrue(form.type(9));
        assertEquals(REPORT_ZONE_EMAIL, form.focus());
        assertFalse(form.type(8));
        // Enter in the address goes back to the note, where it starts a line
        assertTrue(form.type(13));
        assertEquals(REPORT_ZONE_NOTE, form.focus());
        assertTrue(form.type(13));
        type(form, "d");
        assertEquals("ab\nd", form.note());
        assertTrue(form.type(9));
        assertTrue(form.type(9));
        assertEquals(REPORT_ZONE_NOTE, form.focus());
    }

    @Test
    public void keysThatOnlyMeanSomethingToAPcDoNothing() {
        XrReportForm form = new XrReportForm();
        form.open("");
        type(form, "x");
        // Esc, F1, an arrow and Delete from the Fn sheet
        assertFalse(form.type(KB_CODE_VK + 0x1B));
        assertFalse(form.type(KB_CODE_VK + 0x70));
        assertFalse(form.type(KB_CODE_VK + 0x25));
        assertFalse(form.type(KB_CODE_VK + 0x2E));
        assertFalse(form.type(127));
        assertFalse(form.type(0));
        assertEquals("x", form.note());
    }

    @Test
    public void sendWaitsForANoteAndAnAddressThatWillDo() {
        XrReportForm form = new XrReportForm();
        form.open("");
        assertFalse(form.canSend());
        type(form, "   ");
        assertFalse(form.canSend());
        type(form, "it froze");
        assertTrue(form.canSend());
        assertFalse(form.addressWrong());

        form.focus(REPORT_ZONE_EMAIL);
        type(form, "me@home");
        assertTrue(form.addressWrong());
        assertFalse(form.canSend());
        type(form, ".net");
        assertFalse(form.addressWrong());
        assertTrue(form.canSend());
    }

    @Test
    public void theFieldsStopAtTheirLength() {
        XrReportForm form = new XrReportForm();
        form.open("");
        for (int i = 0; i < XrReportForm.NOTE_MAX; i++) {
            assertTrue(form.type('a'));
        }
        assertFalse(form.type('a'));
        assertFalse(form.type(13));
        assertEquals(XrReportForm.NOTE_MAX, form.note().length());
        form.focus(REPORT_ZONE_EMAIL);
        for (int i = 0; i < XrReportForm.ADDRESS_MAX; i++) {
            form.type('b');
        }
        assertFalse(form.type('b'));
        assertEquals(XrReportForm.ADDRESS_MAX, form.address().length());
    }

    @Test
    public void theReportSlotsFollowTheKeyboards() {
        assertEquals(IN_KB_SHEET + 1, IN_REPORT);
        assertEquals(IN_REPORT + 1, IN_REPORT_ZONE);
        assertEquals(IN_REPORT_ZONE + 1, IN_HEAD_AIM);
        // The opening is told apart from every part a press can land on
        assertTrue(REPORT_OPENED > REPORT_ZONE_SEND);
        assertTrue(REPORT_ZONE_NOTE != REPORT_ZONE_EMAIL);
        assertEquals(COG_TAB_COUNT, COG_ART_ROOM);
        assertEquals(COG_ART_ROOM + COG_TAB_ABOUT + 1, COG_ART_ROOM_ABOUT);
    }
}
