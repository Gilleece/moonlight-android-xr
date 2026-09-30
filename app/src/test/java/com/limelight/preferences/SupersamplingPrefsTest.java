package com.limelight.preferences;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

/** The compositor supersampling choice: off unless asked for, and what each stored value becomes. */
public class SupersamplingPrefsTest {

    @Test
    public void theDefaultIsOff() {
        assertEquals("off", PreferenceConfiguration.DEFAULT_VR_SUPERSAMPLING);
        assertEquals(0, PreferenceConfiguration.supersamplingMode(
                PreferenceConfiguration.DEFAULT_VR_SUPERSAMPLING));
    }

    @Test
    public void eachStoredValueMapsToItsMode() {
        assertEquals(0, PreferenceConfiguration.supersamplingMode("off"));
        assertEquals(1, PreferenceConfiguration.supersamplingMode("normal"));
        assertEquals(2, PreferenceConfiguration.supersamplingMode("quality"));
    }

    @Test
    public void anythingElseIsOff() {
        assertEquals(0, PreferenceConfiguration.supersamplingMode(null));
        assertEquals(0, PreferenceConfiguration.supersamplingMode(""));
        assertEquals(0, PreferenceConfiguration.supersamplingMode("Quality"));
        assertEquals(0, PreferenceConfiguration.supersamplingMode("2"));
    }
}
