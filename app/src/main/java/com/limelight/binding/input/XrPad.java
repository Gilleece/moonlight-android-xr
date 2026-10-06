package com.limelight.binding.input;

import com.limelight.binding.video.XrShared;
import com.limelight.nvstream.jni.MoonBridge;

/**
 * Gamepad mode's pad, the VR session's two controllers as one Xbox pad on the
 * host: which number it takes and what it says it is when it arrives.
 *
 * Pure arithmetic over gamepad masks, so it runs on a desktop.
 */
public final class XrPad {
    // As many pads as the mask has bits, as ControllerHandler counts them
    static final int MAX_PADS = 16;

    // An Xbox pad with the buttons two controllers have for an app, triggers
    // that read how far in they are, and rumble, the low motor on the left
    // controller and the high on the right
    public static final byte TYPE = MoonBridge.LI_CTYPE_XBOX;
    public static final int BUTTONS = XrShared.PAD_BUTTONS;
    public static final short CAPABILITIES =
            MoonBridge.LI_CCAP_ANALOG_TRIGGERS | MoonBridge.LI_CCAP_RUMBLE;

    private XrPad() {
    }

    /**
     * The number the pad takes: player 1 unless a pad already has it, then
     * the first one free. Taken is every number in use or counted at the
     * start of the stream. -1 with all sixteen taken.
     */
    public static int numberFor(int taken) {
        for (int i = 0; i < MAX_PADS; i++) {
            if ((taken & (1 << i)) == 0) {
                return i;
            }
        }
        return -1;
    }

    /**
     * The host's rumble as one word for the frame loop to pick up: how many
     * words have come so far over the two 16 bit motors, so the same values
     * twice still read as a new word.
     */
    public static long rumbleWord(long count, int lowMotor, int highMotor) {
        return (count << 32) | ((lowMotor & 0xffffL) << 16) | (highMotor & 0xffffL);
    }

    /** The low frequency motor out of a word, 0 to 65535. */
    public static int rumbleLow(long word) {
        return (int)((word >>> 16) & 0xffff);
    }

    /** The high frequency motor out of a word, 0 to 65535. */
    public static int rumbleHigh(long word) {
        return (int)(word & 0xffff);
    }
}
