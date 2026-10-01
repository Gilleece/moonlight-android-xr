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

    // An Xbox pad with the buttons two controllers have for an app, and
    // triggers that read how far in they are. No rumble: the controllers'
    // haptics are not wired up.
    public static final byte TYPE = MoonBridge.LI_CTYPE_XBOX;
    public static final int BUTTONS = XrShared.PAD_BUTTONS;
    public static final short CAPABILITIES = MoonBridge.LI_CCAP_ANALOG_TRIGGERS;

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
}
