package com.limelight.preferences;

import android.content.SharedPreferences;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** The pointer's own switches: their keys, where they start, and how the log lines name them. */
public class PointerPrefsTest {

    @Test
    public void thePadlockShowsUnlessSwitchedOff() {
        assertEquals("checkbox_vr_show_hand_lock",
                PreferenceConfiguration.VR_SHOW_HAND_LOCK_PREF_STRING);
        assertTrue(PreferenceConfiguration.DEFAULT_VR_SHOW_HAND_LOCK);

        FakePrefs prefs = new FakePrefs();
        assertTrue(PreferenceConfiguration.handLockIconShown(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_SHOW_HAND_LOCK_PREF_STRING, false);
        assertFalse(PreferenceConfiguration.handLockIconShown(prefs));
    }

    @Test
    public void theLogLinesNameThemTheWayTheyJoinTheRest() {
        assertEquals("handLockIcon=true", PreferenceConfiguration.inputLabel(true, "="));
        assertEquals("handLockIcon false", PreferenceConfiguration.inputLabel(false, " "));
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
