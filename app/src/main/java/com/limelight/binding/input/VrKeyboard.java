package com.limelight.binding.input;

import com.limelight.binding.video.XrShared;
import com.limelight.nvstream.input.KeyboardPacket;

/**
 * Turns presses on the in world keyboard into host key events. Letters,
 * digits, the control keys and the keys that type nothing go as virtual key
 * codes, capitals inside a held shift, and other characters as text. Ctrl,
 * Alt and Win are held down on the host for as long as they are lit on the
 * keyboard, so a click or a scroll lands with them as well, and anything typed
 * meanwhile also carries them in its modifier byte for hosts that read that
 * instead.
 */
public final class VrKeyboard {

    /** Where the events go: the connection, or a log. */
    public interface Sink {
        void key(short keyCode, byte action, byte modifiers);
        void text(String text);
    }

    static final short VK_SHIFT = 0x10;
    static final short VK_LCONTROL = 0xA2;
    static final short VK_LMENU = 0xA4;
    static final short VK_LWIN = 0x5B;

    // In the order they go down, and come back up the other way
    private static final int[] MOD_BITS = { XrShared.KB_MOD_CTRL, XrShared.KB_MOD_ALT,
                                            XrShared.KB_MOD_WIN };
    private static final short[] MOD_KEYS = { VK_LCONTROL, VK_LMENU, VK_LWIN };

    private final Sink sink;
    // The modifiers the host has been told are down
    private int held;

    public VrKeyboard(Sink sink) {
        this.sink = sink;
    }

    /** The modifiers the host has been told are down, KB_MOD_ bits. */
    public synchronized int held() {
        return held;
    }

    /**
     * A key typed on the keyboard, with the modifiers lit as it was pressed.
     * They are put down first if the host has not been told yet, and left
     * down: the keyboard says when they go out.
     */
    public synchronized void type(int code, int mods) {
        hold(held | mods);
        byte modifiers = (byte)mods;
        if (code >= XrShared.KB_CODE_VK) {
            press((short)(code - XrShared.KB_CODE_VK), modifiers);
        }
        else if (code == 8 || code == 9 || code == 13 || code == 32
                || (code >= '0' && code <= '9')) {
            press((short)code, modifiers);
        }
        else if (code >= 'a' && code <= 'z') {
            press((short)(code - 32), modifiers);
        }
        else if (code >= 'A' && code <= 'Z') {
            // Shift is held around the letter and named in the modifier as
            // well, so hosts that read either one see the capital
            sink.key(VK_SHIFT, KeyboardPacket.KEY_DOWN, modifiers);
            press((short)code, (byte)(modifiers | KeyboardPacket.MODIFIER_SHIFT));
            sink.key(VK_SHIFT, KeyboardPacket.KEY_UP, modifiers);
        }
        else if (mods != 0 && punctuationKey(code) != 0) {
            // Text cannot carry a modifier, so Ctrl and the minus key go as
            // the key itself
            press(punctuationKey(code), modifiers);
        }
        else {
            sink.text(String.valueOf((char)code));
        }
    }

    /**
     * Ctrl, Alt and Win as lit on the keyboard now. Whatever has just lit goes
     * down on the host and whatever has gone out comes back up.
     */
    public synchronized void hold(int mods) {
        for (int i = 0; i < MOD_BITS.length; i++) {
            int bit = MOD_BITS[i];
            if ((mods & bit) != 0 && (held & bit) == 0) {
                held |= bit;
                sink.key(MOD_KEYS[i], KeyboardPacket.KEY_DOWN, (byte)held);
            }
        }
        for (int i = MOD_BITS.length - 1; i >= 0; i--) {
            int bit = MOD_BITS[i];
            if ((mods & bit) == 0 && (held & bit) != 0) {
                held &= ~bit;
                sink.key(MOD_KEYS[i], KeyboardPacket.KEY_UP, (byte)held);
            }
        }
    }

    /** Lets go of anything still held, as the stream ends. */
    public synchronized void releaseAll() {
        hold(0);
    }

    private void press(short keyCode, byte modifiers) {
        sink.key(keyCode, KeyboardPacket.KEY_DOWN, modifiers);
        sink.key(keyCode, KeyboardPacket.KEY_UP, modifiers);
    }

    /**
     * The US layout's virtual key for the punctuation that has a key of its
     * own on it, or 0. The shifted ones would need a shift as well, and go as
     * text.
     */
    static short punctuationKey(int code) {
        switch (code) {
            case ';': return 0xBA;
            case '=': return 0xBB;
            case ',': return 0xBC;
            case '-': return 0xBD;
            case '.': return 0xBE;
            case '/': return 0xBF;
            case '`': return 0xC0;
            case '[': return 0xDB;
            case '\\': return 0xDC;
            case ']': return 0xDD;
            case '\'': return 0xDE;
            default: return 0;
        }
    }
}
