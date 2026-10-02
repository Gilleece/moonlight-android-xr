package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.XrShared;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;

/** The depth rate in maps a second, where each headset starts, and the one move from a cadence. */
public class DepthRatePrefsTest {

    @Test
    public void theStartingRatesAreWhatTheCadencesGave() {
        // Every third frame of 60 fps, and every sixth of a Gen 1 headset's 72
        assertEquals(20, PreferenceConfiguration.DEFAULT_VR_DEPTH_RATE);
        assertEquals(12, PreferenceConfiguration.GEN1_DEPTH_RATE);
        assertEquals(5, XrShared.DEPTH_RATE_MIN);
        assertEquals(45, XrShared.DEPTH_RATE_MAX);
    }

    @Test
    public void eachCadenceMovesToTheRateItGaveAt60() {
        int[] expected = { 45, 30, 20, 15, 12, 10 };
        for (int cadence = 1; cadence <= 6; cadence++) {
            assertEquals("cadence " + cadence, expected[cadence - 1],
                    PreferenceConfiguration.depthRateForCadence(cadence));
        }
        // Past the old slider's end, still in range
        assertEquals(9, PreferenceConfiguration.depthRateForCadence(7));
        assertEquals(5, PreferenceConfiguration.depthRateForCadence(20));
        assertEquals(45, PreferenceConfiguration.depthRateForCadence(0));
    }

    @Test
    public void aStoredCadenceMovesOnce() {
        FakePrefs prefs = new FakePrefs();
        prefs.values.put("seekbar_vr_inference_cadence", 3);
        assertEquals(3, PreferenceConfiguration.moveCadenceToDepthRate(prefs));
        assertEquals(20, prefs.values.get("seekbar_vr_inference_cadence"));
        assertEquals(true, prefs.values.get("depth_rate_per_second"));

        // The rate set afterwards is a rate, and stays as set
        prefs.values.put("seekbar_vr_inference_cadence", 6);
        assertEquals(0, PreferenceConfiguration.moveCadenceToDepthRate(prefs));
        assertEquals(6, prefs.values.get("seekbar_vr_inference_cadence"));
    }

    @Test
    public void theGen1SeedMovesToItsOwnRate() {
        FakePrefs prefs = new FakePrefs();
        prefs.values.put("seekbar_vr_inference_cadence", 6);
        assertEquals(6, PreferenceConfiguration.moveCadenceToDepthRate(prefs));
        assertEquals(10, prefs.values.get("seekbar_vr_inference_cadence"));
    }

    // Nothing stored: only the marker, so the Gen 1 seed or the xml default
    // that follows writes a rate that is never taken for a cadence
    @Test
    public void aFreshInstallOnlyGetsTheMarker() {
        FakePrefs prefs = new FakePrefs();
        assertEquals(0, PreferenceConfiguration.moveCadenceToDepthRate(prefs));
        assertNull(prefs.values.get("seekbar_vr_inference_cadence"));
        assertEquals(false, prefs.values.get("depth_rate_per_second"));
        prefs.values.put("seekbar_vr_inference_cadence", 12);
        assertEquals(0, PreferenceConfiguration.moveCadenceToDepthRate(prefs));
        assertEquals(12, prefs.values.get("seekbar_vr_inference_cadence"));
    }

    @Test
    public void theStoredRateIsReadInRange() {
        FakePrefs prefs = new FakePrefs();
        assertEquals(20, PreferenceConfiguration.storedDepthRate(prefs, false));
        assertEquals(12, PreferenceConfiguration.storedDepthRate(prefs, true));
        prefs.values.put("seekbar_vr_inference_cadence", 30);
        assertEquals(30, PreferenceConfiguration.storedDepthRate(prefs, false));
        assertEquals(30, PreferenceConfiguration.storedDepthRate(prefs, true));
        prefs.values.put("seekbar_vr_inference_cadence", 2);
        assertEquals(5, PreferenceConfiguration.storedDepthRate(prefs, false));
        prefs.values.put("seekbar_vr_inference_cadence", 90);
        assertEquals(45, PreferenceConfiguration.storedDepthRate(prefs, false));
    }

    @Test
    public void unmovedIsNotMarkedMoved() {
        FakePrefs prefs = new FakePrefs();
        prefs.values.put("seekbar_vr_inference_cadence", 0);
        assertEquals(0, PreferenceConfiguration.moveCadenceToDepthRate(prefs));
        assertFalse((Boolean)prefs.values.get("depth_rate_per_second"));
    }

    // Just enough of SharedPreferences for the move, applied as it is edited
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
