package com.limelight.binding.input;

import org.junit.Test;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * The pads a stream starts with and the number the VR controllers' pad takes
 * beside them, over device lists as a Quest 3 lists them: its Touch
 * controllers and headset buttons as two input devices with gamepad axes
 * under Meta's vendor id, and whatever real pads are attached.
 */
public class AttachedPadsTest {

    private static final int META = 0x2833;
    private static final int MICROSOFT = 0x045e;
    private static final int SONY = 0x054c;

    private static AttachedPads.Device pad(int vendor) {
        return new AttachedPads.Device(vendor, true);
    }

    // Not a pad at all: a keyboard, the volume keys, a headset jack
    private static AttachedPads.Device other(int vendor) {
        return new AttachedPads.Device(vendor, false);
    }

    // The two devices the Quest 3 lists of its own, as it lists them
    private static List<AttachedPads.Device> quest(AttachedPads.Device... more) {
        List<AttachedPads.Device> list = new ArrayList<>(Arrays.asList(pad(META), other(0),
                other(0), pad(META)));
        list.addAll(Arrays.asList(more));
        return list;
    }

    @Test
    public void theHeadsetsOwnDevicesAreNoPads() {
        assertEquals(0x2833, AttachedPads.VENDOR_META);
        assertTrue(AttachedPads.headsetOwn(META));
        assertFalse(AttachedPads.headsetOwn(MICROSOFT));
        assertFalse(AttachedPads.counts(pad(META)));
        assertTrue(AttachedPads.counts(pad(MICROSOFT)));
        assertFalse(AttachedPads.counts(other(MICROSOFT)));
    }

    @Test
    public void aQuestAloneStartsWithNoPadsAndTheVrPadIsPlayerOne() {
        short mask = AttachedPads.mask(quest(), 0, false);
        assertEquals(0, mask);
        // As attachXrPad takes it: nothing in use yet
        int number = XrPad.numberFor(mask);
        assertEquals(0, number);
        assertEquals(0x0001, mask | (1 << number));
    }

    @Test
    public void beforeTheFixTheTwoOwnDevicesTookPlayersOneAndTwo() {
        // What upstream's count made of the same list, every device with
        // joystick axes a pad: the VR pad came out as controller 2, mask 0x7
        int upstream = 0;
        int count = 0;
        for (AttachedPads.Device d : quest()) {
            if (d.joystickAxes) {
                upstream |= 1 << count++;
            }
        }
        assertEquals(0x3, upstream);
        assertEquals(2, XrPad.numberFor(upstream));
    }

    @Test
    public void aRealPadAndTheVrPadArePlayersOneAndTwo() {
        short initial = AttachedPads.mask(quest(pad(MICROSOFT)), 0, false);
        assertEquals(0x1, initial);

        // The VR pad first: it steps round the real one counted at the start
        int vr = XrPad.numberFor(initial);
        assertEquals(1, vr);
        int current = 1 << vr;
        // Then the real pad's first input reserves the lowest number free of
        // those reserved, as assignControllerNumberIfNeeded does
        int real = XrPad.numberFor(current);
        assertEquals(0, real);
        current |= 1 << real;
        assertEquals(0x3, current | (initial & ~(1 << real)));

        // The real pad first, then the VR pad
        current = 1 << XrPad.numberFor(0);
        int initialLeft = initial & ~current;
        assertEquals(1, XrPad.numberFor(current | initialLeft));
    }

    @Test
    public void twoRealPadsLeaveTheVrPadPlayerThree() {
        short initial = AttachedPads.mask(quest(pad(MICROSOFT), other(MICROSOFT), pad(SONY)), 0,
                false);
        assertEquals(0x3, initial);
        assertEquals(2, XrPad.numberFor(initial));
    }

    @Test
    public void usbPadsAndTheOnScreenControlsCountAsUpstreamCountsThem() {
        // USB pads after the input devices, the on-screen controls on bit 0
        assertEquals(0x7, AttachedPads.mask(quest(pad(SONY)), 2, false));
        assertEquals(0x3, AttachedPads.mask(quest(), 2, false));
        assertEquals(0x1, AttachedPads.mask(quest(), 0, true));
        assertEquals(0x1, AttachedPads.mask(quest(pad(MICROSOFT)), 0, true));
        assertEquals(0x1, AttachedPads.mask(quest(), 1, true));
        // Off a headset the count is upstream's own
        List<AttachedPads.Device> phone = Arrays.asList(other(0), pad(MICROSOFT), pad(SONY));
        assertEquals(0x3, AttachedPads.mask(phone, 0, false));
        assertEquals(0, AttachedPads.mask(new ArrayList<AttachedPads.Device>(), 0, false));
    }
}
