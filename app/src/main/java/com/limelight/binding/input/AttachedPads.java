package com.limelight.binding.input;

import java.util.List;

/**
 * The gamepad mask a stream starts with: a bit for each pad attached, counted
 * in order as upstream counts them (input devices with joystick axes, then
 * pads the USB driver will claim, with the on-screen controls holding bit 0),
 * less the input devices a Quest makes of its own Touch controllers and
 * buttons. Those carry a full set of gamepad axes under Meta's vendor id but
 * are the headset itself, so counting them took players 1 and 2 on the host
 * and pushed the VR controllers' pad to player 3.
 *
 * Pure arithmetic over what the caller read off the devices, so it runs on a
 * desktop.
 */
public final class AttachedPads {

    // Meta's USB vendor id, which only its headsets and their controllers use
    static final int VENDOR_META = 0x2833;

    /** One input device as the count sees it. */
    public static final class Device {
        final int vendorId;
        final boolean joystickAxes;

        public Device(int vendorId, boolean joystickAxes) {
            this.vendorId = vendorId;
            this.joystickAxes = joystickAxes;
        }
    }

    private AttachedPads() {
    }

    /** Whether an input device is the headset's own rather than a pad someone attached. */
    public static boolean headsetOwn(int vendorId) {
        return vendorId == VENDOR_META;
    }

    /** Whether an input device counts as a pad attached. */
    public static boolean counts(Device device) {
        return device.joystickAxes && !headsetOwn(device.vendorId);
    }

    /**
     * The mask: one bit per counted input device, then one per USB pad, from
     * bit 0 up, and bit 0 set for the on-screen controls.
     */
    public static short mask(List<Device> inputDevices, int usbPads, boolean onscreen) {
        int count = 0;
        int mask = 0;
        for (Device device : inputDevices) {
            if (counts(device)) {
                mask |= 1 << count++;
            }
        }
        for (int i = 0; i < usbPads; i++) {
            mask |= 1 << count++;
        }
        if (onscreen) {
            mask |= 1;
        }
        return (short)mask;
    }
}
