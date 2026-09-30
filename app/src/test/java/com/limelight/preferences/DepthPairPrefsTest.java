package com.limelight.preferences;

import android.content.SharedPreferences;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;

/** The separation and convergence each model starts on, and what a stored pair does to them. */
public class DepthPairPrefsTest {

    private static final String ZIPDEPTH = PreferenceConfiguration.VR_DEPTH_SOURCE_ZIPDEPTH;
    private static final String MIDAS = PreferenceConfiguration.VR_DEPTH_SOURCE_MIDAS;

    @Test
    public void eachModelHasItsOwnDefaults() {
        assertEquals(6, PreferenceConfiguration.defaultSeparation(ZIPDEPTH));
        assertEquals(50, PreferenceConfiguration.defaultConvergence(ZIPDEPTH));
        assertEquals(5, PreferenceConfiguration.defaultSeparation(MIDAS));
        assertEquals(50, PreferenceConfiguration.defaultConvergence(MIDAS));
    }

    // MiDaS keeps the pair every session started on before there were two
    @Test
    public void midasKeepsTheOldDefaults() {
        assertEquals(PreferenceConfiguration.DEFAULT_VR_SEPARATION,
                PreferenceConfiguration.defaultSeparation(MIDAS));
        assertEquals(PreferenceConfiguration.DEFAULT_VR_CONVERGENCE,
                PreferenceConfiguration.defaultConvergence(MIDAS));
    }

    // A test pattern or depth off runs at the default model's map, and its pair
    @Test
    public void anythingElseTakesZipDepthsPair() {
        for (String value : new String[] { "off", "shifttest", "flat", "", null }) {
            assertEquals(6, PreferenceConfiguration.defaultSeparation(value));
            assertEquals(50, PreferenceConfiguration.defaultConvergence(value));
        }
    }

    @Test
    public void aFreshInstallReadsTheRunningModelsPair() {
        FakePrefs prefs = new FakePrefs();
        assertEquals(6, PreferenceConfiguration.storedSeparation(prefs, ZIPDEPTH));
        assertEquals(50, PreferenceConfiguration.storedConvergence(prefs, ZIPDEPTH));
        assertEquals(5, PreferenceConfiguration.storedSeparation(prefs, MIDAS));
        assertEquals(50, PreferenceConfiguration.storedConvergence(prefs, MIDAS));
    }

    @Test
    public void aStoredPairIsNeverMovedByTheModel() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, 5);
        prefs.putInt(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING, 40);
        for (String model : new String[] { ZIPDEPTH, MIDAS }) {
            assertEquals(5, PreferenceConfiguration.storedSeparation(prefs, model));
            assertEquals(40, PreferenceConfiguration.storedConvergence(prefs, model));
        }
    }

    // Stored separately, so moving one leaves the other on the model's own
    @Test
    public void eachHalfIsItsOwnKey() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(PreferenceConfiguration.VR_SEPARATION_PREF_STRING, 9);
        assertEquals(9, PreferenceConfiguration.storedSeparation(prefs, MIDAS));
        assertEquals(50, PreferenceConfiguration.storedConvergence(prefs, MIDAS));

        prefs.remove(PreferenceConfiguration.VR_SEPARATION_PREF_STRING);
        prefs.putInt(PreferenceConfiguration.VR_CONVERGENCE_PREF_STRING, 65);
        assertEquals(6, PreferenceConfiguration.storedSeparation(prefs, ZIPDEPTH));
        assertEquals(65, PreferenceConfiguration.storedConvergence(prefs, ZIPDEPTH));
    }

    @Test
    public void theLogLinesGiveThePairSeparationFirst() {
        assertEquals("6/50", PreferenceConfiguration.defaultPairLabel(ZIPDEPTH));
        assertEquals("5/50", PreferenceConfiguration.defaultPairLabel(MIDAS));
    }

    // Just enough of SharedPreferences for the two keys
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
