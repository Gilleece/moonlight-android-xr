package com.limelight.binding.video;

import com.limelight.utils.BugReport;

import static com.limelight.binding.video.XrShared.*;

/**
 * What the report sheet's two fields hold while it is up, as the in world
 * keyboard fills them: the note, the address to answer, which of the two has
 * the keys, and whether Send can go. The keyboard's codes arrive as they
 * would for the host, and only the ones that mean something in a text field
 * do anything here. Frame loop only.
 */
final class XrReportForm {
    // Far more than anyone types with a pointer, and the collector cuts the
    // copy it mails in clear at 2000 anyway
    static final int NOTE_MAX = 2000;
    static final int ADDRESS_MAX = 254;

    private final StringBuilder note = new StringBuilder();
    private final StringBuilder address = new StringBuilder();
    private int focus = REPORT_ZONE_NOTE;

    /** A fresh note, the address remembered from the last report, and the keys on the note. */
    void open(String rememberedAddress) {
        note.setLength(0);
        address.setLength(0);
        if (rememberedAddress != null) {
            address.append(rememberedAddress.trim());
        }
        focus = REPORT_ZONE_NOTE;
    }

    /** The field a press landed on takes the keys. */
    void focus(int zone) {
        if (zone == REPORT_ZONE_NOTE || zone == REPORT_ZONE_EMAIL) {
            focus = zone;
        }
    }

    /**
     * One key off the keyboard into the field with the keys: backspace takes
     * the last character, tab moves to the other field, enter starts a new
     * line in the note and goes back to the note from the address, and
     * anything printable goes on the end. The keys that only mean something
     * to a PC do nothing. Says whether anything changed.
     */
    boolean type(int code) {
        StringBuilder into = focus == REPORT_ZONE_NOTE ? note : address;
        int max = focus == REPORT_ZONE_NOTE ? NOTE_MAX : ADDRESS_MAX;
        if (code == 8) {
            if (into.length() == 0) {
                return false;
            }
            into.setLength(into.length() - 1);
            return true;
        }
        if (code == 9) {
            focus = focus == REPORT_ZONE_NOTE ? REPORT_ZONE_EMAIL : REPORT_ZONE_NOTE;
            return true;
        }
        if (code == 13) {
            if (focus == REPORT_ZONE_EMAIL) {
                focus = REPORT_ZONE_NOTE;
                return true;
            }
            return append(into, '\n', max);
        }
        if (code >= 32 && code < KB_CODE_VK && code != 127) {
            return append(into, (char) code, max);
        }
        return false;
    }

    private static boolean append(StringBuilder into, char c, int max) {
        if (into.length() >= max) {
            return false;
        }
        into.append(c);
        return true;
    }

    String note() {
        return note.toString();
    }

    String address() {
        return address.toString();
    }

    int focus() {
        return focus;
    }

    /** A note with something in it, and an address that could be answered or none. */
    boolean canSend() {
        return BugReport.canSend(note.toString(), address.toString());
    }

    /** Something typed in the address that is not one, which the sheet says under it. */
    boolean addressWrong() {
        return !BugReport.addressOk(address.toString());
    }
}
