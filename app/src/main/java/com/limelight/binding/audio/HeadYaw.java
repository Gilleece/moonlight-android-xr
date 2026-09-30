package com.limelight.binding.audio;

/**
 * Where the viewer's head points against the screen, for the virtual
 * surround. Read on the audio thread once a block, so it has to be cheap.
 */
public interface HeadYaw {
    /**
     * Radians, positive with the head turned left of the screen. 0 without a
     * VR session, or with the screen locked to the head.
     */
    float headYawRadians();
}
