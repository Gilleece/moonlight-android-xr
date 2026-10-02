package com.limelight.binding.input;

import com.limelight.binding.video.XrShared;
import com.limelight.nvstream.input.ControllerPacket;
import com.limelight.nvstream.jni.MoonBridge;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

/**
 * Gamepad mode's pad: the number it takes beside real pads, what it says it
 * is when it arrives, its button bits against the packet's own, and the slots
 * the frame hands it back in.
 */
public class XrPadTest {

    @Test
    public void itIsPlayerOneUnlessAPadIsThere() {
        assertEquals(0, XrPad.numberFor(0));
        // A real pad counted at the start or plugged in since
        assertEquals(1, XrPad.numberFor(0x1));
        assertEquals(2, XrPad.numberFor(0x3));
        // The first gap, not past the last pad
        assertEquals(1, XrPad.numberFor(0x5));
        assertEquals(0, XrPad.numberFor(0x6));
        assertEquals(15, XrPad.numberFor(0x7fff));
        assertEquals(-1, XrPad.numberFor(0xffff));
        // Only sixteen numbers, whatever is set above them
        assertEquals(-1, XrPad.numberFor(0x1ffff));
    }

    @Test
    public void itArrivesAsAnXboxPadWithAnalogueTriggers() {
        assertEquals(MoonBridge.LI_CTYPE_XBOX, XrPad.TYPE);
        assertEquals(MoonBridge.LI_CCAP_ANALOG_TRIGGERS, XrPad.CAPABILITIES);
        assertEquals(ControllerPacket.A_FLAG | ControllerPacket.B_FLAG | ControllerPacket.X_FLAG
                | ControllerPacket.Y_FLAG | ControllerPacket.LB_FLAG | ControllerPacket.RB_FLAG
                | ControllerPacket.LS_CLK_FLAG | ControllerPacket.RS_CLK_FLAG
                | ControllerPacket.PLAY_FLAG, XrPad.BUTTONS);
    }

    @Test
    public void itsBitsAreThePacketsOwn() {
        assertEquals(ControllerPacket.A_FLAG, XrShared.PAD_A);
        assertEquals(ControllerPacket.B_FLAG, XrShared.PAD_B);
        assertEquals(ControllerPacket.X_FLAG, XrShared.PAD_X);
        assertEquals(ControllerPacket.Y_FLAG, XrShared.PAD_Y);
        assertEquals(ControllerPacket.LB_FLAG, XrShared.PAD_LB);
        assertEquals(ControllerPacket.RB_FLAG, XrShared.PAD_RB);
        assertEquals(ControllerPacket.LS_CLK_FLAG, XrShared.PAD_LS_CLICK);
        assertEquals(ControllerPacket.RS_CLK_FLAG, XrShared.PAD_RS_CLICK);
        // The menu button is Start, as upstream maps Android's menu key
        assertEquals(ControllerPacket.PLAY_FLAG, XrShared.PAD_START);
    }

    @Test
    public void itComesBackInTheLastSlots() {
        assertEquals(XrShared.IN_MOUSE_DY + 1, XrShared.IN_PAD);
        assertEquals(XrShared.IN_PAD + 1, XrShared.IN_PAD_BUTTONS);
        assertEquals(XrShared.IN_PAD_BUTTONS + 1, XrShared.IN_PAD_LT);
        assertEquals(XrShared.IN_PAD_LT + 1, XrShared.IN_PAD_RT);
        assertEquals(XrShared.IN_PAD_RT + 1, XrShared.IN_PAD_LX);
        assertEquals(XrShared.IN_PAD_LX + 1, XrShared.IN_PAD_LY);
        assertEquals(XrShared.IN_PAD_LY + 1, XrShared.IN_PAD_RX);
        assertEquals(XrShared.IN_PAD_RX + 1, XrShared.IN_PAD_RY);
        // Only the hand lock hint's slot after them
        assertEquals(XrShared.IN_PAD_RY + 1, XrShared.IN_HINT);
        assertEquals(XrShared.IN_HINT + 1, XrShared.IN_SLOTS);
        // A stick's full tilt and the largest button bit survive the float
        // slots exactly
        assertEquals(32766, (int)(float)32766);
        assertEquals(XrShared.PAD_BUTTONS, (int)(float)XrShared.PAD_BUTTONS);
    }
}
