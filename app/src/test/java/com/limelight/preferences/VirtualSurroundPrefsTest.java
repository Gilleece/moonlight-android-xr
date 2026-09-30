package com.limelight.preferences;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;

/** The virtual surround switch: off unless asked for. */
public class VirtualSurroundPrefsTest {

    @Test
    public void theDefaultIsOff() {
        assertFalse(PreferenceConfiguration.DEFAULT_VR_VIRTUAL_SURROUND);
        assertEquals("checkbox_vr_virtual_surround",
                PreferenceConfiguration.VR_VIRTUAL_SURROUND_PREF_STRING);
    }
}
