package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.XrShared;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * Head aim: its switch, its pixels a degree and its dead zone, their keys,
 * where they start and the lanes they are held to, and the slots the frame
 * hands its motion back in.
 */
public class HeadAimPrefsTest {

    @Test
    public void itIsOffUntilSwitchedOn() {
        assertEquals("checkbox_vr_head_aim", PreferenceConfiguration.VR_HEAD_AIM_PREF_STRING);
        assertFalse(PreferenceConfiguration.DEFAULT_VR_HEAD_AIM);

        FakePrefs prefs = new FakePrefs();
        assertFalse(PreferenceConfiguration.headAimOn(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_HEAD_AIM_PREF_STRING, true);
        assertTrue(PreferenceConfiguration.headAimOn(prefs));
        // The pointer's own switches are not touched by it
        assertTrue(PreferenceConfiguration.rayShown(prefs));
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
    }

    @Test
    public void itStartsAtEightPixelsADegreeAndTwoDegreesASecond() {
        assertEquals("seekbar_vr_head_aim_sensitivity",
                PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING);
        assertEquals("seekbar_vr_head_aim_deadzone",
                PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING);
        assertEquals(8, XrShared.HEAD_AIM_SENSITIVITY_DEFAULT);
        assertEquals(1, XrShared.HEAD_AIM_SENSITIVITY_MIN);
        assertEquals(30, XrShared.HEAD_AIM_SENSITIVITY_MAX);
        assertEquals(2, XrShared.HEAD_AIM_DEADZONE_DEFAULT);
        assertEquals(0, XrShared.HEAD_AIM_DEADZONE_MIN);
        assertEquals(20, XrShared.HEAD_AIM_DEADZONE_MAX);

        FakePrefs prefs = new FakePrefs();
        assertEquals(8, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(2, PreferenceConfiguration.headAimDeadZone(prefs));
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, 15);
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, 0);
        assertEquals(15, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(0, PreferenceConfiguration.headAimDeadZone(prefs));
    }

    @Test
    public void storedValuesAreHeldToTheirLanes() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, 0);
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, -3);
        assertEquals(1, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(0, PreferenceConfiguration.headAimDeadZone(prefs));
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, 99);
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, 21);
        assertEquals(30, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(20, PreferenceConfiguration.headAimDeadZone(prefs));
    }

    @Test
    public void theMotionComesBackInTheLastSlots() {
        assertEquals(XrShared.IN_REPORT_ZONE + 1, XrShared.IN_HEAD_AIM);
        assertEquals(XrShared.IN_HEAD_AIM + 1, XrShared.IN_MOUSE_DX);
        assertEquals(XrShared.IN_MOUSE_DX + 1, XrShared.IN_MOUSE_DY);
        assertEquals(XrShared.IN_MOUSE_DY + 1, XrShared.IN_SLOTS);
    }

    private static final class FakePrefs implements SharedPreferences, SharedPreferences.Editor {
        final Map<String, Object> values = new HashMap<>();

        @Override public Map<String, ?> getAll() { return values; }
        @Override public String getString(String key, String def) {
            return values.containsKey(key) ? (String)values.get(key) : def;
        }
        @Override public Set<String> getStringSet(String key, Set<String> def) { return def; }
        @Override public int getInt(String key, int def) {
            return values.containsKey(key) ? (Integer)values.get(key) : def;
        }
        @Override public long getLong(String key, long def) { return def; }
        @Override public float getFloat(String key, float def) { return def; }
        @Override public boolean getBoolean(String key, boolean def) {
            return values.containsKey(key) ? (Boolean)values.get(key) : def;
        }
        @Override public boolean contains(String key) { return values.containsKey(key); }
        @Override public Editor edit() { return this; }
        @Override public void registerOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }
        @Override public void unregisterOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }

        @Override public Editor putString(String key, String value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putStringSet(String key, Set<String> value) { return this; }
        @Override public Editor putInt(String key, int value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putLong(String key, long value) { return this; }
        @Override public Editor putFloat(String key, float value) { return this; }
        @Override public Editor putBoolean(String key, boolean value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor remove(String key) {
            values.remove(key);
            return this;
        }
        @Override public Editor clear() {
            values.clear();
            return this;
        }
        @Override public boolean commit() { return true; }
        @Override public void apply() { }
    }
}
